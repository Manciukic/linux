// SPDX-License-Identifier: GPL-2.0
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <syscall.h>
#include "debug.h"
#include <asm/bug.h>
#include <linux/zalloc.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <pthread.h>
#include <internal/lib.h>
#include "threadpool.h"

#ifndef HAVE_GETTID
static inline pid_t gettid(void)
{
	return (pid_t)syscall(__NR_gettid);
}
#endif

struct threadpool {
	int			nr_threads;	/* number of threads in the pool */
	struct threadpool_entry	*threads;	/* array of threads in the pool */
	struct task_struct	*current_task;	/* current executing function */
};

struct threadpool_entry {
	int				idx;	/* idx of thread in pool->threads */
	pid_t				tid;	/* tid of thread */
	pthread_t			ptid;   /* pthread id */
	struct threadpool		*pool;	/* parent threadpool */
	struct {
		int ack[2];			/* messages from thread (acks) */
		int cmd[2];			/* messages to thread (commands) */
	} pipes;
	bool				running; /* has this thread been started? */
};

enum threadpool_msg {
	THREADPOOL_MSG__UNDEFINED = 0,
	THREADPOOL_MSG__ACK,		/* from th: create and exit ack */
	THREADPOOL_MSG__WAKE,		/* to th: wake up */
	THREADPOOL_MSG__STOP,		/* to th: exit */
	THREADPOOL_MSG__MAX
};

static const char * const threadpool_msg_tags[] = {
	"undefined",
	"ack",
	"wake",
	"stop"
};

static const char * const threadpool_errno_str[] = {
	"Error calling sigprocmask",
	"Error receiving message from thread",
	"Error sending message to thread",
	"Thread sent unexpected message"
};

/**
 * threadpool_entry__init_pipes - initialize all pipes of @thread
 */
static void threadpool_entry__init_pipes(struct threadpool_entry *thread)
{
	thread->pipes.ack[0] = -1;
	thread->pipes.ack[1] = -1;
	thread->pipes.cmd[0] = -1;
	thread->pipes.cmd[1] = -1;
}

/**
 * threadpool_entry__open_pipes - open all pipes of @thread
 *
 * Caller should perform cleanup on error.
 */
static int threadpool_entry__open_pipes(struct threadpool_entry *thread)
{
	char sbuf[STRERR_BUFSIZE];

	if (pipe(thread->pipes.ack)) {
		pr_debug2("threadpool: failed to create comm pipe 'from': %s\n",
			str_error_r(errno, sbuf, sizeof(sbuf)));
		return -ENOMEM;
	}

	if (pipe(thread->pipes.cmd)) {
		pr_debug2("threadpool: failed to create comm pipe 'to': %s\n",
			str_error_r(errno, sbuf, sizeof(sbuf)));
		return -ENOMEM;
	}

	return 0;
}

/**
 * threadpool_entry__close_pipes - close all communication pipes of @thread
 */
static void threadpool_entry__close_pipes(struct threadpool_entry *thread)
{
	if (thread->pipes.ack[0] != -1) {
		close(thread->pipes.ack[0]);
		thread->pipes.ack[0] = -1;
	}
	if (thread->pipes.ack[1] != -1) {
		close(thread->pipes.ack[1]);
		thread->pipes.ack[1] = -1;
	}
	if (thread->pipes.cmd[0] != -1) {
		close(thread->pipes.cmd[0]);
		thread->pipes.cmd[0] = -1;
	}
	if (thread->pipes.cmd[1] != -1) {
		close(thread->pipes.cmd[1]);
		thread->pipes.cmd[1] = -1;
	}
}

/**
 * threadpool__send_cmd - send @cmd to @thread
 */
static int threadpool__send_cmd(struct threadpool *pool, int tidx, enum threadpool_msg cmd)
{
	struct threadpool_entry *thread = &pool->threads[tidx];
	char sbuf[STRERR_BUFSIZE];
	int res = writen(thread->pipes.cmd[1], &cmd, sizeof(cmd));

	if (res < 0) {
		pr_debug2("threadpool: error sending %s msg to tid=%d: %s\n",
			threadpool_msg_tags[cmd], thread->tid,
			str_error_r(errno, sbuf, sizeof(sbuf)));
		return -THREADPOOL_ERROR__WRITEPIPE;
	}

	pr_debug2("threadpool: sent %s msg to tid=%d\n", threadpool_msg_tags[cmd], thread->tid);
	return 0;
}

/**
 * threadpool__wait_thread - receive ack from thread
 *
 * NB: call only from main thread!
 */
static int threadpool__wait_thread(struct threadpool *pool, int tidx)
{
	int res;
	char sbuf[STRERR_BUFSIZE];
	struct threadpool_entry *thread = &pool->threads[tidx];
	enum threadpool_msg msg = THREADPOOL_MSG__UNDEFINED;

	res = readn(thread->pipes.ack[0], &msg, sizeof(msg));
	if (res < 0) {
		pr_debug2("threadpool: failed to recv msg from tid=%d: %s\n",
		       thread->tid, str_error_r(errno, sbuf, sizeof(sbuf)));
		return -THREADPOOL_ERROR__READPIPE;
	}
	if (msg != THREADPOOL_MSG__ACK) {
		pr_debug2("threadpool: received unexpected msg from tid=%d: %s\n",
		       thread->tid, threadpool_msg_tags[msg]);
		return -THREADPOOL_ERROR__INVALIDMSG;
	}

	pr_debug2("threadpool: received ack from tid=%d\n", thread->tid);

	return 0;
}

/**
 * threadpool__terminate_thread - send stop signal to thread and wait for ack
 *
 * NB: call only from main thread!
 */
static int threadpool__terminate_thread(struct threadpool *pool, int tidx)
{
	struct threadpool_entry *thread = &pool->threads[tidx];
	int err;

	if (!thread->running)
		return 0;

	err = threadpool__send_cmd(pool, tidx, THREADPOOL_MSG__STOP);
	if (err)
		goto out_cancel;

	err = threadpool__wait_thread(pool, tidx);
	if (err)
		goto out_cancel;

	thread->running = false;
out:
	return err;

out_cancel:
	pthread_cancel(thread->ptid);
	goto out;
}

/**
 * threadpool_entry__send_ack - send ack to main thread
 */
static int threadpool_entry__send_ack(struct threadpool_entry *thread)
{
	enum threadpool_msg msg = THREADPOOL_MSG__ACK;
	char sbuf[STRERR_BUFSIZE];
	int ret = writen(thread->pipes.ack[1], &msg, sizeof(msg));

	if (ret < 0) {
		pr_debug("threadpool[%d]: failed to send ack: %s\n",
			thread->tid, str_error_r(errno, sbuf, sizeof(sbuf)));
		return -THREADPOOL_ERROR__WRITEPIPE;
	}

	return 0;
}

/**
 * threadpool_entry__recv_cmd - receive command from main thread
 */
static int threadpool_entry__recv_cmd(struct threadpool_entry *thread,
					enum threadpool_msg *cmd)
{
	char sbuf[STRERR_BUFSIZE];
	int ret;

	*cmd = THREADPOOL_MSG__UNDEFINED;
	ret = readn(thread->pipes.cmd[0], cmd, sizeof(*cmd));
	if (ret < 0) {
		pr_debug("threadpool[%d]: error receiving command: %s\n",
			thread->tid, str_error_r(errno, sbuf, sizeof(sbuf)));
		return -THREADPOOL_ERROR__READPIPE;
	}

	if (*cmd != THREADPOOL_MSG__WAKE && *cmd != THREADPOOL_MSG__STOP) {
		pr_debug("threadpool[%d]: received unexpected command: %s\n",
			thread->tid, threadpool_msg_tags[*cmd]);
		return -THREADPOOL_ERROR__INVALIDMSG;
	}

	return 0;
}

/**
 * threadpool_entry__function - function running on thread
 *
 * This function waits for a signal from main thread to start executing
 * a task.
 * On completion, it will go back to sleep, waiting for another signal.
 * Signals are delivered through pipes.
 */
static void *threadpool_entry__function(void *args)
{
	struct threadpool_entry *thread = (struct threadpool_entry *) args;
	enum threadpool_msg cmd;

	thread->tid = gettid();

	pr_debug2("threadpool[%d]: started\n", thread->tid);

	for (;;) {
		if (threadpool_entry__send_ack(thread))
			break;

		if (threadpool_entry__recv_cmd(thread, &cmd))
			break;

		if (cmd == THREADPOOL_MSG__STOP)
			break;
	}

	pr_debug2("threadpool[%d]: exit\n", thread->tid);

	threadpool_entry__send_ack(thread);

	return NULL;
}

/**
 * threadpool__new - create a fixed threadpool with @n_threads threads
 */
struct threadpool *threadpool__new(int n_threads)
{
	int ret, err, t;
	char sbuf[STRERR_BUFSIZE];
	struct threadpool *pool = malloc(sizeof(*pool));

	if (!pool) {
		pr_debug2("threadpool: cannot allocate pool: %s\n",
			str_error_r(errno, sbuf, sizeof(sbuf)));
		err = -ENOMEM;
		goto out_return;
	}

	if (n_threads <= 0) {
		pr_debug2("threadpool: invalid number of threads: %d\n",
			n_threads);
		err = -EINVAL;
		goto out_free_pool;
	}

	pool->nr_threads = n_threads;
	pool->current_task = NULL;

	pool->threads = calloc(n_threads, sizeof(*pool->threads));
	if (!pool->threads) {
		pr_debug2("threadpool: cannot allocate threads: %s\n",
			str_error_r(errno, sbuf, sizeof(sbuf)));
		err = -ENOMEM;
		goto out_free_pool;
	}

	for (t = 0; t < n_threads; t++) {
		pool->threads[t].idx = t;
		pool->threads[t].tid = -1;
		pool->threads[t].ptid = 0;
		pool->threads[t].pool = pool;
		pool->threads[t].running = false;
		threadpool_entry__init_pipes(&pool->threads[t]);
	}

	for (t = 0; t < n_threads; t++) {
		ret = threadpool_entry__open_pipes(&pool->threads[t]);
		if (ret) {
			err = ret;
			goto out_close_pipes;
		}
	}

	return pool;

out_close_pipes:
	for (t = 0; t < n_threads; t++)
		threadpool_entry__close_pipes(&pool->threads[t]);

	zfree(&pool->threads);
out_free_pool:
	free(pool);
out_return:
	return ERR_PTR(err);
}

/**
 * threadpool__strerror - print message regarding given @err in @pool
 *
 * Buffer size should be at least THREADPOOL_STRERR_BUFSIZE bytes.
 */
int threadpool__strerror(struct threadpool *pool __maybe_unused, int err, char *buf, size_t size)
{
	char sbuf[STRERR_BUFSIZE], *emsg;
	const char *errno_str;
	int err_idx = -err-THREADPOOL_ERROR__OFFSET;

	switch (err) {
	case -THREADPOOL_ERROR__SIGPROCMASK:
	case -THREADPOOL_ERROR__READPIPE:
	case -THREADPOOL_ERROR__WRITEPIPE:
		emsg = str_error_r(errno, sbuf, sizeof(sbuf));
		errno_str = threadpool_errno_str[err_idx];
		return scnprintf(buf, size, "%s: %s.\n", errno_str, emsg);
	case -THREADPOOL_ERROR__INVALIDMSG:
		errno_str = threadpool_errno_str[err_idx];
		return scnprintf(buf, size, "%s.\n", errno_str);
	default:
		emsg = str_error_r(err, sbuf, sizeof(sbuf));
		return scnprintf(buf, size, "Error: %s", emsg);
	}
}

/**
 * threadpool__new_strerror - print message regarding @err_ptr
 *
 * Buffer size should be at least THREADPOOL_STRERR_BUFSIZE bytes.
 */
int threadpool__new_strerror(struct threadpool *err_ptr, char *buf, size_t size)
{
	return threadpool__strerror(err_ptr, PTR_ERR(err_ptr), buf, size);
}

/**
 * threadpool__delete - free the @pool and all its resources
 */
void threadpool__delete(struct threadpool *pool)
{
	struct threadpool_entry *thread;
	int t;

	if (IS_ERR_OR_NULL(pool))
		return;

	for (t = 0; t < pool->nr_threads; t++) {
		thread = &pool->threads[t];
		threadpool_entry__close_pipes(thread);
	}

	zfree(&pool->threads);
	free(pool);
}

/**
 * threadpool__size - get number of threads in the threadpool
 */
int threadpool__size(struct threadpool *pool)
{
	return pool->nr_threads;
}

/**
 * threadpool__start_thread - start thread @tidx of the pool
 *
 * The function blocks until the thread is up and running.
 * This function can also be called if the threadpool is already executing.
 */
int threadpool__start_thread(struct threadpool *pool, int tidx)
{
	char sbuf[STRERR_BUFSIZE];
	int ret, err = 0;
	sigset_t full, mask;
	pthread_attr_t attrs;
	struct threadpool_entry *thread = &pool->threads[tidx];

	if (thread->running)
		return -EBUSY;

	sigfillset(&full);
	if (sigprocmask(SIG_SETMASK, &full, &mask)) {
		pr_debug2("Failed to block signals on threads start: %s\n",
			str_error_r(errno, sbuf, sizeof(sbuf)));
		return -THREADPOOL_ERROR__SIGPROCMASK;
	}

	pthread_attr_init(&attrs);
	pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);

	ret = pthread_create(&thread->ptid, &attrs, threadpool_entry__function, thread);
	if (ret) {
		err = -ret;
		pr_debug2("Failed to start threads: %s\n", str_error_r(ret, sbuf, sizeof(sbuf)));
		goto out;
	}

	err = threadpool__wait_thread(pool, tidx);
	if (err)
		goto out_cancel;

	thread->running = true;

out:
	pthread_attr_destroy(&attrs);

	if (sigprocmask(SIG_SETMASK, &mask, NULL)) {
		pr_debug2("Failed to unblock signals on threads start: %s\n",
			str_error_r(errno, sbuf, sizeof(sbuf)));
		err = -THREADPOOL_ERROR__SIGPROCMASK;
	}

	return err;

out_cancel:
	pthread_cancel(thread->ptid);
	goto out;
}

/**
 * threadpool__start - start all threads in the pool.
 *
 * The function blocks until all threads are up and running.
 */
int threadpool__start(struct threadpool *pool)
{
	int t, tt, err = 0, nr_threads = pool->nr_threads;

	for (t = 0; t < nr_threads; t++) {
		err = threadpool__start_thread(pool, t);
		if (err)
			goto out_terminate;
	}

out:
	return err;

out_terminate:
	for (tt = 0; tt < t; tt++)
		threadpool__terminate_thread(pool, tt);
	goto out;
}


/**
 * threadpool__stop - stop all threads in the pool.
 *
 * This function blocks waiting for ack from all threads.
 */
int threadpool__stop(struct threadpool *pool)
{
	int t, ret, err = 0;

	for (t = 0; t < pool->nr_threads; t++) {
		/**
		 * Even if a termination fails, we should continue to terminate
		 * all other threads.
		 */
		ret = threadpool__terminate_thread(pool, t);
		if (ret)
			err = ret;
	}

	return err;
}

/**
 * threadpool__is_running - return true if any of the threads is running
 */
bool threadpool__is_running(struct threadpool *pool)
{
	int t;

	for (t = 0; t < pool->nr_threads; t++)
		if (pool->threads[t].running)
			return true;
	return false;
}

// SPDX-License-Identifier: GPL-2.0
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/zalloc.h>
#include "debug.h"
#include <internal/lib.h>
#include "workqueue.h"

enum worker_msg {
	WORKER_MSG__UNDEFINED,
	WORKER_MSG__READY,                          /* from worker: ack */
	WORKER_MSG__WAKE,                           /* to worker: wake up */
	WORKER_MSG__STOP,                           /* to worker: exit */
	WORKER_MSG__ERROR,
	WORKER_MSG__MAX
};

enum worker_status {
	WORKER_STATUS__IDLE,		/* worker is sleeping, waiting for signal */
	WORKER_STATUS__BUSY,		/* worker is executing */
	WORKER_STATUS__MAX
};

struct workqueue_struct {
	pthread_mutex_t		lock;		/* locking of the workqueue */
	pthread_cond_t		idle_cond;	/* all workers are idle cond */
	struct threadpool	*pool;		/* underlying pool */
	int			pool_errno;	/* latest pool error */
	struct task_struct	task;		/* threadpool task */
	struct list_head	busy_list;	/* busy workers */
	struct list_head	idle_list;	/* idle workers */
	int			msg_pipe[2];	/* main thread comm pipes */
	struct worker		**workers;	/* array of all workers */
};

static const char * const workqueue_errno_str[] = {
	"Error creating threadpool",
	"Error executing function in threadpool",
	"Error stopping the threadpool",
	"Error starting thread in the threadpool",
	"Error sending message to worker",
	"Error receiving message from worker",
	"Received unexpected message from worker",
};

struct worker {
	pthread_mutex_t			lock;		/* locking of the thread_pool */
	int				tidx;		/* idx of thread in pool */
	struct list_head		entry;		/* in idle or busy list */
	struct work_struct		*current_work;	/* work being processed */
	int				msg_pipe[2];	/* main thread comm pipes*/
	struct list_head		queue;		/* pending work items */
	enum worker_status		status;		/* worker status */
};

#define for_each_busy_worker(wq, m_worker) \
	list_for_each_entry(m_worker, &wq->busy_list, entry)

#define for_each_idle_worker(wq, m_worker) \
	list_for_each_entry(m_worker, &wq->idle_list, entry)

static inline int lock_workqueue(struct workqueue_struct *wq)
__acquires(&wq->lock)
{
	__acquire(&wq->lock);
	return pthread_mutex_lock(&wq->lock);
}

static inline int unlock_workqueue(struct workqueue_struct *wq)
__releases(&wq->lock)
{
	__release(&wq->lock);
	return pthread_mutex_unlock(&wq->lock);
}

static inline int lock_worker(struct worker *worker)
__acquires(&worker->lock)
{
	__acquire(&worker->lock);
	return pthread_mutex_lock(&worker->lock);
}

static inline int unlock_worker(struct worker *worker)
__releases(&worker->lock)
{
	__release(&worker->lock);
	return pthread_mutex_unlock(&worker->lock);
}

/**
 * available_work - check if worker @worker has work to do
 */
static int available_work(struct worker *worker)
__must_hold(&worker->lock)
{
	return !list_empty(&worker->queue);
}

/**
 * dequeue_work - retrieve the next work in worker @worker's queue
 *
 * Called inside worker.
 */
static struct work_struct *dequeue_work(struct worker *worker)
__must_hold(&worker->lock)
{
	struct work_struct *work = list_first_entry(&worker->queue, struct work_struct, entry);

	list_del_init(&work->entry);
	return work;
}

/**
 * spinup_worker - start worker underlying thread and wait for it
 *
 * This function MUST NOT hold any lock and can be called only from main thread.
 */
static int spinup_worker(struct workqueue_struct *wq, int tidx)
{
	int ret;
	enum worker_msg msg = WORKER_MSG__UNDEFINED;
	char sbuf[STRERR_BUFSIZE];

	wq->pool_errno = threadpool__start_thread(wq->pool, tidx);
	if (wq->pool_errno)
		return -WORKQUEUE_ERROR__POOLSTARTTHREAD;

	ret = readn(wq->msg_pipe[0], &msg, sizeof(msg));
	if (ret < 0) {
		pr_debug("workqueue: error receiving ack: %s\n",
			str_error_r(errno, sbuf, sizeof(sbuf)));
		return -WORKQUEUE_ERROR__READPIPE;
	}
	if (msg != WORKER_MSG__READY) {
		pr_debug2("workqueue: received error\n");
		return -WORKQUEUE_ERROR__INVALIDMSG;
	}

	pr_debug("workqueue: spinup worker %d\n", tidx);

	return 0;
}

/**
 * sleep_worker - worker @worker of workqueue @wq goes to sleep
 *
 * Called inside worker.
 * If this was the last idle thread, signal it to the main thread, in case it
 * was flushing the workqueue.
 */
static void sleep_worker(struct workqueue_struct *wq, struct worker *worker)
__must_hold(&wq->lock)
{
	worker->status = WORKER_STATUS__IDLE;
	list_move(&worker->entry, &wq->idle_list);
	if (list_empty(&wq->busy_list))
		pthread_cond_signal(&wq->idle_cond);
}

/**
 * dequeue_or_sleep - check if work is available and dequeue or go to sleep
 *
 * Called inside worker.
 */
static void dequeue_or_sleep(struct worker *worker, struct workqueue_struct *wq)
__must_hold(&worker->lock)
{
	if (available_work(worker)) {
		worker->current_work = dequeue_work(worker);
		pr_debug2("worker[%d]: dequeued work\n", worker->tidx);
	} else {
		unlock_worker(worker);

		lock_workqueue(wq);
		lock_worker(worker);

		// Check if I've been assigned new work in the
		// meantime
		if (available_work(worker)) {
			// yep, no need to sleep
			worker->current_work = dequeue_work(worker);
		} else {
			// nope, I gotta sleep
			worker->current_work = NULL;
			sleep_worker(wq, worker);
			pr_debug2("worker[%d]: going to sleep\n", worker->tidx);
		}
		unlock_workqueue(wq);
	}
}


/**
 * stop_worker - stop worker @worker
 *
 * Called from main thread.
 * Send stop message to worker @worker.
 */
static int stop_worker(struct worker *worker)
{
	int ret;
	enum worker_msg msg;
	char sbuf[STRERR_BUFSIZE];

	msg = WORKER_MSG__STOP;
	ret = writen(worker->msg_pipe[1], &msg, sizeof(msg));
	if (ret < 0) {
		pr_debug2("workqueue: error sending stop msg: %s\n",
			str_error_r(errno, sbuf, sizeof(sbuf)));
		return -WORKQUEUE_ERROR__WRITEPIPE;
	}

	return 0;
}

/**
 * init_worker - init @worker struct
 * @worker: the struct to init
 * @tidx: index of the executing thread inside the threadpool
 */
static int init_worker(struct worker *worker, int tidx)
{
	int ret;
	char sbuf[STRERR_BUFSIZE];

	if (pipe(worker->msg_pipe)) {
		pr_debug2("worker[%d]: error opening pipe: %s\n",
			tidx, str_error_r(errno, sbuf, sizeof(sbuf)));
		return -ENOMEM;
	}

	worker->tidx = tidx;
	worker->current_work = NULL;
	worker->status = WORKER_STATUS__IDLE;
	INIT_LIST_HEAD(&worker->entry);
	INIT_LIST_HEAD(&worker->queue);

	ret = pthread_mutex_init(&worker->lock, NULL);
	if (ret)
		return -ret;

	return 0;
}

/**
 * fini_worker - deallocate resources used by @worker struct
 */
static void fini_worker(struct worker *worker)
{
	close(worker->msg_pipe[0]);
	worker->msg_pipe[0] = -1;
	close(worker->msg_pipe[1]);
	worker->msg_pipe[1] = -1;
	pthread_mutex_destroy(&worker->lock);
}

/**
 * register_worker - add worker to @wq->idle_list
 */
static void register_worker(struct workqueue_struct *wq, struct worker *worker)
__must_hold(&wq->lock)
{
	list_move(&worker->entry, &wq->idle_list);
	wq->workers[worker->tidx] = worker;
}

/**
 * unregister_worker - remove worker from @wq->idle_list
 */
static void unregister_worker(struct workqueue_struct *wq __maybe_unused,
			struct worker *worker)
__must_hold(&wq->lock)
{
	list_del_init(&worker->entry);
	wq->workers[worker->tidx] = NULL;
}

/**
 * worker_thread - worker function executed on threadpool
 */
static void worker_thread(int tidx, struct task_struct *task)
{
	struct workqueue_struct *wq = container_of(task, struct workqueue_struct, task);
	char sbuf[STRERR_BUFSIZE];
	struct worker this_worker;
	enum worker_msg msg;
	int ret, init_err = init_worker(&this_worker, tidx);

	if (init_err) {
		// send error message to main thread
		msg = WORKER_MSG__ERROR;
	} else {
		lock_workqueue(wq);
		register_worker(wq, &this_worker);
		unlock_workqueue(wq);

		// ack worker creation
		msg = WORKER_MSG__READY;
	}

	ret = writen(wq->msg_pipe[1], &msg, sizeof(msg));
	if (ret < 0) {
		pr_debug("worker[%d]: error sending msg: %s\n",
			tidx, str_error_r(errno, sbuf, sizeof(sbuf)));

		if (init_err)
			return;
		goto out;
	}

	// stop if there have been errors in init
	if (init_err)
		return;

	for (;;) {
		msg = WORKER_MSG__UNDEFINED;
		ret = readn(this_worker.msg_pipe[0], &msg, sizeof(msg));
		if (ret < 0 || (msg != WORKER_MSG__WAKE && msg != WORKER_MSG__STOP)) {
			pr_debug("worker[%d]: error receiving msg: %s\n",
				tidx, str_error_r(errno, sbuf, sizeof(sbuf)));
			break;
		}

		if (msg == WORKER_MSG__STOP)
			break;

		// main thread takes care of moving to busy list and appending
		// work to list

		for (;;) {
			lock_worker(&this_worker);
			dequeue_or_sleep(&this_worker, wq);
			unlock_worker(&this_worker);

			if (!this_worker.current_work)
				break;

			this_worker.current_work->func(this_worker.current_work);
		};
	}

out:
	lock_workqueue(wq);
	unregister_worker(wq, &this_worker);
	unlock_workqueue(wq);

	fini_worker(&this_worker);
}

/**
 * create_workqueue - create a workqueue associated to @pool
 *
 * The workqueue will create a threadpool on which to execute.
 */
struct workqueue_struct *create_workqueue(int nr_threads)
{
	int ret, err = 0, t;
	struct worker *worker;
	struct workqueue_struct *wq = zalloc(sizeof(struct workqueue_struct));

	if (!wq) {
		err = -ENOMEM;
		goto out_return;
	}

	wq->pool = threadpool__new(nr_threads);
	if (IS_ERR(wq->pool)) {
		err = -WORKQUEUE_ERROR__POOLNEW;
		wq->pool_errno = PTR_ERR(wq->pool);
		goto out_free_wq;
	}

	wq->workers = calloc(nr_threads, sizeof(*wq->workers));
	if (!wq->workers) {
		err = -ENOMEM;
		goto out_delete_pool;
	}

	ret = pthread_mutex_init(&wq->lock, NULL);
	if (ret) {
		err = -ret;
		goto out_free_workers;
	}

	ret = pthread_cond_init(&wq->idle_cond, NULL);
	if (ret) {
		err = -ret;
		goto out_destroy_mutex;
	}

	INIT_LIST_HEAD(&wq->busy_list);
	INIT_LIST_HEAD(&wq->idle_list);

	ret = pipe(wq->msg_pipe);
	if (ret) {
		err = -ENOMEM;
		goto out_destroy_cond;
	}

	wq->task.fn = worker_thread;

	wq->pool_errno = threadpool__set_task(wq->pool, &wq->task);
	if (wq->pool_errno) {
		err = -WORKQUEUE_ERROR__POOLEXE;
		goto out_close_pipe;
	}

	for (t = 0; t < nr_threads; t++) {
		err = spinup_worker(wq, t);
		if (err)
			goto out_stop_pool;
	}

	return wq;

out_stop_pool:
	lock_workqueue(wq);
	for_each_idle_worker(wq, worker) {
		ret = stop_worker(worker);
		if (ret)
			err = ret;
	}
	unlock_workqueue(wq);
out_close_pipe:
	close(wq->msg_pipe[0]);
	wq->msg_pipe[0] = -1;
	close(wq->msg_pipe[1]);
	wq->msg_pipe[1] = -1;
out_destroy_cond:
	pthread_cond_destroy(&wq->idle_cond);
out_destroy_mutex:
	pthread_mutex_destroy(&wq->lock);
out_free_workers:
	free(wq->workers);
out_delete_pool:
	threadpool__delete(wq->pool);
out_free_wq:
	free(wq);
out_return:
	return ERR_PTR(err);
}

/**
 * destroy_workqueue - stop @wq workers and destroy @wq
 */
int destroy_workqueue(struct workqueue_struct *wq)
{
	struct worker *worker;
	int err = 0, ret;
	char sbuf[STRERR_BUFSIZE];

	if (IS_ERR_OR_NULL(wq))
		return 0;

	lock_workqueue(wq);
	for_each_idle_worker(wq, worker) {
		ret = stop_worker(worker);
		if (ret)
			err = ret;
	}
	unlock_workqueue(wq);

	wq->pool_errno = threadpool__stop(wq->pool);
	if (wq->pool_errno) {
		pr_debug2("workqueue: error stopping threadpool\n");
		err = -WORKQUEUE_ERROR__POOLSTOP;
	}

	threadpool__delete(wq->pool);
	wq->pool = NULL;

	ret = pthread_mutex_destroy(&wq->lock);
	if (ret) {
		err = -ret;
		pr_debug2("workqueue: error pthread_mutex_destroy: %s\n",
			str_error_r(ret, sbuf, sizeof(sbuf)));
	}

	ret = pthread_cond_destroy(&wq->idle_cond);
	if (ret) {
		err = -ret;
		pr_debug2("workqueue: error pthread_cond_destroy: %s\n",
			str_error_r(ret, sbuf, sizeof(sbuf)));
	}

	close(wq->msg_pipe[0]);
	wq->msg_pipe[0] = -1;

	close(wq->msg_pipe[1]);
	wq->msg_pipe[1] = -1;

	zfree(&wq->workers);
	free(wq);
	return err;
}

/**
 * workqueue_strerror - print message regarding lastest error in @wq
 *
 * Buffer size should be at least WORKQUEUE_STRERR_BUFSIZE bytes.
 */
int workqueue_strerror(struct workqueue_struct *wq, int err, char *buf, size_t size)
{
	int ret, err_idx = -err - WORKQUEUE_ERROR__OFFSET;
	char sbuf[THREADPOOL_STRERR_BUFSIZE], *emsg;
	const char *errno_str;

	switch (err) {
	case -WORKQUEUE_ERROR__POOLNEW:
	case -WORKQUEUE_ERROR__POOLEXE:
	case -WORKQUEUE_ERROR__POOLSTOP:
	case -WORKQUEUE_ERROR__POOLSTARTTHREAD:
		errno_str = workqueue_errno_str[err_idx];
		if (IS_ERR_OR_NULL(wq))
			return scnprintf(buf, size, "%s: unknown.\n",
				errno_str);

		ret = threadpool__strerror(wq->pool, wq->pool_errno, sbuf, sizeof(sbuf));
		if (ret < 0)
			return ret;
		return scnprintf(buf, size, "%s: %s.\n", errno_str, sbuf);
	case -WORKQUEUE_ERROR__WRITEPIPE:
	case -WORKQUEUE_ERROR__READPIPE:
		errno_str = workqueue_errno_str[err_idx];
		emsg = str_error_r(errno, sbuf, sizeof(sbuf));
		return scnprintf(buf, size, "%s: %s.\n", errno_str, emsg);
	case -WORKQUEUE_ERROR__INVALIDMSG:
	case -WORKQUEUE_ERROR__INVALIDWORKERSTATUS:
	case -WORKQUEUE_ERROR__NOTREADY:
		errno_str = workqueue_errno_str[err_idx];
		return scnprintf(buf, size, "%s.\n", errno_str);
	default:
		emsg = str_error_r(err, sbuf, sizeof(sbuf));
		return scnprintf(buf, size, "Error: %s\n", emsg);
	}
}

/**
 * create_workqueue_strerror - print message regarding @err_ptr
 *
 * Buffer size should be at least WORKQUEUE_STRERR_BUFSIZE bytes.
 */
int create_workqueue_strerror(struct workqueue_struct *err_ptr, char *buf, size_t size)
{
	return workqueue_strerror(err_ptr, PTR_ERR(err_ptr), buf, size);
}

/**
 * destroy_workqueue_strerror - print message regarding @err
 *
 * Buffer size should be at least WORKQUEUE_STRERR_BUFSIZE bytes.
 */
int destroy_workqueue_strerror(int err, char *buf, size_t size)
{
	return workqueue_strerror(NULL, err, buf, size);
}

/**
 * workqueue_nr_threads - get size of threadpool underlying @wq
 */
int workqueue_nr_threads(struct workqueue_struct *wq)
{
	return threadpool__size(wq->pool);
}

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

struct workqueue_struct {
	pthread_mutex_t		lock;		/* locking of the workqueue */
	pthread_cond_t		idle_cond;	/* all workers are idle cond */
	struct threadpool	*pool;		/* underlying pool */
	int			pool_errno;	/* latest pool error */
	struct task_struct	task;		/* threadpool task */
	struct list_head	busy_list;	/* busy workers */
	struct list_head	idle_list;	/* idle workers */
	int			msg_pipe[2];	/* main thread comm pipes */
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

/**
 * create_workqueue - create a workqueue associated to @pool
 *
 * The workqueue will create a threadpool on which to execute.
 */
struct workqueue_struct *create_workqueue(int nr_threads)
{
	int ret, err = 0;
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

	ret = pthread_mutex_init(&wq->lock, NULL);
	if (ret) {
		err = -ret;
		goto out_delete_pool;
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

	return wq;

out_destroy_cond:
	pthread_cond_destroy(&wq->idle_cond);
out_destroy_mutex:
	pthread_mutex_destroy(&wq->lock);
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
	int err = 0, ret;
	char sbuf[STRERR_BUFSIZE];

	if (IS_ERR_OR_NULL(wq))
		return 0;

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
	int ret;
	char sbuf[THREADPOOL_STRERR_BUFSIZE], *emsg;
	const char *errno_str;

	errno_str = workqueue_errno_str[-err-WORKQUEUE_ERROR__OFFSET];

	switch (err) {
	case -WORKQUEUE_ERROR__POOLNEW:
	case -WORKQUEUE_ERROR__POOLEXE:
	case -WORKQUEUE_ERROR__POOLSTOP:
	case -WORKQUEUE_ERROR__POOLSTARTTHREAD:
		if (IS_ERR_OR_NULL(wq))
			return scnprintf(buf, size, "%s: unknown.\n",
				errno_str);

		ret = threadpool__strerror(wq->pool, wq->pool_errno, sbuf, sizeof(sbuf));
		if (ret < 0)
			return ret;
		return scnprintf(buf, size, "%s: %s.\n", errno_str, sbuf);
	case -WORKQUEUE_ERROR__WRITEPIPE:
	case -WORKQUEUE_ERROR__READPIPE:
		emsg = str_error_r(errno, sbuf, sizeof(sbuf));
		return scnprintf(buf, size, "%s: %s.\n", errno_str, emsg);
	case -WORKQUEUE_ERROR__INVALIDMSG:
		return scnprintf(buf, size, "%s.\n", errno_str);
	default:
		emsg = str_error_r(err, sbuf, sizeof(sbuf));
		return scnprintf(buf, size, "Error: %s", emsg);
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

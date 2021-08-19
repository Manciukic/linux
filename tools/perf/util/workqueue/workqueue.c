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
#include <linux/kernel.h>
#include "debug.h"
#include <internal/lib.h>
#include "workqueue.h"

struct workqueue_struct *global_wq;

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
	struct worker		*next_worker;	/* next worker to choose (round robin) */
	int			first_stopped_worker; /* next worker to start if needed */
};

static const char * const workqueue_errno_str[] = {
	"Error creating threadpool",
	"Error executing function in threadpool",
	"Error stopping the threadpool",
	"Error starting thread in the threadpool",
	"Error setting affinity in threadpool",
	"Error sending message to worker",
	"Error receiving message from worker",
	"Received unexpected message from worker",
	"Worker is not ready",
	"Worker is in an unrecognized status",
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

static void advance_next_worker(struct workqueue_struct *wq)
__must_hold(&wq->lock)
{
	if (list_is_last(&wq->next_worker->entry, &wq->busy_list))
		wq->next_worker = list_first_entry(&wq->busy_list, struct worker, entry);
	else
		wq->next_worker = list_next_entry(wq->next_worker, entry);
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
	if (wq->next_worker == worker)
		advance_next_worker(wq);
	list_move(&worker->entry, &wq->idle_list);
	if (list_empty(&wq->busy_list)) {
		wq->next_worker = NULL;
		pthread_cond_signal(&wq->idle_cond);
	}
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
 * wake_worker - prepare for waking worker @worker of workqueue @wq assigning @work to do
 *
 * Called from main thread.
 * Moves worker from idle to busy list and assigns @work to it.
 * Must call wake_worker outside critical section afterwards.
 */
static int prepare_wake_worker(struct workqueue_struct *wq, struct worker *worker,
			struct work_struct *work)
__must_hold(&wq->lock)
__must_hold(&worker->lock)
{
	if (wq->next_worker)
		list_move_tail(&worker->entry, &wq->next_worker->entry);
	else
		list_move(&worker->entry, &wq->busy_list);
	wq->next_worker = worker;

	list_add_tail(&work->entry, &worker->queue);
	worker->status = WORKER_STATUS__BUSY;

	return 0;
}

/**
 * wake_worker - send wake message to worker @worker of workqueue @wq
 *
 * Called from main thread.
 * Must be called after prepare_wake_worker and outside critical section to
 * reduce time spent inside it
 */
static int wake_worker(struct worker *worker)
{
	enum worker_msg msg = WORKER_MSG__WAKE;
	int ret;
	char sbuf[STRERR_BUFSIZE];

	ret = writen(worker->msg_pipe[1], &msg, sizeof(msg));
	if (ret < 0) {
		pr_debug2("wake worker %d: error seding msg: %s\n",
			worker->tidx, str_error_r(errno, sbuf, sizeof(sbuf)));
		return -WORKQUEUE_ERROR__WRITEPIPE;
	}

	return 0;
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

	wq->pool_errno = threadpool__execute(wq->pool, &wq->task);
	if (wq->pool_errno) {
		err = -WORKQUEUE_ERROR__POOLEXE;
		goto out_close_pipe;
	}

	wq->next_worker = NULL;
	wq->first_stopped_worker = 0;

	return wq;

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
	case -WORKQUEUE_ERROR__INVALIDWORKERSTATUS:
	case -WORKQUEUE_ERROR__NOTREADY:
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

/**
 * __queue_work_on_worker - add @work to the internal queue of worker @worker
 *
 * NB: this function releases the locks to be able to send notification to
 * thread outside the critical section.
 */
static int __queue_work_on_worker(struct workqueue_struct *wq __maybe_unused,
				struct worker *worker, struct work_struct *work)
__must_hold(&wq->lock)
__must_hold(&worker->lock)
__releases(&wq->lock)
__releases(&worker->lock)
{
	int ret;

	switch (worker->status) {
	case WORKER_STATUS__BUSY:
		list_add_tail(&work->entry, &worker->queue);

		unlock_worker(worker);
		unlock_workqueue(wq);
		pr_debug("workqueue: queued new work item\n");
		return 0;
	case WORKER_STATUS__IDLE:
		ret = prepare_wake_worker(wq, worker, work);
		unlock_worker(worker);
		unlock_workqueue(wq);
		if (ret)
			return ret;
		ret = wake_worker(worker);
		if (!ret)
		pr_debug("workqueue: woke worker %d\n", worker->tidx);
		return ret;
	default:
	case WORKER_STATUS__MAX:
		unlock_worker(worker);
		unlock_workqueue(wq);
		pr_debug2("workqueue: worker is in unrecognized status %d\n",
			worker->status);
		return -WORKQUEUE_ERROR__INVALIDWORKERSTATUS;
	}

	return 0;
}

/**
 * queue_work - add @work to @wq internal queue
 *
 * If there are idle threads, one of these will be woken up.
 * Otherwise, the work is added to the pending list.
 */
int queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	int ret;
	struct worker *worker;

repeat:
	lock_workqueue(wq);
	if (list_empty(&wq->idle_list)) {
		// find a worker to spin up
		while (wq->first_stopped_worker < threadpool__size(wq->pool)
				&& wq->workers[wq->first_stopped_worker])
			wq->first_stopped_worker++;

		// found one
		if (wq->first_stopped_worker < threadpool__size(wq->pool)) {
			// spinup does not hold the lock to make the thread register itself
			unlock_workqueue(wq);
			ret = spinup_worker(wq, wq->first_stopped_worker);
			if (ret)
				return ret;
			// worker is now in idle_list
			goto repeat;
		}

		worker = wq->next_worker;
		advance_next_worker(wq);
	} else {
		worker = list_first_entry(&wq->idle_list, struct worker, entry);
	}
	lock_worker(worker);

	return __queue_work_on_worker(wq, worker, work);
}

/**
 * queue_work_on_worker - add @work to worker @tidx internal queue
 */
int queue_work_on_worker(int tidx, struct workqueue_struct *wq, struct work_struct *work)
{
	int ret;

	lock_workqueue(wq);
	if (!wq->workers[tidx]) {
		// spinup does not hold the lock to make the thread register itself
		unlock_workqueue(wq);
		ret = spinup_worker(wq, tidx);
		if (ret)
			return ret;

		// now recheck if worker is available
		lock_workqueue(wq);
		if (!wq->workers[tidx]) {
			unlock_workqueue(wq);
			return -WORKQUEUE_ERROR__NOTREADY;
		}
	}

	lock_worker(wq->workers[tidx]);
	return __queue_work_on_worker(wq, wq->workers[tidx], work);
}

/**
 * flush_workqueue - wait for all currently executed and pending work to finish
 *
 * This function blocks until all threads become idle.
 */
int flush_workqueue(struct workqueue_struct *wq)
{
	int err = 0, ret;

	lock_workqueue(wq);
	while (!list_empty(&wq->busy_list)) {
		ret = pthread_cond_wait(&wq->idle_cond, &wq->lock);
		if (ret) {
			pr_debug2("%s: error in pthread_cond_wait\n", __func__);
			err = -ret;
			break;
		}
	}
	unlock_workqueue(wq);

	return err;
}

/**
 * workqueue_set_affinities - set affinities to all threads in @wq->pool
 */
int workqueue_set_affinities(struct workqueue_struct *wq,
				struct mmap_cpu_mask *affinities)
{
	wq->pool_errno = threadpool__set_affinities(wq->pool, affinities);
	return wq->pool_errno ? -WORKQUEUE_ERROR__POOLAFFINITY : 0;
}

/**
 * workqueue_set_affinities - set affinity to thread @tidx in @wq->pool
 */
int workqueue_set_affinity(struct workqueue_struct *wq, int tidx,
				struct mmap_cpu_mask *affinity)
{
	wq->pool_errno = threadpool__set_affinity(wq->pool, tidx, affinity);
	return wq->pool_errno ? -WORKQUEUE_ERROR__POOLAFFINITY : 0;
}

/**
 * init_work - initialize the @work struct
 */
void init_work(struct work_struct *work)
{
	INIT_LIST_HEAD(&work->entry);
}

/* Parallel-for utility */

struct parallel_for_work {
	struct work_struct work;	/* work item that is queued */
	parallel_for_func_t func;	/* function to execute for each item */
	void *args;			/* additional args to pass to func */
	int start;			/* first item to execute */
	int num;			/* number of items to execute */
};

/**
 * parallel_for_work_fn - execute parallel_for_work.func in parallel
 *
 * This function will be executed by workqueue's workers.
 */
static void parallel_for_work_fn(struct work_struct *work)
{
	struct parallel_for_work *pfw = container_of(work, struct parallel_for_work, work);
	int i;

	for (i = 0; i < pfw->num; i++)
		pfw->func(pfw->start+i, pfw->args);
}

static inline void init_parallel_for_work(struct parallel_for_work *pfw,
					parallel_for_func_t func, void *args,
					int start, int num)
{
	init_work(&pfw->work);
	pfw->work.func = parallel_for_work_fn;
	pfw->func = func;
	pfw->args = args;
	pfw->start = start;
	pfw->num = num;

	pr_debug2("pfw: start=%d, num=%d\n", start, num);
}

/**
 * parallel_for - execute @func in parallel over indexes between @from and @to
 * @wq: workqueue that will run @func in parallel
 * @from: first index
 * @to: last index (excluded)
 * @work_size: number of indexes to handle on the same work item.
 *             ceil((to-from)/work_size) work items will be added to @wq
 *             NB: this is only a hint. The function will reduce the size of
 *                 the work items to fill all workers.
 * @func: function to execute in parallel
 * @args: additional arguments to @func
 *
 * This function is equivalent to:
 * for (i = from; i < to; i++) {
 *     // parallel
 *     func(i, args);
 * }
 * // sync
 *
 * This function takes care of:
 *  - creating balanced work items to submit to workqueue
 *  - submitting the work items to the workqueue
 *  - waiting for completion of the work items
 *  - cleanup of the work items
 */
int parallel_for(struct workqueue_struct *wq, int from, int to, int work_size,
		parallel_for_func_t func, void *args)
{
	int n = to-from;
	int n_work_items;
	int nr_threads = workqueue_nr_threads(wq);
	int i, j, start, num, m, base, num_per_item;
	struct parallel_for_work *pfw_array;
	int ret, err = 0;

	if (work_size <= 0) {
		pr_debug("workqueue parallel-for: work_size must be >0\n");
		return -EINVAL;
	}

	if (to < from) {
		pr_debug("workqueue parallel-for: to must be >= from\n");
		return -EINVAL;
	} else if (to == from) {
		pr_debug2("workqueue parallel-for: skip since from == to\n");
		return 0;
	}

	n_work_items = DIV_ROUND_UP(n, work_size);
	if (n_work_items < nr_threads)
		n_work_items = min(n, nr_threads);

	pfw_array = calloc(n_work_items, sizeof(*pfw_array));

	if (!pfw_array) {
		pr_debug2("%s: error allocating pfw_array\n", __func__);
		return -ENOMEM;
	}

	num_per_item = n / n_work_items;
	m = n % n_work_items;

	for (i = 0; i < m; i++) {
		num = num_per_item + 1;
		start = i * num;
		init_parallel_for_work(&pfw_array[i], func, args, start, num);
		ret = queue_work(wq, &pfw_array[i].work);
		if (ret) {
			err = ret;
			goto out;
		}
	}
	if (i != 0)
		base = pfw_array[i-1].start + pfw_array[i-1].num;
	else
		base = 0;
	for (j = i; j < n_work_items; j++) {
		num = num_per_item;
		start = base + (j - i) * num;
		init_parallel_for_work(&pfw_array[j], func, args, start, num);
		ret = queue_work(wq, &pfw_array[j].work);
		if (ret) {
			err = ret;
			goto out;
		}
	}

out:
	ret = flush_workqueue(wq);
	if (ret)
		err = ret;

	free(pfw_array);
	return err;
}

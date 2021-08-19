/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __WORKQUEUE_WORKQUEUE_H
#define __WORKQUEUE_WORKQUEUE_H

#include <stdlib.h>
#include <sys/types.h>
#include <linux/list.h>
#include <linux/err.h>
#include "threadpool.h"

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
	struct list_head entry;
	work_func_t func;
};

struct workqueue_struct;

extern struct workqueue_struct *create_workqueue(int nr_threads);
extern int destroy_workqueue(struct workqueue_struct *wq);

extern int workqueue_nr_threads(struct workqueue_struct *wq);

extern int queue_work(struct workqueue_struct *wq, struct work_struct *work);
extern int queue_work_on_worker(int tidx, struct workqueue_struct *wq, struct work_struct *work);
extern int queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work);

extern int flush_workqueue(struct workqueue_struct *wq);

extern int workqueue_set_affinities(struct workqueue_struct *wq,
				struct mmap_cpu_mask *affinities);
extern int workqueue_set_affinity(struct workqueue_struct *wq, int tidx,
				struct mmap_cpu_mask *affinity);
extern int workqueue_set_affinity_cpu(struct workqueue_struct *wq, int tidx, int cpu);
extern int workqueue_set_affinities_cpu(struct workqueue_struct *wq,
					struct perf_cpu_map *cpus);

extern void init_work(struct work_struct *work);

/* parallel_for utility */

typedef void (*parallel_for_func_t)(int i, void *args);

extern int parallel_for(struct workqueue_struct *wq, int from, int to, int work_size,
			parallel_for_func_t func, void *args);

/* Global workqueue */

extern struct workqueue_struct *global_wq;

/**
 * setup_global_wq - create the global_wq
 */
static inline int setup_global_workqueue(int nr_threads)
{
	global_wq = create_workqueue(nr_threads);
	return IS_ERR(global_wq) ? PTR_ERR(global_wq) : 0;
}

/**
 * teardown_global_wq - destroy the global_wq
 */
static inline int teardown_global_workqueue(void)
{
	int ret = destroy_workqueue(global_wq);

	global_wq = NULL;
	return ret;
}

/**
 * schedule_work - queue @work on the global_wq
 */
static inline int schedule_work(struct work_struct *work)
{
	return queue_work(global_wq, work);
}

/**
 * schedule_work - queue @work on thread @tidx of global_wq
 */
static inline int schedule_work_on_worker(int tidx, struct work_struct *work)
{
	return queue_work_on_worker(tidx, global_wq, work);
}

/**
 * schedule_work_on - queue @work to be executed on @cpu by global_wq
 */
static inline int schedule_work_on(int cpu, struct work_struct *work)
{
	return queue_work_on(cpu, global_wq, work);
}

/**
 * flush_scheduled_work - ensure that any scheduled work in global_wq has run to completion
 */
static inline int flush_scheduled_work(void)
{
	return flush_workqueue(global_wq);
}

#define WORKQUEUE_STRERR_BUFSIZE (128+THREADPOOL_STRERR_BUFSIZE)
#define WORKQUEUE_ERROR__OFFSET 512
enum {
	WORKQUEUE_ERROR__POOLNEW = WORKQUEUE_ERROR__OFFSET,
	WORKQUEUE_ERROR__POOLEXE,
	WORKQUEUE_ERROR__POOLSTOP,
	WORKQUEUE_ERROR__POOLSTARTTHREAD,
	WORKQUEUE_ERROR__POOLAFFINITY,
	WORKQUEUE_ERROR__WRITEPIPE,
	WORKQUEUE_ERROR__READPIPE,
	WORKQUEUE_ERROR__INVALIDMSG,
	WORKQUEUE_ERROR__NOTREADY,
	WORKQUEUE_ERROR__INVALIDWORKERSTATUS,
};
extern int workqueue_strerror(struct workqueue_struct *wq, int err, char *buf, size_t size);
extern int create_workqueue_strerror(struct workqueue_struct *err_ptr, char *buf, size_t size);
extern int destroy_workqueue_strerror(int err, char *buf, size_t size);
#endif /* __WORKQUEUE_WORKQUEUE_H */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __WORKQUEUE_THREADPOOL_H
#define __WORKQUEUE_THREADPOOL_H

#include "util/mmap.h"

struct threadpool;
struct task_struct;

typedef void (*task_func_t)(int tidx, struct task_struct *task);

struct task_struct {
	task_func_t fn;
};

extern struct threadpool *threadpool__new(int n_threads);
extern void threadpool__delete(struct threadpool *pool);

extern int threadpool__start_thread(struct threadpool *pool, int tidx);
extern int threadpool__start(struct threadpool *pool);
extern int threadpool__stop(struct threadpool *pool);

extern int threadpool__wait(struct threadpool *pool);
extern int threadpool__set_task(struct threadpool *pool, struct task_struct *task);

extern int threadpool__size(struct threadpool *pool);
extern bool threadpool__is_running(struct threadpool *pool);
extern bool threadpool__is_busy(struct threadpool *pool);

extern int threadpool__set_affinities(struct threadpool *pool,
				struct mmap_cpu_mask *affinities);
extern int threadpool__set_affinity(struct threadpool *pool, int tid,
				struct mmap_cpu_mask *affinity);

/* Error management */
#define THREADPOOL_STRERR_BUFSIZE (128+STRERR_BUFSIZE)
#define THREADPOOL_ERROR__OFFSET 512
enum {
	THREADPOOL_ERROR__SIGPROCMASK = THREADPOOL_ERROR__OFFSET,
	THREADPOOL_ERROR__READPIPE,
	THREADPOOL_ERROR__WRITEPIPE,
	THREADPOOL_ERROR__INVALIDMSG,
	THREADPOOL_ERROR__NOTALLOWED
};
extern int threadpool__strerror(struct threadpool *pool, int err, char *buf, size_t size);
extern int threadpool__new_strerror(struct threadpool *err_ptr, char *buf, size_t size);

#endif /* __WORKQUEUE_THREADPOOL_H */

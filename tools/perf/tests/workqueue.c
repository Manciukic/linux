// SPDX-License-Identifier: GPL-2.0
#include <unistd.h>
#include <stdlib.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/zalloc.h>
#include <linux/bitmap.h>
#include <perf/cpumap.h>
#include "tests.h"
#include "util/debug.h"
#include "util/workqueue/threadpool.h"
#include "util/workqueue/workqueue.h"

#define DUMMY_FACTOR 100000
#define N_DUMMY_WORK_SIZES 7

struct threadpool_test_args_t {
	int pool_size;
};

struct workqueue_test_args_t {
	int pool_size;
	int n_work_items;
};

struct parallel_for_test_args_t {
	int pool_size;
	int n_work_items;
	int work_size;
};

struct test_task {
	struct task_struct task;
	int n_threads;
	int *array;
	struct mmap_cpu_mask *affinity_masks;
};

/**
 * dummy_work - calculates DUMMY_FACTOR * (idx % N_DUMMY_WORK_SIZES) inefficiently
 *
 * This function uses modulus to create work items of different sizes.
 */
static void dummy_work(int idx)
{
	volatile int prod = 0;	/* prevent possible compiler optimizations */
	int k = idx % N_DUMMY_WORK_SIZES;
	int i, j;

	for (i = 0; i < DUMMY_FACTOR; i++)
		for (j = 0; j < k; j++)
			prod++;

	pr_debug3("dummy: %d * %d = %d\n", DUMMY_FACTOR, k, prod);
}

static void test_task_fn1(int tidx, struct task_struct *task)
{
	struct test_task *mtask = container_of(task, struct test_task, task);
	struct mmap_cpu_mask real_affinity_mask, *set_affinity_mask;
	int ret;

	set_affinity_mask = &mtask->affinity_masks[tidx];
	real_affinity_mask.nbits = set_affinity_mask->nbits;
	real_affinity_mask.bits = bitmap_alloc(real_affinity_mask.nbits);
	if (!real_affinity_mask.bits) {
		pr_err("ENOMEM in malloc real_affinity_mask.bits\n");
		goto out;
	}

	ret = pthread_getaffinity_np(pthread_self(), real_affinity_mask.nbits,
				(cpu_set_t *)real_affinity_mask.bits);
	if (ret) {
		pr_err("Error in pthread_getaffinity_np: %s\n", strerror(ret));
		goto out;
	}

	if (!bitmap_equal(real_affinity_mask.bits, set_affinity_mask->bits,
			real_affinity_mask.nbits)) {
		pr_err("affinity mismatch!\n");
		mmap_cpu_mask__scnprintf(set_affinity_mask, "set affinity");
		mmap_cpu_mask__scnprintf(&real_affinity_mask, "real affinity");
		goto out;
	}

	dummy_work(tidx);
	mtask->array[tidx] = tidx+1;
out:
	bitmap_free(real_affinity_mask.bits);
}

static void test_task_fn2(int tidx, struct task_struct *task)
{
	struct test_task *mtask = container_of(task, struct test_task, task);

	dummy_work(tidx);
	mtask->array[tidx] = tidx*2;
}


static int __threadpool__prepare(struct threadpool **pool, int pool_size)
{
	int ret;

	*pool = threadpool__new(pool_size);
	TEST_ASSERT_VAL("threadpool creation failure", !IS_ERR(*pool));
	TEST_ASSERT_VAL("threadpool size is wrong",
			threadpool__size(*pool) == pool_size);

	ret = threadpool__start(*pool);
	TEST_ASSERT_VAL("threadpool start failure", ret == 0);
	TEST_ASSERT_VAL("threadpool is not ready", threadpool__is_running(*pool));

	return TEST_OK;
}

static int __threadpool__teardown(struct threadpool *pool)
{
	int ret = threadpool__stop(pool);

	TEST_ASSERT_VAL("threadpool stop failure", ret == 0);
	TEST_ASSERT_VAL("stopped threadpool is ready",
			!threadpool__is_running(pool));

	threadpool__delete(pool);

	return TEST_OK;
}

static int __threadpool__exec_wait(struct threadpool *pool,
				struct task_struct *task)
{
	int ret = threadpool__execute(pool, task);

	TEST_ASSERT_VAL("threadpool execute failure", ret == 0);
	TEST_ASSERT_VAL("threadpool is not executing", threadpool__is_busy(pool));

	ret = threadpool__wait(pool);
	TEST_ASSERT_VAL("threadpool wait failure", ret == 0);
	TEST_ASSERT_VAL("waited threadpool is not running", threadpool__is_running(pool));

	return TEST_OK;
}

static int __test__threadpool(void *_args)
{
	struct threadpool_test_args_t *args = _args;
	struct threadpool *pool;
	int ret, i, nr_cpus, nr_bits, cpu;
	struct test_task task;
	int pool_size = args->pool_size ?: sysconf(_SC_NPROCESSORS_ONLN);
	struct perf_cpu_map *cpumap = perf_cpu_map__new(NULL);
	struct mmap_cpu_mask *affinity_masks;

	if (!cpumap) {
		pr_err("ENOMEM in perf_cpu_map__new\n");
		return TEST_FAIL;
	}

	nr_cpus = perf_cpu_map__nr(cpumap);
	nr_bits = BITS_TO_LONGS(nr_cpus) * sizeof(unsigned long);

	affinity_masks = calloc(pool_size, sizeof(*affinity_masks));
	if (!affinity_masks) {
		pr_err("ENOMEM in calloc affinity_masks\n");
		ret = TEST_FAIL;
		goto out_put_cpumap;
	}

	for (i = 0; i < pool_size; i++) {
		affinity_masks[i].nbits = nr_bits;
		affinity_masks[i].bits = bitmap_alloc(nr_cpus);
		if (!affinity_masks[i].bits) {
			ret = TEST_FAIL;
			goto out_free_affinity_masks;
		}
		bitmap_zero(affinity_masks[i].bits, affinity_masks[i].nbits);
		cpu = perf_cpu_map__cpu(cpumap, i % nr_cpus);
		test_and_set_bit(cpu, affinity_masks[i].bits);
	}

	task.task.fn = test_task_fn1;
	task.n_threads = pool_size;
	task.affinity_masks = affinity_masks;
	task.array = calloc(pool_size, sizeof(*task.array));
	TEST_ASSERT_VAL("calloc failure", task.array);

	ret = __threadpool__prepare(&pool, pool_size);
	if (ret)
		goto out_free_tasks;

	ret = threadpool__set_affinities(pool, task.affinity_masks);
	if (ret) {
		ret = TEST_FAIL;
		goto out_free_tasks;
	}

	ret = __threadpool__exec_wait(pool, &task.task);
	if (ret)
		goto out_free_tasks;

	for (i = 0; i < pool_size; i++)
		TEST_ASSERT_VAL("failed array check (1)", task.array[i] == i+1);

	task.task.fn = test_task_fn2;

	ret = __threadpool__exec_wait(pool, &task.task);
	if (ret)
		goto out_free_tasks;

	for (i = 0; i < pool_size; i++)
		TEST_ASSERT_VAL("failed array check (2)", task.array[i] == 2*i);

	ret = __threadpool__teardown(pool);
	if (ret)
		goto out_free_tasks;

out_free_tasks:
	free(task.array);
out_free_affinity_masks:
	for (i = 0; i < pool_size; i++)
		bitmap_free(affinity_masks[i].bits);
	free(affinity_masks);
out_put_cpumap:
	perf_cpu_map__put(cpumap);
	return ret;
}

struct test_work {
	struct work_struct work;
	int i;
	int *array;
};

static void test_work_fn1(struct work_struct *work)
{
	struct test_work *mwork = container_of(work, struct test_work, work);

	dummy_work(mwork->i);
	mwork->array[mwork->i] = mwork->i+1;
}

static void test_work_fn2(struct work_struct *work)
{
	struct test_work *mwork = container_of(work, struct test_work, work);

	dummy_work(mwork->i);
	mwork->array[mwork->i] = mwork->i*2;
}

static int __workqueue__prepare(struct workqueue_struct **wq,
				int pool_size)
{
	*wq = create_workqueue(pool_size);
	TEST_ASSERT_VAL("workqueue creation failure", !IS_ERR(*wq));
	TEST_ASSERT_VAL("workqueue wrong size", workqueue_nr_threads(*wq) == pool_size);

	return TEST_OK;
}

static int __workqueue__teardown(struct workqueue_struct *wq)
{
	int ret = destroy_workqueue(wq);

	TEST_ASSERT_VAL("workqueue detruction failure", ret == 0);

	return 0;
}

static int __workqueue__exec_wait(struct workqueue_struct *wq,
				int *array, struct test_work *works,
				work_func_t func, int n_work_items)
{
	int ret, i;

	for (i = 0; i < n_work_items; i++) {
		works[i].array = array;
		works[i].i = i;

		init_work(&works[i].work);
		works[i].work.func = func;
		queue_work(wq, &works[i].work);
	}

	ret = flush_workqueue(wq);
	TEST_ASSERT_VAL("workqueue flush failure", ret == 0);

	return TEST_OK;
}


static int __test__workqueue(void *_args)
{
	struct workqueue_test_args_t *args = _args;
	struct workqueue_struct *wq;
	struct test_work *works;
	int *array;
	int pool_size = args->pool_size ?: sysconf(_SC_NPROCESSORS_ONLN);
	int i, ret = __workqueue__prepare(&wq, pool_size);

	if (ret)
		return ret;

	array = calloc(args->n_work_items, sizeof(*array));
	TEST_ASSERT_VAL("failed array calloc", array);
	works = calloc(args->n_work_items, sizeof(*works));
	TEST_ASSERT_VAL("failed works calloc", works);

	ret = __workqueue__exec_wait(wq, array, works, test_work_fn1,
					args->n_work_items);
	if (ret)
		goto out;

	for (i = 0; i < args->n_work_items; i++)
		TEST_ASSERT_VAL("failed array check (1)", array[i] == i+1);

	ret = __workqueue__exec_wait(wq, array, works, test_work_fn2,
					args->n_work_items);
	if (ret)
		goto out;

	for (i = 0; i < args->n_work_items; i++)
		TEST_ASSERT_VAL("failed array check (2)", array[i] == 2*i);

	ret = __workqueue__teardown(wq);
	if (ret)
		goto out;

out:
	free(array);
	free(works);
	return ret;
}

static void test_pfw_fn(int i, void *args)
{
	int *array = args;

	dummy_work(i);
	array[i] = i+1;
}

static int __test__parallel_for(void *_args)
{
	struct parallel_for_test_args_t *args = _args;
	struct workqueue_struct *wq;
	int ret, i, pool_size = args->pool_size ?: sysconf(_SC_NPROCESSORS_ONLN);
	int *array = calloc(args->n_work_items, sizeof(*array));

	TEST_ASSERT_VAL("calloc array failure", array);

	ret = __workqueue__prepare(&wq, pool_size);
	if (ret)
		goto out;

	ret = parallel_for(wq, 0, args->n_work_items, args->work_size,
				test_pfw_fn, array);
	TEST_ASSERT_VAL("parallel_for failure", ret == 0);

	for (i = 0; i < args->n_work_items; i++)
		TEST_ASSERT_VAL("failed array check", array[i] == i+1);

	ret = __workqueue__teardown(wq);
	if (ret)
		goto out;

out:
	free(array);

	return TEST_OK;
}

static const struct threadpool_test_args_t threadpool_test_args[] = {
	{
		.pool_size = 1
	},
	{
		.pool_size = 2
	},
	{
		.pool_size = 4
	},
	{
		.pool_size = 8
	},
	{
		.pool_size = 16
	},
	{
		.pool_size = 0	// sysconf(_SC_NPROCESSORS_ONLN)
	}
};

static const struct workqueue_test_args_t workqueue_test_args[] = {
	{
		.pool_size = 1,
		.n_work_items = 1
	},
	{
		.pool_size = 1,
		.n_work_items = 10
	},
	{
		.pool_size = 2,
		.n_work_items = 1
	},
	{
		.pool_size = 2,
		.n_work_items = 100
	},
	{
		.pool_size = 16,
		.n_work_items = 7
	},
	{
		.pool_size = 16,
		.n_work_items = 2789
	},
	{
		.pool_size = 0,	// sysconf(_SC_NPROCESSORS_ONLN)
		.n_work_items = 8191
	}
};

static const struct parallel_for_test_args_t parallel_for_test_args[] = {
	{
		.pool_size = 1,
		.n_work_items = 1,
		.work_size = 1
	},
	{
		.pool_size = 1,
		.n_work_items = 10,
		.work_size = 3
	},
	{
		.pool_size = 2,
		.n_work_items = 1,
		.work_size = 1
	},
	{
		.pool_size = 2,
		.n_work_items = 100,
		.work_size = 10
	},
	{
		.pool_size = 16,
		.n_work_items = 7,
		.work_size = 2
	},
	{
		.pool_size = 16,
		.n_work_items = 2789,
		.work_size = 16
	},
	{
		.pool_size = 0,	// sysconf(_SC_NPROCESSORS_ONLN)
		.n_work_items = 8191,
		.work_size = 17
	}
};

struct test_case {
	const char *desc;
	int (*func)(void *args);
	void *args;
	int n_args;
	int arg_size;
};

static struct test_case workqueue_testcase_table[] = {
	{
		.desc = "Threadpool",
		.func = __test__threadpool,
		.args = (void *) threadpool_test_args,
		.n_args = (int)ARRAY_SIZE(threadpool_test_args),
		.arg_size = sizeof(struct threadpool_test_args_t)
	},
	{
		.desc = "Workqueue",
		.func = __test__workqueue,
		.args = (void *) workqueue_test_args,
		.n_args = (int)ARRAY_SIZE(workqueue_test_args),
		.arg_size = sizeof(struct workqueue_test_args_t)
	},
	{
		.desc = "Workqueue parallel-for",
		.func = __test__parallel_for,
		.args = (void *) parallel_for_test_args,
		.n_args = (int)ARRAY_SIZE(parallel_for_test_args),
		.arg_size = sizeof(struct parallel_for_test_args_t)
	}
};


int test__workqueue(struct test *test __maybe_unused, int i)
{
	int j, ret;
	struct test_case *tc;

	if (i < 0 || i >= (int)ARRAY_SIZE(workqueue_testcase_table))
		return TEST_FAIL;

	tc = &workqueue_testcase_table[i];

	for (j = 0; j < tc->n_args; j++) {
		ret = tc->func((void *)((char *)tc->args + (j*tc->arg_size)));
		if (ret)
			return ret;
	}

	return TEST_OK;
}


int test__workqueue_subtest_get_nr(void)
{
	return (int)ARRAY_SIZE(workqueue_testcase_table);
}

const char *test__workqueue_subtest_get_desc(int i)
{
	if (i < 0 || i >= (int)ARRAY_SIZE(workqueue_testcase_table))
		return NULL;
	return workqueue_testcase_table[i].desc;
}

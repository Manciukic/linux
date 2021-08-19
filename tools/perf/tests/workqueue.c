// SPDX-License-Identifier: GPL-2.0
#include <unistd.h>
#include <stdlib.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/zalloc.h>
#include "tests.h"
#include "util/debug.h"
#include "util/workqueue/threadpool.h"
#include "util/workqueue/workqueue.h"

#define DUMMY_FACTOR 100000
#define N_DUMMY_WORK_SIZES 7

struct test_task {
	struct task_struct task;
	int n_threads;
	int *array;
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

	dummy_work(tidx);
	mtask->array[tidx] = tidx+1;
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
	int ret = threadpool__set_task(pool, task);

	TEST_ASSERT_VAL("threadpool execute failure", ret == 0);
	TEST_ASSERT_VAL("threadpool is not executing", threadpool__is_busy(pool));

	ret = threadpool__wait(pool);
	TEST_ASSERT_VAL("threadpool wait failure", ret == 0);
	TEST_ASSERT_VAL("waited threadpool is not running", threadpool__is_running(pool));

	return TEST_OK;
}

static int __test__threadpool(int pool_size)
{
	struct threadpool *pool;
	struct test_task task;
	int i, ret;

	if (!pool_size)
		pool_size = sysconf(_SC_NPROCESSORS_ONLN);

	ret = __threadpool__prepare(&pool, pool_size);
	if (ret)
		goto out;

	task.task.fn = test_task_fn1;
	task.n_threads = pool_size;
	task.array = calloc(pool_size, sizeof(*task.array));
	TEST_ASSERT_VAL("calloc failure", task.array);

	ret = __threadpool__exec_wait(pool, &task.task);
	if (ret)
		goto out;

	for (i = 0; i < pool_size; i++)
		TEST_ASSERT_VAL("failed array check (1)", task.array[i] == i+1);

	task.task.fn = test_task_fn2;

	ret = __threadpool__exec_wait(pool, &task.task);
	if (ret)
		goto out;

	for (i = 0; i < pool_size; i++)
		TEST_ASSERT_VAL("failed array check (2)", task.array[i] == 2*i);

	ret = __threadpool__teardown(pool);
	if (ret)
		goto out;

out:
	free(task.array);
	return ret;
}

static const struct threadpool_test_args_t {
	int pool_size;
} threadpool_test_args[] = {
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

static int test__threadpool(struct test_suite *test __maybe_unused,
			    int subtest __maybe_unused)
{
	int i, ret;

	for (i = 0; i < (int)ARRAY_SIZE(threadpool_test_args); i++) {
		ret = __test__threadpool(threadpool_test_args[i].pool_size);
		if (ret)
			return ret;
	}

	return TEST_OK;
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


static int __test__workqueue(int pool_size, int n_work_items)
{
	struct workqueue_struct *wq;
	struct test_work *works;
	int *array;
	int i, ret;

	if (!pool_size)
		pool_size = sysconf(_SC_NPROCESSORS_ONLN);

	ret = __workqueue__prepare(&wq, pool_size);
	if (ret)
		return ret;

	array = calloc(n_work_items, sizeof(*array));
	TEST_ASSERT_VAL("failed array calloc", array);
	works = calloc(n_work_items, sizeof(*works));
	TEST_ASSERT_VAL("failed works calloc", works);

	ret = __workqueue__exec_wait(wq, array, works, test_work_fn1,
					n_work_items);
	if (ret)
		goto out;

	for (i = 0; i < n_work_items; i++)
		TEST_ASSERT_VAL("failed array check (1)", array[i] == i+1);

	ret = __workqueue__exec_wait(wq, array, works, test_work_fn2,
					n_work_items);
	if (ret)
		goto out;

	for (i = 0; i < n_work_items; i++)
		TEST_ASSERT_VAL("failed array check (2)", array[i] == 2*i);

	ret = __workqueue__teardown(wq);
	if (ret)
		goto out;

out:
	free(array);
	free(works);
	return ret;
}

static const struct workqueue_test_args_t {
	int pool_size;
	int n_work_items;
} workqueue_test_args[] = {
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

static int test__workqueue(struct test_suite *test __maybe_unused,
			    int subtest __maybe_unused)
{
	int i, ret;

	for (i = 0; i < (int)ARRAY_SIZE(workqueue_test_args); i++) {
		ret = __test__workqueue(workqueue_test_args[i].pool_size,
					 workqueue_test_args[i].n_work_items);
		if (ret)
			return ret;
	}

	return TEST_OK;
}

static struct test_case workqueue_tests[] = {
	TEST_CASE("Threadpool", threadpool),
	TEST_CASE("Workqueue", workqueue),
	{ .name = NULL, }
};

struct test_suite suite__workqueue = {
	.desc = "Test workqueue lib",
	.test_cases = workqueue_tests,
};

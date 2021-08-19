// SPDX-License-Identifier: GPL-2.0
#include <unistd.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include "tests.h"
#include "util/debug.h"
#include "util/workqueue/threadpool.h"

static int __threadpool__prepare(struct threadpool **pool, int pool_size)
{
	*pool = threadpool__new(pool_size);
	TEST_ASSERT_VAL("threadpool creation failure", !IS_ERR(*pool));
	TEST_ASSERT_VAL("threadpool size is wrong",
			threadpool__size(*pool) == pool_size);

	return TEST_OK;
}

static int __threadpool__teardown(struct threadpool *pool)
{
	threadpool__delete(pool);

	return TEST_OK;
}

static int __test__threadpool(int pool_size)
{
	struct threadpool *pool;
	int ret;

	if (!pool_size)
		pool_size = sysconf(_SC_NPROCESSORS_ONLN);

	ret = __threadpool__prepare(&pool, pool_size);
	if (ret)
		goto out;

	ret = __threadpool__teardown(pool);
	if (ret)
		goto out;

out:
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

static struct test_case workqueue_tests[] = {
	TEST_CASE("Threadpool", threadpool),
	{ .name = NULL, }
};

struct test_suite suite__workqueue = {
	.desc = "Test workqueue lib",
	.test_cases = workqueue_tests,
};

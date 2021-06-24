/* SPDX-License-Identifier: GPL-2.0 */
/* For debugging general purposes */
#ifndef __PERF_DEBUG_H
#define __PERF_DEBUG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <linux/compiler.h>

#ifdef __CHECKER__
#undef __user
#undef __force
# define __user     __attribute__((noderef, address_space(1)))
# define __kernel   __attribute__((address_space(0)))
# define __safe     __attribute__((safe))
# define __force    __attribute__((force))
# define __nocast   __attribute__((nocast))
# define __iomem    __attribute__((noderef, address_space(2)))
# define __must_hold(x) __attribute__((context(x,1,1)))
# define __acquires(x)  __attribute__((context(x,0,1)))
# define __releases(x)  __attribute__((context(x,1,0)))
# define __acquire(x)   __context__(x,1)
# define __release(x)   __context__(x,-1)
# define __cond_lock(x,c)   ((c) ? ({ __acquire(x); 1; }) : 0)
# define __percpu   __attribute__((noderef, address_space(3)))
# define __pmem     __attribute__((noderef, address_space(5)))
#ifdef CONFIG_SPARSE_RCU_POINTER
# define __rcu      __attribute__((noderef, address_space(4)))
#else
# define __rcu
#endif
extern void __chk_user_ptr(const volatile void __user *);
extern void __chk_io_ptr(const volatile void __iomem *);
#else
# define __user
# define __kernel
# define __safe
# define __force
# define __nocast
# define __iomem
# define __chk_user_ptr(x) (void)0
# define __chk_io_ptr(x) (void)0
# define __builtin_warning(x, y...) (1)
# define __must_hold(x)
# define __acquires(x)
# define __releases(x)
# define __acquire(x) (void)0
# define __release(x) (void)0
# define __cond_lock(x,c) (c)
# define __percpu
# define __rcu
# define __pmem
#endif

extern int verbose;
extern int debug_peo_args;
extern bool quiet, dump_trace;
extern int debug_ordered_events;
extern int debug_data_convert;

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

#define pr_err(fmt, ...) \
	eprintf(0, verbose, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warning(fmt, ...) \
	eprintf(0, verbose, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...) \
	eprintf(0, verbose, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug(fmt, ...) \
	eprintf(1, verbose, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debugN(n, fmt, ...) \
	eprintf(n, verbose, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug2(fmt, ...) pr_debugN(2, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug3(fmt, ...) pr_debugN(3, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_debug4(fmt, ...) pr_debugN(4, pr_fmt(fmt), ##__VA_ARGS__)

/* Special macro to print perf_event_open arguments/return value. */
#define pr_debug2_peo(fmt, ...) {				\
	if (debug_peo_args)						\
		pr_debugN(0, pr_fmt(fmt), ##__VA_ARGS__);	\
	else							\
		pr_debugN(2, pr_fmt(fmt), ##__VA_ARGS__);	\
}

#define pr_time_N(n, var, t, fmt, ...) \
	eprintf_time(n, var, t, fmt, ##__VA_ARGS__)

#define pr_oe_time(t, fmt, ...)  pr_time_N(1, debug_ordered_events, t, pr_fmt(fmt), ##__VA_ARGS__)
#define pr_oe_time2(t, fmt, ...) pr_time_N(2, debug_ordered_events, t, pr_fmt(fmt), ##__VA_ARGS__)

#define STRERR_BUFSIZE	128	/* For the buffer size of str_error_r */

union perf_event;

int dump_printf(const char *fmt, ...) __printf(1, 2);
void trace_event(union perf_event *event);

int ui__error(const char *format, ...) __printf(1, 2);
int ui__warning(const char *format, ...) __printf(1, 2);

void pr_stat(const char *fmt, ...);

int eprintf(int level, int var, const char *fmt, ...) __printf(3, 4);
int eprintf_time(int level, int var, u64 t, const char *fmt, ...) __printf(4, 5);
int veprintf(int level, int var, const char *fmt, va_list args);

int perf_debug_option(const char *str);
void debug_set_file(FILE *file);
void debug_set_display_time(bool set);
void perf_debug_setup(void);
int perf_quiet_option(void);

void dump_stack(void);
void sighandler_dump_stack(int sig);

#endif	/* __PERF_DEBUG_H */

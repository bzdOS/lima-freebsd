/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * lima_trace.h — tracepoints stub for Lima on FreeBSD 15.1
 *
 * Linux TRACE_EVENT() maps to ftrace/perf; FreeBSD has no equivalent.
 * Define all Lima tracepoints as no-ops.
 */

#ifndef __LIMA_TRACE_H__
#define __LIMA_TRACE_H__

struct lima_sched_task;

static inline void trace_lima_task_submit(struct lima_sched_task *task) {}
static inline void trace_lima_task_run(struct lima_sched_task *task) {}
static inline void trace_lima_task_fini(struct lima_sched_task *task) {}

#endif /* __LIMA_TRACE_H__ */

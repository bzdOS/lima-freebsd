/* SPDX-License-Identifier: GPL-2.0 OR MIT */
/*
 * lima_dump.h — GPU crash-dump format for Lima (Mali-400/450)
 *
 * Ported from Linux 6.6 drivers/gpu/drm/lima/lima_dump.h
 * Used for error-task serialisation: lima_device.dump (head counters)
 * and lima_sched_save_task_state() (task/chunk layout).
 */

#ifndef __LIMA_DUMP_H__
#define __LIMA_DUMP_H__

#include <linux/types.h>

/* File-level magic / version */
#define LIMA_DUMP_MAGIC		0x414d494c	/* "LIMA" LE */
#define LIMA_DUMP_MAJOR		1
#define LIMA_DUMP_MINOR		1

/* Chunk type IDs */
#define LIMA_DUMP_CHUNK_FRAME		0
#define LIMA_DUMP_CHUNK_PROCESS_NAME	1
#define LIMA_DUMP_CHUNK_PROCESS_ID	2
#define LIMA_DUMP_CHUNK_BUFFER		3

/*
 * lima_dump_head — file header, embedded in lima_device.dump.
 * size / num_tasks are accumulated as error tasks are appended.
 */
struct lima_dump_head {
	u32 magic;
	u16 version_major;
	u16 version_minor;
	u32 size;
	u32 num_tasks;
};

/*
 * lima_dump_task — one GPU job entry; immediately followed by chunks.
 * id: pipe id (lima_pipe_gp or lima_pipe_pp).
 */
struct lima_dump_task {
	u32 id;
	u32 size;       /* total bytes of all chunks that follow */
	u32 num_chunks;
};

/*
 * lima_dump_chunk — generic chunk header; payload follows immediately.
 */
struct lima_dump_chunk {
	u32 id;
	u32 size;
};

/*
 * lima_dump_chunk_pid — LIMA_DUMP_CHUNK_PROCESS_ID payload (no extra data).
 */
struct lima_dump_chunk_pid {
	u32 id;
	u32 size;
	u32 pid;
};

/*
 * lima_dump_chunk_buffer — LIMA_DUMP_CHUNK_BUFFER header; BO bytes follow.
 */
struct lima_dump_chunk_buffer {
	u32 id;
	u32 size;
	u64 va;         /* GPU virtual address of this BO */
};

#endif /* __LIMA_DUMP_H__ */

/* SPDX-License-Identifier: MIT */
#ifndef B1NIX_GPU_SCHEDULER_H
#define B1NIX_GPU_SCHEDULER_H

#include <b1nix/dma_fence.h>
#include <b1nix/spinlock.h>
#include <b1nix/types.h>

/*
 * M100 — a minimal drm_gpu_scheduler.
 *
 * One kernel thread owns the hardware ring. Submitters push jobs and get a
 * fence back; they never touch the ring themselves and never spin on it. That
 * is the whole point:
 *
 *   - Submission stops being synchronous. A task that submits work can wait on
 *     the fence (and be descheduled) or carry on and wait later.
 *   - The ring has exactly one writer, so ordering and the doorbell no longer
 *     need a lock held across the device round trip.
 *   - Per-entity FIFOs with round-robin between entities keep one busy client
 *     from starving another, which a single global queue cannot do.
 *
 * The driver supplies one callback, `run_job`, which pushes the job at the
 * hardware and returns 0 on success. It runs in the scheduler thread, so it may
 * sleep. The scheduler signals the job's `finished` fence with that return
 * value once run_job returns; a driver with asynchronous completion instead
 * signals the fence itself from its interrupt handler and returns
 * DRM_SCHED_RUN_ASYNC.
 */

struct drm_gpu_scheduler;
struct drm_sched_job;
struct drm_sched_entity;

/* run_job return: work finished (or failed) by the time run_job returned. */
#define DRM_SCHED_RUN_DONE  0
/* run_job return: work is in flight; the driver will signal job->finished. */
#define DRM_SCHED_RUN_ASYNC 1

struct drm_sched_backend_ops {
	/* Push `job` at the hardware. Return DRM_SCHED_RUN_DONE,
	 * DRM_SCHED_RUN_ASYNC, or a negative errno. */
	int (*run_job)(struct drm_sched_job *job);
	/* Optional: release driver-private state after the job's fence is put. */
	void (*free_job)(struct drm_sched_job *job);
};

struct drm_sched_job {
	struct dma_fence finished;   /* signalled when the job is complete */
	struct drm_sched_entity *entity;
	struct drm_gpu_scheduler *sched;
	struct drm_sched_job *next;  /* entity FIFO linkage */
	struct dma_fence *dependency; /* optional: wait for this before running */
	void *driver_data;
	u64 id;
};

struct drm_sched_entity {
	struct drm_gpu_scheduler *sched;
	struct drm_sched_job *head;
	struct drm_sched_job *tail;
	struct drm_sched_entity *next; /* scheduler's entity ring */
	u64 context;                   /* fence timeline for this entity */
	u64 next_seqno;
	u64 submitted;
	u64 completed;
	const char *name;
};

struct drm_gpu_scheduler {
	const char *name;
	const struct drm_sched_backend_ops *ops;
	struct drm_sched_entity *entities; /* circular-ish list, round-robined */
	struct drm_sched_entity *cursor;   /* next entity to service */
	spinlock_t lock;
	volatile int stop;
	volatile int alive;
	volatile u64 ran;      /* jobs whose run_job returned */
	volatile u64 pushed;   /* jobs accepted */
	u64 next_job_id;
};

/* Bring up the scheduler and its thread. Returns 0, or -ENOMEM when the thread
 * could not be created (in which case the caller must fall back to synchronous
 * submission rather than silently dropping work). */
int drm_sched_init(struct drm_gpu_scheduler *sched,
                   const struct drm_sched_backend_ops *ops, const char *name);

/* Drain and stop. Sleeps. */
void drm_sched_fini(struct drm_gpu_scheduler *sched);

/* Attach an entity (an independent submission stream) to the scheduler. */
int drm_sched_entity_init(struct drm_sched_entity *entity,
                          struct drm_gpu_scheduler *sched, const char *name);
/* Wait for the entity's queue to drain, then detach it. Sleeps. */
void drm_sched_entity_fini(struct drm_sched_entity *entity);

/* Prepare a job for `entity`: initialises its finished fence on the entity's
 * timeline. The caller owns the storage until the fence's last reference goes.
 * Returns 0. */
int drm_sched_job_init(struct drm_sched_job *job, struct drm_sched_entity *e);

/* Make `job` wait for `dep` before run_job is called. The job takes a reference
 * on the dependency. */
void drm_sched_job_add_dependency(struct drm_sched_job *job,
                                  struct dma_fence *dep);

/* Queue the job. Returns its `finished` fence with a reference taken for the
 * caller (release with dma_fence_put), or NULL if the scheduler is stopping. */
struct dma_fence *drm_sched_job_submit(struct drm_sched_job *job);

/* 1 when the scheduler thread is running and accepting work. */
int drm_sched_ready(struct drm_gpu_scheduler *sched);

/* M100 in-kernel self-test for the fence + scheduler pair. Emits M100-SMOKE
 * markers; no-op outside b1nix.test=1. */
void drm_sched_selftest(void);

#endif

/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * M100 — minimal drm_gpu_scheduler. See kernel/include/b1nix/gpu_scheduler.h.
 */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/gpu_scheduler.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <string.h>

/* Pick the next job to run, round-robining across entities so one busy stream
 * cannot starve another. Caller must not hold the lock. */
static struct drm_sched_job *sched_pick(struct drm_gpu_scheduler *sched)
{
	u64 flags;
	spin_lock_irqsave(&sched->lock, &flags);

	struct drm_sched_entity *start = sched->cursor ? sched->cursor
	                                               : sched->entities;
	struct drm_sched_entity *e = start;
	struct drm_sched_job *job = 0;

	while (e) {
		if (e->head) {
			job = e->head;
			e->head = job->next;
			if (!e->head)
				e->tail = 0;
			job->next = 0;
			/* Resume from the entity after this one next time. */
			sched->cursor = e->next ? e->next : sched->entities;
			break;
		}
		e = e->next ? e->next : sched->entities;
		if (e == start)
			break;
	}

	spin_unlock_irqrestore(&sched->lock, flags);
	return job;
}

static int sched_has_work(struct drm_gpu_scheduler *sched)
{
	u64 flags;
	int any = 0;
	spin_lock_irqsave(&sched->lock, &flags);
	for (struct drm_sched_entity *e = sched->entities; e; e = e->next) {
		if (e->head) {
			any = 1;
			break;
		}
	}
	spin_unlock_irqrestore(&sched->lock, flags);
	return any;
}

static void sched_thread(void *arg)
{
	struct drm_gpu_scheduler *sched = arg;
	sched->alive = 1;

	while (!sched->stop) {
		struct drm_sched_job *job = sched_pick(sched);
		if (!job) {
			scheduler_wait_prepare_timeout(sched, 10);
			if (sched_has_work(sched) || sched->stop)
				scheduler_wait_cancel();
			else
				scheduler_wait_commit();
			continue;
		}

		/* Dependencies are resolved here, in the scheduler thread, so the
		 * submitter never blocks on another client's work. */
		if (job->dependency) {
			dma_fence_wait_uninterruptible(job->dependency);
			dma_fence_put(job->dependency);
			job->dependency = 0;
		}

		int rc = sched->ops && sched->ops->run_job ? sched->ops->run_job(job)
		                                           : -ENODEV;
		sched->ran++;

		/* Everything that reads the job body happens BEFORE the fence is
		 * signalled. The instant it signals, the submitter is free to run —
		 * and a submitter whose job lives on its own stack (virtio-gpu's does
		 * not, but a future one might) would tear that storage down. The only
		 * post-signal access left is the fence itself, which the submitter's
		 * own reference keeps alive. */
		struct dma_fence *fence = &job->finished;
		struct drm_sched_entity *e = job->entity;
		if (e) {
			u64 flags;
			spin_lock_irqsave(&sched->lock, &flags);
			e->completed++;
			spin_unlock_irqrestore(&sched->lock, flags);
		}

		if (rc == DRM_SCHED_RUN_ASYNC) {
			/* The driver signals job->finished from its completion path. */
		} else if (rc < 0) {
			dma_fence_signal_error(fence, rc);
		} else {
			dma_fence_signal(fence);
		}

		if (e)
			scheduler_wake_all((void *)(usize)&e->completed);
		/* The scheduler's own reference. Dropping it may free the embedding
		 * job through the fence's release callback, so nothing may touch the
		 * job afterwards. */
		dma_fence_put(fence);
	}

	sched->alive = 0;
	scheduler_wake_all((void *)(usize)&sched->ran);
	scheduler_exit_current(0);
}

int drm_sched_init(struct drm_gpu_scheduler *sched,
                   const struct drm_sched_backend_ops *ops, const char *name)
{
	if (!sched || !ops || !ops->run_job)
		return -EINVAL;
	memset(sched, 0, sizeof(*sched));
	sched->ops = ops;
	sched->name = name ? name : "drm-sched";
	sched->lock = SPINLOCK_INIT;
	sched->next_job_id = 1;

	if (kthread_create(sched->name, sched_thread, sched) < 0)
		return -ENOMEM;
	return 0;
}

int drm_sched_ready(struct drm_gpu_scheduler *sched)
{
	return sched && sched->alive && !sched->stop;
}

void drm_sched_fini(struct drm_gpu_scheduler *sched)
{
	if (!sched)
		return;
	sched->stop = 1;
	scheduler_wake_all(sched);
	for (int i = 0; i < 1000 && sched->alive; i++) {
		if (!scheduler_can_block())
			break;
		scheduler_sleep_ticks(1);
	}
}

int drm_sched_entity_init(struct drm_sched_entity *entity,
                          struct drm_gpu_scheduler *sched, const char *name)
{
	if (!entity || !sched)
		return -EINVAL;
	memset(entity, 0, sizeof(*entity));
	entity->sched = sched;
	entity->name = name ? name : "entity";
	entity->context = dma_fence_context_alloc(1);
	entity->next_seqno = 1;

	u64 flags;
	spin_lock_irqsave(&sched->lock, &flags);
	entity->next = sched->entities;
	sched->entities = entity;
	if (!sched->cursor)
		sched->cursor = entity;
	spin_unlock_irqrestore(&sched->lock, flags);
	return 0;
}

void drm_sched_entity_fini(struct drm_sched_entity *entity)
{
	if (!entity || !entity->sched)
		return;
	struct drm_gpu_scheduler *sched = entity->sched;

	/* Wait for everything this entity submitted to complete before unlinking
	 * it — the scheduler thread still dereferences the entity for accounting. */
	while (entity->completed < entity->submitted) {
		if (!scheduler_can_block())
			break;
		scheduler_wait_prepare_timeout((void *)(usize)&entity->completed, 1);
		if (entity->completed >= entity->submitted)
			scheduler_wait_cancel();
		else
			scheduler_wait_commit();
	}

	u64 flags;
	spin_lock_irqsave(&sched->lock, &flags);
	struct drm_sched_entity **pp = &sched->entities;
	while (*pp) {
		if (*pp == entity) {
			*pp = entity->next;
			break;
		}
		pp = &(*pp)->next;
	}
	if (sched->cursor == entity)
		sched->cursor = sched->entities;
	spin_unlock_irqrestore(&sched->lock, flags);
	entity->sched = 0;
}

int drm_sched_job_init(struct drm_sched_job *job, struct drm_sched_entity *e)
{
	if (!job || !e || !e->sched)
		return -EINVAL;
	job->entity = e;
	job->sched = e->sched;
	job->next = 0;
	job->dependency = 0;
	job->id = 0;
	dma_fence_init_named(&job->finished, e->context, e->next_seqno++, "sched-job");
	return 0;
}

void drm_sched_job_add_dependency(struct drm_sched_job *job,
                                  struct dma_fence *dep)
{
	if (!job || !dep)
		return;
	/* One dependency slot is all the current submitters need; a job arriving
	 * with a second would silently lose the first, so wait the earlier one out
	 * here rather than dropping it. */
	if (job->dependency) {
		dma_fence_wait_uninterruptible(job->dependency);
		dma_fence_put(job->dependency);
	}
	job->dependency = dma_fence_get(dep);
}

struct dma_fence *drm_sched_job_submit(struct drm_sched_job *job)
{
	if (!job || !job->entity || !job->sched)
		return 0;
	struct drm_gpu_scheduler *sched = job->sched;
	if (sched->stop)
		return 0;

	/* Two references beyond the creator's: one for the scheduler thread (which
	 * puts it after running the job) and one returned to the caller. */
	dma_fence_get(&job->finished);
	struct dma_fence *ret = dma_fence_get(&job->finished);

	u64 flags;
	spin_lock_irqsave(&sched->lock, &flags);
	job->id = sched->next_job_id++;
	struct drm_sched_entity *e = job->entity;
	job->next = 0;
	if (e->tail)
		e->tail->next = job;
	else
		e->head = job;
	e->tail = job;
	e->submitted++;
	sched->pushed++;
	spin_unlock_irqrestore(&sched->lock, flags);

	scheduler_wake_all(sched);
	return ret;
}

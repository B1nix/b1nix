/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Did the GT come up, and will it run anything?
 *
 * The display half of this driver proves itself by putting a picture on a
 * panel. The GT half has no such witness: GGTT, PPGTT, contexts and execlists
 * can all initialise without complaint and still submit nothing, and a driver
 * that reports no error while executing no work reads exactly like one that
 * works. Inference from register dumps took the display bring-up a long way and
 * cost days doing it; this asks the engine instead.
 *
 * Three questions, in order of what each one settles:
 *
 *   1. Which engines did the driver find, and what does each submit through —
 *      execlists or a legacy ring? That is one line of log against which every
 *      later claim about submission can be checked.
 *   2. How much GGTT is there, and is full PPGTT in use? A GT with per-context
 *      page tables is the one Mesa expects; aliasing PPGTT is a different
 *      machine wearing the same name.
 *   3. Does a request submitted to an engine's own kernel context retire? This
 *      is the whole path end to end — a ring in the GGTT, a context image, a
 *      submission through execlists, an interrupt, a fence signalled — and it
 *      either completes within the timeout or it does not.
 *
 * Nothing here is a substitute for running Mesa. It is the step before it: when
 * iris fails, this says whether it failed on a GT that was already unable to
 * retire an empty request.
 */

#include "i915_drv.h"
#include "i915_request.h"
#include "gt/intel_engine.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_gt.h"
#include "gt/intel_gtt.h"
#include "gt/intel_context.h"
#include "gt/intel_gt_pm.h"
#include "i915_reg.h"
#include "intel_uncore.h"

#include <drm/drm_print.h>

#include <drm/drm_device.h>
#include <linux/printk.h>
#include <lkpi/env.h>
#include <linux/delay.h>

static struct drm_i915_private *gt_probe_i915(struct drm_device *dev)
{
	_Static_assert(offsetof(struct drm_i915_private, drm) == 0,
	               "drm_i915_private must start with its drm_device");
	return (struct drm_i915_private *)dev;
}

/* One line per engine: what it is called, and how work reaches it. */
static void gt_probe_engines(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	unsigned int count = 0;

	for_each_engine(engine, gt, id) {
		count++;
		pr_info("i915-gt: engine %s class=%u instance=%u submission=%s\n",
		        engine->name, engine->uabi_class, engine->uabi_instance,
		        engine->execlists.submit_reg ? "execlists" : "ring");
	}
	pr_info("i915-gt: %u engine(s)\n", count);
}

/* What the address space looks like: GGTT size, and the PPGTT the parts claim. */
static void gt_probe_address_space(struct drm_i915_private *i915,
                                   struct intel_gt *gt)
{
	struct i915_ggtt *ggtt = gt->ggtt;

	pr_info("i915-gt: ggtt total=%llu MiB mappable=%llu MiB\n",
	        ggtt ? (unsigned long long)(ggtt->vm.total >> 20) : 0ull,
	        ggtt ? (unsigned long long)(ggtt->mappable_end >> 20) : 0ull);
	/* Runtime info, not static: the PPGTT a part is capable of and the one it
	 * ended up with are different fields, and only the runtime one is true of
	 * this machine. */
	pr_info("i915-gt: ppgtt type=%d size=%u bits\n",
	        (int)INTEL_PPGTT(i915),
	        (unsigned)RUNTIME_INFO(i915)->ppgtt_size);
}

/*
 * Submit nothing, and see whether nothing comes back.
 *
 * An empty request still carries the whole machine: the engine's kernel context
 * is pinned, its ring lives in the GGTT, the context image is loaded by the
 * hardware, and the breadcrumb the request writes has to be seen by the
 * interrupt that signals its fence. A timeout here is not a subtle failure —
 * it means the GT accepted work and never finished it.
 */
static int gt_probe_nop(struct intel_gt *gt, int *executed_out, int *engines_out)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int failures = 0;
	int executed = 0;
	int engines = 0;
	struct intel_engine_cs *unprompted_engine = 0;

	for_each_engine(engine, gt, id) {
		struct i915_request *rq;
		long left;

		engines++;
		unprompted_engine = unprompted_engine ? unprompted_engine : engine;
		if (!engine->kernel_context) {
			pr_err("i915-gt: %s has no kernel context\n", engine->name);
			failures++;
			continue;
		}

		rq = i915_request_create(engine->kernel_context);
		if (IS_ERR(rq)) {
			pr_err("i915-gt: %s request_create failed (%ld)\n",
			       engine->name, PTR_ERR(rq));
			failures++;
			continue;
		}

		i915_request_get(rq);
		i915_request_add(rq);

		/*
		 * Execution and notification are two claims, and they fail
		 * apart. Poll the request first: that reads the breadcrumb the
		 * engine writes, so it answers "did the GPU run this" without
		 * involving an interrupt, a fence callback or a wakeup. Only
		 * then wait properly, which answers "was I told".
		 */
		{
			u64 spent_ms = 0;

			while (!i915_request_completed(rq) && spent_ms < 2000) {
				msleep(1);
				spent_ms += 10;   /* msleep rounds up to a jiffy */
			}
			if (i915_request_completed(rq)) {
				executed++;
				pr_info("i915-gt: %s executed (within %llu ms)\n",
				        engine->name, (unsigned long long)spent_ms);
			}
			else
				pr_err("i915-gt: %s never executed\n", engine->name);
		}

		/* Two seconds. A GT that is running retires an empty request in
		 * microseconds; one that is not will not be helped by more time. */
		left = i915_request_wait(rq, 0, msecs_to_jiffies(2000));
		if (left < 0) {
			struct drm_printer pr = drm_info_printer(engine->i915->drm.dev);

			/* Did the hardware run it and only fail to say so?
			 *
			 * Those are two different faults with one symptom. If the
			 * request is complete by the time the wait gives up, the
			 * engine executed the batch and the breadcrumb landed —
			 * what is missing is the interrupt that signals the fence.
			 * If it is not complete, nothing ran, and the ring, the
			 * context image or the GGTT behind them is where to look.
			 * The engine dump carries both answers: ring head/tail,
			 * the HWSP seqno the hardware wrote, and what execlists
			 * believes it has in its ports. */
			pr_err("i915-gt: %s nop request did not retire (%ld), completed=%d\n",
			       engine->name, left, i915_request_completed(rq));
			intel_engine_dump(engine, &pr, "i915-gt %s: ", engine->name);
			failures++;
		} else {
			pr_info("i915-gt: %s nop request retired\n", engine->name);
		}
		i915_request_put(rq);
	}

	/*
	 * And once more on one engine, with nobody looking.
	 *
	 * Everything above polls the request before waiting on it, and i915's
	 * fence reports itself signalled when asked — so a poll is enough to make
	 * the wait that follows succeed. That does not prove the driver is ever
	 * TOLD a request finished, which is what an interrupt-driven fence is for
	 * and what any real client depends on: a compositor blocked on a fence has
	 * nobody polling on its behalf. So: submit, wait, and touch nothing.
	 */
	if (unprompted_engine && unprompted_engine->kernel_context) {
		struct i915_request *rq;

		/*
		 * Hold the GT awake across this.
		 *
		 * Breadcrumbs arm the completion interrupt only through
		 * intel_gt_pm_get_if_awake(): a parked GT is left alone, on the
		 * grounds that nothing can be running on it. If this makes the
		 * difference, the fault is not in the interrupt path at all — it is
		 * that the GT is never marked awake while a request is in flight.
		 */
		intel_gt_pm_get(gt);
		pr_info("i915-gt: gt awake=%d before unprompted wait\n",
		        intel_gt_pm_is_awake(gt));
		rq = i915_request_create(unprompted_engine->kernel_context);

		if (!IS_ERR(rq)) {
			struct drm_i915_private *i915 = unprompted_engine->i915;
			u64 irqs_before = lkpi_device_irq_count();
			u64 irqs_after;
			u64 q0 = 0, r0 = 0, h0 = 0, q1 = 0, r1 = 0, h1 = 0;
			u64 sg0 = 0, cb0 = 0, sg1 = 0, cb1 = 0, tmp = 0;
			long left;

			lkpi_irq_work_counts(&q0, &r0, &h0);
			lkpi_fence_counts(&tmp, &tmp, &sg0, &cb0);

			i915_request_get(rq);
			i915_request_add(rq);
			/* Right after submission and arming: is this request on the
			 * engine's signaler list? intel_engine_dump prints them under
			 * "Signals:". An empty list here means the arming declined it, and
			 * no interrupt will ever signal it; a listed request that still
			 * times out means the interrupt is the missing half. */
			{
				struct drm_printer pr2 =
					drm_info_printer(unprompted_engine->i915->drm.dev);

				(void)dma_fence_add_callback; /* arming happens in the wait */
				intel_engine_dump(unprompted_engine, &pr2, "armed %s: ",
				                  unprompted_engine->name);
			}
			left = i915_request_wait(rq, 0, msecs_to_jiffies(1000));
			irqs_after = lkpi_device_irq_count();
			lkpi_irq_work_counts(&q1, &r1, &h1);
			lkpi_fence_counts(&tmp, &tmp, &sg1, &cb1);
			/* Deltas across the wait: totals include the whole boot and hide
			 * whether anything happened while this request was outstanding. */
			pr_info("i915-gt: during wait: irqs=%llu claimed=%llu irq_work q=%llu r=%llu signals=%llu cbs=%llu\n",
			        (unsigned long long)(irqs_after - irqs_before),
			        (unsigned long long)(h1 - h0),
			        (unsigned long long)(q1 - q0),
			        (unsigned long long)(r1 - r0),
			        (unsigned long long)(sg1 - sg0),
			        (unsigned long long)(cb1 - cb0));

			/* Whether anything arrived while we waited, and whether
			 * the GT was even allowed to raise it. Those are the two
			 * halves of "nobody told us": an interrupt that never
			 * fires, and one that fires into a masked bit. */
			{
				u64 sched_n = 0, ran_n = 0;

				lkpi_tasklet_counts(&sched_n, &ran_n);
				pr_info("i915-gt: tasklets scheduled=%llu ran=%llu\n",
				        (unsigned long long)sched_n,
				        (unsigned long long)ran_n);
			}
			{
				u64 asked = 0, accepted = 0, signalled = 0, cbs = 0;

				lkpi_fence_counts(&asked, &accepted, &signalled, &cbs);
				pr_info("i915-gt: fences armed=%llu/%llu signalled=%llu callbacks=%llu\n",
				        (unsigned long long)accepted,
				        (unsigned long long)asked,
				        (unsigned long long)signalled,
				        (unsigned long long)cbs);
			}
			{
				u64 q = 0, r = 0, h = 0;

				lkpi_irq_work_counts(&q, &r, &h);
				pr_info("i915-gt: irq_work queued=%llu ran=%llu, irqs claimed=%llu\n",
				        (unsigned long long)q, (unsigned long long)r,
				        (unsigned long long)h);
			}
			pr_info("i915-gt: irqs during wait: %llu\n",
			        (unsigned long long)(irqs_after - irqs_before));
			pr_info("i915-gt: master=%08x gt0 imr=%08x ier=%08x iir=%08x\n",
			        intel_uncore_read(&i915->uncore, GEN8_MASTER_IRQ),
			        intel_uncore_read(&i915->uncore, GEN8_GT_IMR(0)),
			        intel_uncore_read(&i915->uncore, GEN8_GT_IER(0)),
			        intel_uncore_read(&i915->uncore, GEN8_GT_IIR(0)));
			if (left < 0) {
				struct drm_printer pr =
					drm_info_printer(unprompted_engine->i915->drm.dev);

				/* completed but not signalled = the hardware finished and
				 * nothing marked the fence; both false = the request never
				 * ran at all. */
				pr_info("I915-GT: FAILED unprompted-signalling (%s, %ld) completed=%d signalled=%d\n",
				        unprompted_engine->name, left,
				        i915_request_completed(rq),
				        dma_fence_is_signaled(&rq->fence));
				intel_engine_dump(unprompted_engine, &pr, "unprompted %s: ",
				                  unprompted_engine->name);
			}
			else
				pr_info("I915-GT: ok unprompted-signalling (%s)\n",
				        unprompted_engine->name);
			i915_request_put(rq);
		}
		intel_gt_pm_put(gt);
	}

	*executed_out = executed;
	*engines_out = engines;
	return failures;
}

/* Entry point, called from the boot once the driver has bound. */
void lkpi_i915_gt_probe(struct drm_device *dev)
{
	struct drm_i915_private *i915;
	struct intel_gt *gt;
	int failures, executed = 0, engines = 0;

	if (!dev || !lkpi_bootflag("b1nix.i915-gt-probe"))
		return;

	i915 = gt_probe_i915(dev);
	gt = to_gt(i915);
	if (!gt) {
		pr_err("i915-gt: no GT on this device\n");
		pr_info("I915-GT: FAILED no-gt\n");
		return;
	}

	pr_info("I915-GT: begin\n");
	gt_probe_engines(gt);
	gt_probe_address_space(i915, gt);
	failures = gt_probe_nop(gt, &executed, &engines);

	/* Two claims, two markers. The GT executing submitted work and the driver
	 * being told about it are different things, and reporting them as one hid
	 * which of the two was missing. */
	if (executed == engines)
		pr_info("I915-GT: ok execution (%d engine(s))\n", executed);
	else
		pr_info("I915-GT: FAILED execution (%d of %d engine(s))\n",
		        executed, engines);
	if (failures == 0)
		pr_info("I915-GT: ok signalling\n");
	else
		pr_info("I915-GT: FAILED signalling (%d engine(s))\n", failures);
}

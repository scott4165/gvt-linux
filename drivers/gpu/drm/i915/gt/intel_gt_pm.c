/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_params.h"
#include "intel_context.h"
#include "intel_engine_pm.h"
#include "intel_gt.h"
#include "intel_gt_pm.h"
#include "intel_pm.h"
#include "intel_wakeref.h"

static void pm_notify(struct intel_gt *gt, int state)
{
	blocking_notifier_call_chain(&gt->pm_notifications, state, gt->i915);
}

static int __gt_unpark(struct intel_wakeref *wf)
{
	struct intel_gt *gt = container_of(wf, typeof(*gt), wakeref);
	struct drm_i915_private *i915 = gt->i915;

	GEM_TRACE("\n");

	/*
	 * It seems that the DMC likes to transition between the DC states a lot
	 * when there are no connected displays (no active power domains) during
	 * command submission.
	 *
	 * This activity has negative impact on the performance of the chip with
	 * huge latencies observed in the interrupt handler and elsewhere.
	 *
	 * Work around it by grabbing a GT IRQ power domain whilst there is any
	 * GT activity, preventing any DC state transitions.
	 */
	gt->awake = intel_display_power_get(i915, POWER_DOMAIN_GT_IRQ);
	GEM_BUG_ON(!gt->awake);

	intel_enable_gt_powersave(i915);

	i915_update_gfx_val(i915);
	if (INTEL_GEN(i915) >= 6)
		gen6_rps_busy(i915);

	i915_pmu_gt_unparked(i915);

	intel_gt_queue_hangcheck(gt);

	pm_notify(gt, INTEL_GT_UNPARK);

	return 0;
}

static int __gt_park(struct intel_wakeref *wf)
{
	struct intel_gt *gt = container_of(wf, typeof(*gt), wakeref);
	intel_wakeref_t wakeref = fetch_and_zero(&gt->awake);
	struct drm_i915_private *i915 = gt->i915;

	GEM_TRACE("\n");

	pm_notify(gt, INTEL_GT_PARK);

	i915_pmu_gt_parked(i915);
	if (INTEL_GEN(i915) >= 6)
		gen6_rps_idle(i915);

	/* Everything switched off, flush any residual interrupt just in case */
	intel_synchronize_irq(i915);

	GEM_BUG_ON(!wakeref);
	intel_display_power_put(i915, POWER_DOMAIN_GT_IRQ, wakeref);

	return 0;
}

static const struct intel_wakeref_ops wf_ops = {
	.get = __gt_unpark,
	.put = __gt_park,
	.flags = INTEL_WAKEREF_PUT_ASYNC,
};

void intel_gt_pm_init_early(struct intel_gt *gt)
{
	intel_wakeref_init(&gt->wakeref, &gt->i915->runtime_pm, &wf_ops);

	BLOCKING_INIT_NOTIFIER_HEAD(&gt->pm_notifications);
}

static bool reset_engines(struct intel_gt *gt)
{
	if (INTEL_INFO(gt->i915)->gpu_reset_clobbers_display)
		return false;

	return __intel_gt_reset(gt, ALL_ENGINES) == 0;
}

/**
 * intel_gt_sanitize: called after the GPU has lost power
 * @gt: the i915 GT container
 * @force: ignore a failed reset and sanitize engine state anyway
 *
 * Anytime we reset the GPU, either with an explicit GPU reset or through a
 * PCI power cycle, the GPU loses state and we must reset our state tracking
 * to match. Note that calling intel_gt_sanitize() if the GPU has not
 * been reset results in much confusion!
 */
void intel_gt_sanitize(struct intel_gt *gt, bool force)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	GEM_TRACE("\n");

	intel_uc_sanitize(&gt->uc);

	if (!reset_engines(gt) && !force)
		return;

	for_each_engine(engine, gt->i915, id)
		__intel_engine_reset(engine, false);
}

static bool is_mock_device(const struct intel_gt *gt)
{
	return I915_SELFTEST_ONLY(gt->awake == -1);
}

void intel_gt_pm_enable(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	/* Powersaving is controlled by the host when inside a VM */
	if (intel_vgpu_active(gt->i915))
		return;

	if (is_mock_device(gt))
		return;

	intel_gt_pm_get(gt);

	for_each_engine(engine, gt->i915, id) {
		intel_engine_pm_get(engine);
		engine->serial++; /* force kernel context reload */
		intel_engine_pm_put(engine);
	}

	intel_gt_pm_put(gt);
}

void intel_gt_pm_disable(struct intel_gt *gt)
{
	if (is_mock_device(gt))
		return;

	intel_sanitize_gt_powersave(gt->i915);
}

int intel_gt_resume(struct intel_gt *gt)
{
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	int err = 0;

	/*
	 * After resume, we may need to poke into the pinned kernel
	 * contexts to paper over any damage caused by the sudden suspend.
	 * Only the kernel contexts should remain pinned over suspend,
	 * allowing us to fixup the user contexts on their first pin.
	 */
	intel_gt_pm_get(gt);
	for_each_engine(engine, gt->i915, id) {
		struct intel_context *ce;

		intel_engine_pm_get(engine);

		ce = engine->kernel_context;
		if (ce) {
			GEM_BUG_ON(!intel_context_is_pinned(ce));
			mutex_acquire(&ce->pin_mutex.dep_map, 0, 0, _THIS_IP_);
			ce->ops->reset(ce);
			mutex_release(&ce->pin_mutex.dep_map, 0, _THIS_IP_);
		}

		engine->serial++; /* kernel context lost */
		err = engine->resume(engine);

		intel_engine_pm_put(engine);
		if (err) {
			dev_err(gt->i915->drm.dev,
				"Failed to restart %s (%d)\n",
				engine->name, err);
			break;
		}
	}
	intel_gt_pm_put(gt);

	return err;
}

void intel_gt_runtime_suspend(struct intel_gt *gt)
{
	intel_uc_runtime_suspend(&gt->uc);
}

int intel_gt_runtime_resume(struct intel_gt *gt)
{
	intel_gt_init_swizzling(gt);

	return intel_uc_runtime_resume(&gt->uc);
}

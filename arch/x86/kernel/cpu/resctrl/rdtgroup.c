// SPDX-License-Identifier: GPL-2.0-only
/*
 * User interface for Resource Allocation in Resource Director Technology(RDT)
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Author: Fenghua Yu <fenghua.yu@intel.com>
 *
 * More information about RDT be found in the Intel (R) x86 Architecture
 * Software Developer Manual.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/fs_parser.h>
#include <linux/sysfs.h>
#include <linux/kernfs.h>
#include <linux/resctrl.h>
#include <linux/seq_buf.h>
#include <linux/seq_file.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/user_namespace.h>

#include <uapi/linux/magic.h>

#include <asm/msr.h>
#include "internal.h"

DEFINE_STATIC_KEY_FALSE(rdt_enable_key);

DEFINE_STATIC_KEY_FALSE(rdt_mon_enable_key);

DEFINE_STATIC_KEY_FALSE(rdt_alloc_enable_key);

/*
 * This is safe against resctrl_arch_sched_in() called from __switch_to()
 * because __switch_to() is executed with interrupts disabled. A local call
 * from update_closid_rmid() is protected against __switch_to() because
 * preemption is disabled.
 */
void resctrl_arch_sync_cpu_closid_rmid(void *info)
{
	struct resctrl_cpu_defaults *r = info;

	if (r) {
		this_cpu_write(pqr_state.default_closid, r->closid);
		this_cpu_write(pqr_state.default_rmid, r->rmid);
	}

	/*
	 * We cannot unconditionally write the MSR because the current
	 * executing task might have its own closid selected. Just reuse
	 * the context switch code.
	 */
	resctrl_arch_sched_in(current);
}

#define INVALID_CONFIG_INDEX   UINT_MAX

/**
 * mon_event_config_index_get - get the hardware index for the
 *                              configurable event
 * @evtid: event id.
 *
 * Return: 0 for evtid == QOS_L3_MBM_TOTAL_EVENT_ID
 *         1 for evtid == QOS_L3_MBM_LOCAL_EVENT_ID
 *         INVALID_CONFIG_INDEX for invalid evtid
 */
static inline unsigned int mon_event_config_index_get(u32 evtid)
{
	switch (evtid) {
	case QOS_L3_MBM_TOTAL_EVENT_ID:
		return 0;
	case QOS_L3_MBM_LOCAL_EVENT_ID:
		return 1;
	default:
		/* Should never reach here */
		return INVALID_CONFIG_INDEX;
	}
}

void resctrl_arch_mon_event_config_read(void *_config_info)
{
	struct resctrl_mon_config_info *config_info = _config_info;
	unsigned int index;
	u64 msrval;

	index = mon_event_config_index_get(config_info->evtid);
	if (index == INVALID_CONFIG_INDEX) {
		pr_warn_once("Invalid event id %d\n", config_info->evtid);
		return;
	}
	rdmsrq(MSR_IA32_EVT_CFG_BASE + index, msrval);

	/* Report only the valid event configuration bits */
	config_info->mon_config = msrval & MAX_EVT_CONFIG_BITS;
}

void resctrl_arch_mon_event_config_write(void *_config_info)
{
	struct resctrl_mon_config_info *config_info = _config_info;
	unsigned int index;

	index = mon_event_config_index_get(config_info->evtid);
	if (index == INVALID_CONFIG_INDEX) {
		pr_warn_once("Invalid event id %d\n", config_info->evtid);
		return;
	}
	wrmsrq(MSR_IA32_EVT_CFG_BASE + index, config_info->mon_config);
}

static void l3_qos_cfg_update(void *arg)
{
	bool *enable = arg;

	wrmsrq(MSR_IA32_L3_QOS_CFG, *enable ? L3_QOS_CDP_ENABLE : 0ULL);
}

static void l2_qos_cfg_update(void *arg)
{
	bool *enable = arg;

	wrmsrq(MSR_IA32_L2_QOS_CFG, *enable ? L2_QOS_CDP_ENABLE : 0ULL);
}

static int set_cache_qos_cfg(struct rdt_hw_resource *hw_res, struct resctrl_ctrl *ctrl,
			     bool enable)
{
	struct rdt_resource *r = &hw_res->r_resctrl;
	void (*update)(void *arg);
	struct rdt_ctrl_domain *d;
	cpumask_var_t cpu_mask;
	int cpu;

	/* Walking ctrl->domains, ensure it can't race with cpuhp */
	lockdep_assert_cpus_held();

	if (r->rid == RDT_RESOURCE_L3)
		update = l3_qos_cfg_update;
	else if (r->rid == RDT_RESOURCE_L2)
		update = l2_qos_cfg_update;
	else
		return -EINVAL;

	if (!zalloc_cpumask_var(&cpu_mask, GFP_KERNEL))
		return -ENOMEM;

	list_for_each_entry_rcu(d, &ctrl->domains, hdr.list, lockdep_is_cpus_held()) {
		if (hw_res->has_per_cpu_cache_cfg)
			/* Pick all the CPUs in the domain instance */
			for_each_cpu(cpu, &d->hdr.cpu_mask)
				cpumask_set_cpu(cpu, cpu_mask);
		else
			/* Pick one CPU from each domain instance to update MSR */
			cpumask_set_cpu(cpumask_any(&d->hdr.cpu_mask), cpu_mask);
	}

	/* Update QOS_CFG MSR on all the CPUs in cpu_mask */
	on_each_cpu_mask(cpu_mask, update, &enable, 1);

	free_cpumask_var(cpu_mask);

	return 0;
}

/* Restore the qos cfg state when a domain comes online */
void rdt_domain_reconfigure_cdp(struct rdt_resource *r)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);

	if (!r->cdp_capable)
		return;

	if (r->rid == RDT_RESOURCE_L2)
		l2_qos_cfg_update(&hw_res->cdp_enabled);

	if (r->rid == RDT_RESOURCE_L3)
		l3_qos_cfg_update(&hw_res->cdp_enabled);
}

static int cdp_enable(struct rdt_hw_resource *hw_res, struct resctrl_ctrl *ctrl)
{
	struct rdt_resource *r = &hw_res->r_resctrl;
	int ret;

	if (!r->alloc_capable)
		return -EINVAL;

	ret = set_cache_qos_cfg(hw_res, ctrl, true);
	if (!ret)
		hw_res->cdp_enabled = true;

	return ret;
}

static void cdp_disable(struct rdt_hw_resource *hw_res, struct resctrl_ctrl *ctrl)
{
	if (hw_res->cdp_enabled) {
		set_cache_qos_cfg(hw_res, ctrl, false);
		hw_res->cdp_enabled = false;
	}
}

int resctrl_arch_set_cdp_enabled(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
				 bool enable)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);

	if (!r->cdp_capable)
		return -EINVAL;

	if (enable)
		return cdp_enable(hw_res, ctrl);

	cdp_disable(hw_res, ctrl);

	return 0;
}

bool resctrl_arch_get_cdp_enabled(struct rdt_resource *r)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);

	return hw_res->cdp_enabled;
}

static void reset_single_ctrl(struct msr_param *m)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(m->res);
	struct resctrl_ctrl *ctrl = m->ctrl;
	struct rdt_hw_ctrl_domain *hw_dom;
	struct resctrl_hw_ctrl *hw_ctrl;
	struct rdt_ctrl_domain *d;
	int i;

	hw_ctrl = resctrl_to_arch_ctrl(ctrl);
	list_for_each_entry_rcu(d, &ctrl->domains, hdr.list, lockdep_is_cpus_held()) {
		hw_dom = resctrl_to_arch_ctrl_dom(d);

		for (i = 0; i < hw_res->num_closid; i++)
			hw_dom->ctrl_val[i] = resctrl_get_default_ctrlval(ctrl);
		m->dom = d;
		if (hw_ctrl->msr_update)
			smp_call_function_any(&d->hdr.cpu_mask, rdt_ctrl_update, m, 1);
	}
}

void resctrl_arch_reset_all_ctrls(struct rdt_resource *r)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);
	struct resctrl_ctrl *ctrl, *em_ctrl;
	struct msr_param msr_param;

	/* Walking ctrl->domains, ensure it can't race with cpuhp */
	lockdep_assert_cpus_held();

	msr_param.res = r;
	msr_param.low = 0;
	msr_param.high = hw_res->num_closid;

	/*
	 * Disable resource control for this resource by setting all
	 * control values in all control domains to their reset value.
	 */
	for_each_resource_ctrl(ctrl, r) {
		msr_param.ctrl = ctrl;
		reset_single_ctrl(&msr_param);
		if (!list_empty(&ctrl->emulated_by)) {
			list_for_each_entry(em_ctrl, &ctrl->emulated_by, entry) {
				msr_param.ctrl = em_ctrl;
				reset_single_ctrl(&msr_param);
			}
		}
	}

	return;
}

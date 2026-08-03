// SPDX-License-Identifier: GPL-2.0-only
/*
 * Resource Director Technology(RDT)
 * - Cache Allocation code.
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Authors:
 *    Fenghua Yu <fenghua.yu@intel.com>
 *    Tony Luck <tony.luck@intel.com>
 *
 * More information about RDT be found in the Intel (R) x86 Architecture
 * Software Developer Manual June 2016, volume 3, section 17.17.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/math.h>

#include "internal.h"

u32 resctrl_arch_preconvert_bw(const struct rdt_resource *r,
			       struct resctrl_ctrl *ctrl, u32 val)
{
	return roundup(val, (unsigned long)ctrl->scalar.gran);
}

int resctrl_arch_update_one(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
			    struct rdt_ctrl_domain *d, u32 closid,
			    enum resctrl_conf_type t, u32 cfg_val)
{
	struct rdt_hw_ctrl_domain *hw_dom = resctrl_to_arch_ctrl_dom(d);
	struct resctrl_hw_ctrl *hw_ctrl = resctrl_to_arch_ctrl(ctrl);
	u32 idx = resctrl_get_config_index(closid, t);
	struct msr_param msr_param;

	if (!cpumask_test_cpu(smp_processor_id(), &d->hdr.cpu_mask))
		return -EINVAL;

	hw_dom->ctrl_val[idx] = cfg_val;

	msr_param.res = r;
	msr_param.ctrl = ctrl;
	msr_param.dom = d;
	msr_param.low = idx;
	msr_param.high = idx + 1;
	hw_ctrl->msr_update(&msr_param);

	return 0;
}

/*
 * Backing controls have the same number of domains and domain IDs map to
 * same CPUs.
 */
static void program_backing_controls(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
				     struct rdt_ctrl_domain *d, u32 closid,
				     struct msr_param *m)
{
	struct rdt_hw_ctrl_domain *em_hw_dom;
	struct resctrl_staged_config *cfg;
	struct resctrl_hw_ctrl *em_hw_ctrl;
	struct resctrl_ctrl *em_ctrl;
	struct rdt_ctrl_domain *em_d;
	struct rdt_domain_hdr *hdr;
	enum resctrl_conf_type t;
	u32 idx;

	list_for_each_entry(em_ctrl, &ctrl->emulated_by, entry) {
		m->ctrl = em_ctrl;
		hdr = resctrl_find_domain(&em_ctrl->domains, d->hdr.id, NULL);
		em_d = container_of(hdr, struct rdt_ctrl_domain, hdr);
		m->dom = em_d;
		em_hw_dom = resctrl_to_arch_ctrl_dom(em_d);
		em_hw_ctrl = resctrl_to_arch_ctrl(em_ctrl);
		for (t = 0; t < CDP_NUM_TYPES; t++) {
			cfg = &d->staged_config[t];
			if (!cfg->have_new_ctrl)
				continue;

			idx = resctrl_get_config_index(closid, t);
			em_hw_dom->ctrl_val[idx] = em_hw_ctrl->emulate_val(cfg->new_ctrl);
		}

		smp_call_function_any(&em_d->hdr.cpu_mask, rdt_ctrl_update, m, 1);
	}
}

static void _resctrl_arch_update_domains(struct rdt_resource *r,
					 struct resctrl_ctrl *ctrl, u32 closid)
{
	struct resctrl_staged_config *cfg;
	struct rdt_hw_ctrl_domain *hw_dom;
	struct resctrl_hw_ctrl *hw_ctrl;
	struct msr_param msr_param;
	struct rdt_ctrl_domain *d;
	enum resctrl_conf_type t;
	u32 idx;

	/* Walking ctrl->domains, ensure it can't race with cpuhp */
	lockdep_assert_cpus_held();

	/*
	 * If control is emulated and mode is legacy, do not actually
	 * update control, just stage the values of the emulating control
	 */
	msr_param.ctrl = ctrl;
	hw_ctrl = resctrl_to_arch_ctrl(ctrl);
	list_for_each_entry_rcu(d, &ctrl->domains, hdr.list, lockdep_is_cpus_held()) {
		hw_dom = resctrl_to_arch_ctrl_dom(d);
		msr_param.res = NULL;
		for (t = 0; t < CDP_NUM_TYPES; t++) {
			cfg = &hw_dom->d_resctrl.staged_config[t];
			if (!cfg->have_new_ctrl)
				continue;

			idx = resctrl_get_config_index(closid, t);
			if (cfg->new_ctrl == hw_dom->ctrl_val[idx]) {
				cfg->have_new_ctrl = false;
				continue;
			}
			hw_dom->ctrl_val[idx] = cfg->new_ctrl;

			if (!msr_param.res) {
				msr_param.low = idx;
				msr_param.high = msr_param.low + 1;
				msr_param.res = r;
				msr_param.dom = d;
			} else {
				msr_param.low = min(msr_param.low, idx);
				msr_param.high = max(msr_param.high, idx + 1);
			}
		}

		if (!msr_param.res)
			continue;

		if (!hw_ctrl->msr_update)
			program_backing_controls(r, ctrl, d, closid, &msr_param);
		else
			smp_call_function_any(&d->hdr.cpu_mask, rdt_ctrl_update, &msr_param, 1);
	}
}

/*
 * Avoid duplicate control configurations by only checking enabled controls
 * for any staged configurations.
 */
int resctrl_arch_update_domains(struct rdt_resource *r, u32 closid)
{
	struct resctrl_ctrl *ctrl;

	for_each_enabled_ctrl(ctrl, r)
		_resctrl_arch_update_domains(r, ctrl, closid);

	return 0;
}

u32 resctrl_arch_get_config(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
			    struct rdt_ctrl_domain *d, u32 closid, enum
			    resctrl_conf_type type)
{
	struct rdt_hw_ctrl_domain *hw_dom = resctrl_to_arch_ctrl_dom(d);
	u32 idx = resctrl_get_config_index(closid, type);

	return hw_dom->ctrl_val[idx];
}

bool resctrl_arch_get_io_alloc_enabled(struct rdt_resource *r)
{
	return resctrl_to_arch_res(r)->sdciae_enabled;
}

static void resctrl_sdciae_set_one_amd(void *arg)
{
	bool *enable = arg;

	if (*enable)
		msr_set_bit(MSR_IA32_L3_QOS_EXT_CFG, SDCIAE_ENABLE_BIT);
	else
		msr_clear_bit(MSR_IA32_L3_QOS_EXT_CFG, SDCIAE_ENABLE_BIT);
}

static void _resctrl_sdciae_enable(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
				   bool enable)
{
	struct rdt_ctrl_domain *d;

	/* Walking ctrl->domains, ensure it can't race with cpuhp */
	lockdep_assert_cpus_held();

	/* Update MSR_IA32_L3_QOS_EXT_CFG MSR on all the CPUs in all domains */
	list_for_each_entry_rcu(d, &ctrl->domains, hdr.list, lockdep_is_cpus_held())
		on_each_cpu_mask(&d->hdr.cpu_mask, resctrl_sdciae_set_one_amd, &enable, 1);
}

int resctrl_arch_io_alloc_enable(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
				 bool enable)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);

	if (hw_res->r_resctrl.cache_io_alloc_capable &&
	    hw_res->sdciae_enabled != enable) {
		_resctrl_sdciae_enable(r, ctrl, enable);
		hw_res->sdciae_enabled = enable;
	}

	return 0;
}


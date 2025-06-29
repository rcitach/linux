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
 *    Vikas Shivappa <vikas.shivappa@intel.com>
 *
 * More information about RDT be found in the Intel (R) x86 Architecture
 * Software Developer Manual June 2016, volume 3, section 17.17.
 */

#define pr_fmt(fmt)	"resctrl: " fmt

#include <linux/cpu.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/cpuhotplug.h>

#include <asm/cpu_device_id.h>
#include <asm/msr.h>
#include <asm/resctrl.h>
#include "internal.h"

/*
 * rdt_domain structures are kfree()d when their last CPU goes offline,
 * and allocated when the first CPU in a new domain comes online.
 * The rdt_resource's domain list is updated when this happens. Readers of
 * the domain list must either take cpus_read_lock(), or rely on an RCU
 * read-side critical section, to avoid observing concurrent modification.
 * All writers take this mutex:
 */
static DEFINE_MUTEX(domain_list_lock);

/*
 * The cached resctrl_pqr_state is strictly per CPU and can never be
 * updated from a remote CPU. Functions which modify the state
 * are called with interrupts disabled and no preemption, which
 * is sufficient for the protection.
 */
DEFINE_PER_CPU(struct resctrl_pqr_state, pqr_state);

/*
 * Global boolean for rdt_alloc which is true if any
 * resource allocation is enabled.
 */
bool rdt_alloc_capable;

static void mba_wrmsr_intel(struct msr_param *m);
static void cat_wrmsr(struct msr_param *m);
static void mba_wrmsr_amd(struct msr_param *m);

#define ctrl_domain_init(id) LIST_HEAD_INIT(rdt_resources_all[id].r_resctrl.ctrl_domains)
#define mon_domain_init(id) LIST_HEAD_INIT(rdt_resources_all[id].r_resctrl.mon_domains)

struct rdt_hw_resource rdt_resources_all[RDT_NUM_RESOURCES] = {
	[RDT_RESOURCE_L3] =
	{
		.r_resctrl = {
			.name			= "L3",
			.ctrl_scope		= RESCTRL_L3_CACHE,
			.mon_scope		= RESCTRL_L3_CACHE,
			.ctrl_domains		= ctrl_domain_init(RDT_RESOURCE_L3),
			.mon_domains		= mon_domain_init(RDT_RESOURCE_L3),
			.schema_fmt		= RESCTRL_SCHEMA_BITMAP,
		},
		.msr_base		= MSR_IA32_L3_CBM_BASE,
		.msr_update		= cat_wrmsr,
	},
	[RDT_RESOURCE_L2] =
	{
		.r_resctrl = {
			.name			= "L2",
			.ctrl_scope		= RESCTRL_L2_CACHE,
			.ctrl_domains		= ctrl_domain_init(RDT_RESOURCE_L2),
			.schema_fmt		= RESCTRL_SCHEMA_BITMAP,
		},
		.msr_base		= MSR_IA32_L2_CBM_BASE,
		.msr_update		= cat_wrmsr,
	},
	[RDT_RESOURCE_MBA] =
	{
		.r_resctrl = {
			.name			= "MB",
			.ctrl_scope		= RESCTRL_L3_CACHE,
			.ctrl_domains		= ctrl_domain_init(RDT_RESOURCE_MBA),
			.schema_fmt		= RESCTRL_SCHEMA_RANGE,
		},
	},
	[RDT_RESOURCE_SMBA] =
	{
		.r_resctrl = {
			.name			= "SMBA",
			.ctrl_scope		= RESCTRL_L3_CACHE,
			.ctrl_domains		= ctrl_domain_init(RDT_RESOURCE_SMBA),
			.schema_fmt		= RESCTRL_SCHEMA_RANGE,
		},
	},
};

u32 resctrl_arch_system_num_rmid_idx(void)
{
	struct rdt_resource *r = &rdt_resources_all[RDT_RESOURCE_L3].r_resctrl;

	/* RMID are independent numbers for x86. num_rmid_idx == num_rmid */
	return r->num_rmid;
}

struct rdt_resource *resctrl_arch_get_resource(enum resctrl_res_level l)
{
	if (l >= RDT_NUM_RESOURCES)
		return NULL;

	return &rdt_resources_all[l].r_resctrl;
}

/*
 * cache_alloc_hsw_probe() - Have to probe for Intel haswell server CPUs
 * as they do not have CPUID enumeration support for Cache allocation.
 * The check for Vendor/Family/Model is not enough to guarantee that
 * the MSRs won't #GP fault because only the following SKUs support
 * CAT:
 *	Intel(R) Xeon(R)  CPU E5-2658  v3  @  2.20GHz
 *	Intel(R) Xeon(R)  CPU E5-2648L v3  @  1.80GHz
 *	Intel(R) Xeon(R)  CPU E5-2628L v3  @  2.00GHz
 *	Intel(R) Xeon(R)  CPU E5-2618L v3  @  2.30GHz
 *	Intel(R) Xeon(R)  CPU E5-2608L v3  @  2.00GHz
 *	Intel(R) Xeon(R)  CPU E5-2658A v3  @  2.20GHz
 *
 * Probe by trying to write the first of the L3 cache mask registers
 * and checking that the bits stick. Max CLOSids is always 4 and max cbm length
 * is always 20 on hsw server parts. The minimum cache bitmask length
 * allowed for HSW server is always 2 bits. Hardcode all of them.
 */
static inline void cache_alloc_hsw_probe(void)
{
	struct rdt_hw_resource *hw_res = &rdt_resources_all[RDT_RESOURCE_L3];
	struct rdt_resource *r  = &hw_res->r_resctrl;
	u64 max_cbm = BIT_ULL_MASK(20) - 1, l3_cbm_0;

	if (wrmsrq_safe(MSR_IA32_L3_CBM_BASE, max_cbm))
		return;

	rdmsrq(MSR_IA32_L3_CBM_BASE, l3_cbm_0);

	/* If all the bits were set in MSR, return success */
	if (l3_cbm_0 != max_cbm)
		return;

	hw_res->num_closid = 4;
	r->cache.cbm_len = 20;
	r->cache.shareable_bits = 0xc0000;
	r->cache.min_cbm_bits = 2;
	r->cache.arch_has_sparse_bitmasks = false;
	r->alloc_capable = true;

	rdt_alloc_capable = true;
}

/*
 * rdt_get_mb_table() - get a mapping of bandwidth(b/w) percentage values
 * exposed to user interface and the h/w understandable delay values.
 *
 * The non-linear delay values have the granularity of power of two
 * and also the h/w does not guarantee a curve for configured delay
 * values vs. actual b/w enforced.
 * Hence we need a mapping that is pre calibrated so the user can
 * express the memory b/w as a percentage value.
 */
static inline bool rdt_get_mb_table(struct rdt_resource *r)
{
	/*
	 * There are no Intel SKUs as of now to support non-linear delay.
	 */
	pr_info("MBA b/w map not implemented for cpu:%d, model:%d",
		boot_cpu_data.x86, boot_cpu_data.x86_model);

	return false;
}

static __init bool __get_mem_config_intel(struct rdt_resource *r)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);
	union cpuid_0x10_3_eax eax;
	union cpuid_0x10_x_edx edx;
	u32 ebx, ecx, max_delay;

	cpuid_count(0x00000010, 3, &eax.full, &ebx, &ecx, &edx.full);
	hw_res->num_closid = edx.split.cos_max + 1;
	max_delay = eax.split.max_delay + 1;
	r->membw.max_bw = MAX_MBA_BW;
	r->membw.arch_needs_linear = true;
	if (ecx & MBA_IS_LINEAR) {
		r->membw.delay_linear = true;
		r->membw.min_bw = MAX_MBA_BW - max_delay;
		r->membw.bw_gran = MAX_MBA_BW - max_delay;
	} else {
		if (!rdt_get_mb_table(r))
			return false;
		r->membw.arch_needs_linear = false;
	}

	if (boot_cpu_has(X86_FEATURE_PER_THREAD_MBA))
		r->membw.throttle_mode = THREAD_THROTTLE_PER_THREAD;
	else
		r->membw.throttle_mode = THREAD_THROTTLE_MAX;

	r->alloc_capable = true;

	return true;
}

static __init bool __rdt_get_mem_config_amd(struct rdt_resource *r)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);
	u32 eax, ebx, ecx, edx, subleaf;

	/*
	 * Query CPUID_Fn80000020_EDX_x01 for MBA and
	 * CPUID_Fn80000020_EDX_x02 for SMBA
	 */
	subleaf = (r->rid == RDT_RESOURCE_SMBA) ? 2 :  1;

	cpuid_count(0x80000020, subleaf, &eax, &ebx, &ecx, &edx);
	hw_res->num_closid = edx + 1;
	r->membw.max_bw = 1 << eax;

	/* AMD does not use delay */
	r->membw.delay_linear = false;
	r->membw.arch_needs_linear = false;

	/*
	 * AMD does not use memory delay throttle model to control
	 * the allocation like Intel does.
	 */
	r->membw.throttle_mode = THREAD_THROTTLE_UNDEFINED;
	r->membw.min_bw = 0;
	r->membw.bw_gran = 1;

	r->alloc_capable = true;

	return true;
}

static void rdt_get_cache_alloc_cfg(int idx, struct rdt_resource *r)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);
	union cpuid_0x10_1_eax eax;
	union cpuid_0x10_x_ecx ecx;
	union cpuid_0x10_x_edx edx;
	u32 ebx, default_ctrl;

	cpuid_count(0x00000010, idx, &eax.full, &ebx, &ecx.full, &edx.full);
	hw_res->num_closid = edx.split.cos_max + 1;
	r->cache.cbm_len = eax.split.cbm_len + 1;
	default_ctrl = BIT_MASK(eax.split.cbm_len + 1) - 1;
	r->cache.shareable_bits = ebx & default_ctrl;
	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL)
		r->cache.arch_has_sparse_bitmasks = ecx.split.noncont;
	r->alloc_capable = true;
}

static void rdt_get_cdp_config(int level)
{
	/*
	 * By default, CDP is disabled. CDP can be enabled by mount parameter
	 * "cdp" during resctrl file system mount time.
	 */
	rdt_resources_all[level].cdp_enabled = false;
	rdt_resources_all[level].r_resctrl.cdp_capable = true;
}

static void rdt_get_cdp_l3_config(void)
{
	rdt_get_cdp_config(RDT_RESOURCE_L3);
}

static void rdt_get_cdp_l2_config(void)
{
	rdt_get_cdp_config(RDT_RESOURCE_L2);
}

static void mba_wrmsr_amd(struct msr_param *m)
{
	struct rdt_hw_ctrl_domain *hw_dom = resctrl_to_arch_ctrl_dom(m->dom);
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(m->res);
	unsigned int i;

	for (i = m->low; i < m->high; i++)
		wrmsrq(hw_res->msr_base + i, hw_dom->ctrl_val[i]);
}

/*
 * Map the memory b/w percentage value to delay values
 * that can be written to QOS_MSRs.
 * There are currently no SKUs which support non linear delay values.
 */
static u32 delay_bw_map(unsigned long bw, struct rdt_resource *r)
{
	if (r->membw.delay_linear)
		return MAX_MBA_BW - bw;

	pr_warn_once("Non Linear delay-bw map not supported but queried\n");
	return MAX_MBA_BW;
}

static void mba_wrmsr_intel(struct msr_param *m)
{
	struct rdt_hw_ctrl_domain *hw_dom = resctrl_to_arch_ctrl_dom(m->dom);
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(m->res);
	unsigned int i;

	/*  Write the delay values for mba. */
	for (i = m->low; i < m->high; i++)
		wrmsrq(hw_res->msr_base + i, delay_bw_map(hw_dom->ctrl_val[i], m->res));
}

static void cat_wrmsr(struct msr_param *m)
{
	struct rdt_hw_ctrl_domain *hw_dom = resctrl_to_arch_ctrl_dom(m->dom);
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(m->res);
	unsigned int i;

	for (i = m->low; i < m->high; i++)
		wrmsrq(hw_res->msr_base + i, hw_dom->ctrl_val[i]);
}

u32 resctrl_arch_get_num_closid(struct rdt_resource *r)
{
	return resctrl_to_arch_res(r)->num_closid;
}

void rdt_ctrl_update(void *arg)
{
	struct rdt_hw_resource *hw_res;
	struct msr_param *m = arg;

	hw_res = resctrl_to_arch_res(m->res);
	hw_res->msr_update(m);
}

static void setup_default_ctrlval(struct rdt_resource *r, u32 *dc)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);
	int i;

	/*
	 * Initialize the Control MSRs to having no control.
	 * For Cache Allocation: Set all bits in cbm
	 * For Memory Allocation: Set b/w requested to 100%
	 */
	for (i = 0; i < hw_res->num_closid; i++, dc++)
		*dc = resctrl_get_default_ctrl(r);
}

static void ctrl_domain_free(struct rdt_hw_ctrl_domain *hw_dom)
{
	kfree(hw_dom->ctrl_val);
	kfree(hw_dom);
}

static void mon_domain_free(struct rdt_hw_mon_domain *hw_dom)
{
	kfree(hw_dom->arch_mbm_total);
	kfree(hw_dom->arch_mbm_local);
	kfree(hw_dom);
}

static int domain_setup_ctrlval(struct rdt_resource *r, struct rdt_ctrl_domain *d)
{
	struct rdt_hw_ctrl_domain *hw_dom = resctrl_to_arch_ctrl_dom(d);
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);
	struct msr_param m;
	u32 *dc;

	dc = kmalloc_array(hw_res->num_closid, sizeof(*hw_dom->ctrl_val),
			   GFP_KERNEL);
	if (!dc)
		return -ENOMEM;

	hw_dom->ctrl_val = dc;
	setup_default_ctrlval(r, dc);

	m.res = r;
	m.dom = d;
	m.low = 0;
	m.high = hw_res->num_closid;
	hw_res->msr_update(&m);
	return 0;
}

/**
 * arch_domain_mbm_alloc() - Allocate arch private storage for the MBM counters
 * @num_rmid:	The size of the MBM counter array
 * @hw_dom:	The domain that owns the allocated arrays
 */
static int arch_domain_mbm_alloc(u32 num_rmid, struct rdt_hw_mon_domain *hw_dom)
{
	size_t tsize;

	if (resctrl_arch_is_mbm_total_enabled()) {
		tsize = sizeof(*hw_dom->arch_mbm_total);
		hw_dom->arch_mbm_total = kcalloc(num_rmid, tsize, GFP_KERNEL);
		if (!hw_dom->arch_mbm_total)
			return -ENOMEM;
	}
	if (resctrl_arch_is_mbm_local_enabled()) {
		tsize = sizeof(*hw_dom->arch_mbm_local);
		hw_dom->arch_mbm_local = kcalloc(num_rmid, tsize, GFP_KERNEL);
		if (!hw_dom->arch_mbm_local) {
			kfree(hw_dom->arch_mbm_total);
			hw_dom->arch_mbm_total = NULL;
			return -ENOMEM;
		}
	}

	return 0;
}

static int get_domain_id_from_scope(int cpu, enum resctrl_scope scope)
{
	switch (scope) {
	case RESCTRL_L2_CACHE:
	case RESCTRL_L3_CACHE:
		return get_cpu_cacheinfo_id(cpu, scope);
	case RESCTRL_L3_NODE:
		return cpu_to_node(cpu);
	default:
		break;
	}

	return -EINVAL;
}

static void domain_add_cpu_ctrl(int cpu, struct rdt_resource *r)
{
	int id = get_domain_id_from_scope(cpu, r->ctrl_scope);
	struct rdt_hw_ctrl_domain *hw_dom;
	struct list_head *add_pos = NULL;
	struct rdt_domain_hdr *hdr;
	struct rdt_ctrl_domain *d;
	int err;

	lockdep_assert_held(&domain_list_lock);

	if (id < 0) {
		pr_warn_once("Can't find control domain id for CPU:%d scope:%d for resource %s\n",
			     cpu, r->ctrl_scope, r->name);
		return;
	}

	hdr = resctrl_find_domain(&r->ctrl_domains, id, &add_pos);
	if (hdr) {
		if (WARN_ON_ONCE(hdr->type != RESCTRL_CTRL_DOMAIN))
			return;
		d = container_of(hdr, struct rdt_ctrl_domain, hdr);

		cpumask_set_cpu(cpu, &d->hdr.cpu_mask);
		if (r->cache.arch_has_per_cpu_cfg)
			rdt_domain_reconfigure_cdp(r);
		return;
	}

	hw_dom = kzalloc_node(sizeof(*hw_dom), GFP_KERNEL, cpu_to_node(cpu));
	if (!hw_dom)
		return;

	d = &hw_dom->d_resctrl;
	d->hdr.id = id;
	d->hdr.type = RESCTRL_CTRL_DOMAIN;
	cpumask_set_cpu(cpu, &d->hdr.cpu_mask);

	rdt_domain_reconfigure_cdp(r);

	if (domain_setup_ctrlval(r, d)) {
		ctrl_domain_free(hw_dom);
		return;
	}

	list_add_tail_rcu(&d->hdr.list, add_pos);

	err = resctrl_online_ctrl_domain(r, d);
	if (err) {
		list_del_rcu(&d->hdr.list);
		synchronize_rcu();
		ctrl_domain_free(hw_dom);
	}
}

static void domain_add_cpu_mon(int cpu, struct rdt_resource *r)
{
	int id = get_domain_id_from_scope(cpu, r->mon_scope);
	struct list_head *add_pos = NULL;
	struct rdt_hw_mon_domain *hw_dom;
	struct rdt_domain_hdr *hdr;
	struct rdt_mon_domain *d;
	struct cacheinfo *ci;
	int err;

	lockdep_assert_held(&domain_list_lock);

	if (id < 0) {
		pr_warn_once("Can't find monitor domain id for CPU:%d scope:%d for resource %s\n",
			     cpu, r->mon_scope, r->name);
		return;
	}

	hdr = resctrl_find_domain(&r->mon_domains, id, &add_pos);
	if (hdr) {
		if (WARN_ON_ONCE(hdr->type != RESCTRL_MON_DOMAIN))
			return;
		d = container_of(hdr, struct rdt_mon_domain, hdr);

		cpumask_set_cpu(cpu, &d->hdr.cpu_mask);
		return;
	}

	hw_dom = kzalloc_node(sizeof(*hw_dom), GFP_KERNEL, cpu_to_node(cpu));
	if (!hw_dom)
		return;

	d = &hw_dom->d_resctrl;
	d->hdr.id = id;
	d->hdr.type = RESCTRL_MON_DOMAIN;
	ci = get_cpu_cacheinfo_level(cpu, RESCTRL_L3_CACHE);
	if (!ci) {
		pr_warn_once("Can't find L3 cache for CPU:%d resource %s\n", cpu, r->name);
		mon_domain_free(hw_dom);
		return;
	}
	d->ci_id = ci->id;
	cpumask_set_cpu(cpu, &d->hdr.cpu_mask);

	arch_mon_domain_online(r, d);

	if (arch_domain_mbm_alloc(r->num_rmid, hw_dom)) {
		mon_domain_free(hw_dom);
		return;
	}

	list_add_tail_rcu(&d->hdr.list, add_pos);

	err = resctrl_online_mon_domain(r, d);
	if (err) {
		list_del_rcu(&d->hdr.list);
		synchronize_rcu();
		mon_domain_free(hw_dom);
	}
}

static void domain_add_cpu(int cpu, struct rdt_resource *r)
{
	if (r->alloc_capable)
		domain_add_cpu_ctrl(cpu, r);
	if (r->mon_capable)
		domain_add_cpu_mon(cpu, r);
}

static void domain_remove_cpu_ctrl(int cpu, struct rdt_resource *r)
{
	int id = get_domain_id_from_scope(cpu, r->ctrl_scope);
	struct rdt_hw_ctrl_domain *hw_dom;
	struct rdt_domain_hdr *hdr;
	struct rdt_ctrl_domain *d;

	lockdep_assert_held(&domain_list_lock);

	if (id < 0) {
		pr_warn_once("Can't find control domain id for CPU:%d scope:%d for resource %s\n",
			     cpu, r->ctrl_scope, r->name);
		return;
	}

	hdr = resctrl_find_domain(&r->ctrl_domains, id, NULL);
	if (!hdr) {
		pr_warn("Can't find control domain for id=%d for CPU %d for resource %s\n",
			id, cpu, r->name);
		return;
	}

	if (WARN_ON_ONCE(hdr->type != RESCTRL_CTRL_DOMAIN))
		return;

	d = container_of(hdr, struct rdt_ctrl_domain, hdr);
	hw_dom = resctrl_to_arch_ctrl_dom(d);

	cpumask_clear_cpu(cpu, &d->hdr.cpu_mask);
	if (cpumask_empty(&d->hdr.cpu_mask)) {
		resctrl_offline_ctrl_domain(r, d);
		list_del_rcu(&d->hdr.list);
		synchronize_rcu();

		/*
		 * rdt_ctrl_domain "d" is going to be freed below, so clear
		 * its pointer from pseudo_lock_region struct.
		 */
		if (d->plr)
			d->plr->d = NULL;
		ctrl_domain_free(hw_dom);

		return;
	}
}

static void domain_remove_cpu_mon(int cpu, struct rdt_resource *r)
{
	int id = get_domain_id_from_scope(cpu, r->mon_scope);
	struct rdt_hw_mon_domain *hw_dom;
	struct rdt_domain_hdr *hdr;
	struct rdt_mon_domain *d;

	lockdep_assert_held(&domain_list_lock);

	if (id < 0) {
		pr_warn_once("Can't find monitor domain id for CPU:%d scope:%d for resource %s\n",
			     cpu, r->mon_scope, r->name);
		return;
	}

	hdr = resctrl_find_domain(&r->mon_domains, id, NULL);
	if (!hdr) {
		pr_warn("Can't find monitor domain for id=%d for CPU %d for resource %s\n",
			id, cpu, r->name);
		return;
	}

	if (WARN_ON_ONCE(hdr->type != RESCTRL_MON_DOMAIN))
		return;

	d = container_of(hdr, struct rdt_mon_domain, hdr);
	hw_dom = resctrl_to_arch_mon_dom(d);

	cpumask_clear_cpu(cpu, &d->hdr.cpu_mask);
	if (cpumask_empty(&d->hdr.cpu_mask)) {
		resctrl_offline_mon_domain(r, d);
		list_del_rcu(&d->hdr.list);
		synchronize_rcu();
		mon_domain_free(hw_dom);

		return;
	}
}

static void domain_remove_cpu(int cpu, struct rdt_resource *r)
{
	if (r->alloc_capable)
		domain_remove_cpu_ctrl(cpu, r);
	if (r->mon_capable)
		domain_remove_cpu_mon(cpu, r);
}

static void clear_closid_rmid(int cpu)
{
	struct resctrl_pqr_state *state = this_cpu_ptr(&pqr_state);

	state->default_closid = RESCTRL_RESERVED_CLOSID;
	state->default_rmid = RESCTRL_RESERVED_RMID;
	state->cur_closid = RESCTRL_RESERVED_CLOSID;
	state->cur_rmid = RESCTRL_RESERVED_RMID;
	wrmsr(MSR_IA32_PQR_ASSOC, RESCTRL_RESERVED_RMID,
	      RESCTRL_RESERVED_CLOSID);
}

static int resctrl_arch_online_cpu(unsigned int cpu)
{
	struct rdt_resource *r;

	mutex_lock(&domain_list_lock);
	for_each_capable_rdt_resource(r)
		domain_add_cpu(cpu, r);
	mutex_unlock(&domain_list_lock);

	clear_closid_rmid(cpu);
	resctrl_online_cpu(cpu);

	return 0;
}

static int resctrl_arch_offline_cpu(unsigned int cpu)
{
	struct rdt_resource *r;

	resctrl_offline_cpu(cpu);

	mutex_lock(&domain_list_lock);
	for_each_capable_rdt_resource(r)
		domain_remove_cpu(cpu, r);
	mutex_unlock(&domain_list_lock);

	clear_closid_rmid(cpu);

	return 0;
}

enum {
	RDT_FLAG_CMT,
	RDT_FLAG_MBM_TOTAL,
	RDT_FLAG_MBM_LOCAL,
	RDT_FLAG_L3_CAT,
	RDT_FLAG_L3_CDP,
	RDT_FLAG_L2_CAT,
	RDT_FLAG_L2_CDP,
	RDT_FLAG_MBA,
	RDT_FLAG_SMBA,
	RDT_FLAG_BMEC,
};

#define RDT_OPT(idx, n, f)	\
[idx] = {			\
	.name = n,		\
	.flag = f		\
}

struct rdt_options {
	char	*name;
	int	flag;
	bool	force_off, force_on;
};

static struct rdt_options rdt_options[]  __ro_after_init = {
	RDT_OPT(RDT_FLAG_CMT,	    "cmt",	X86_FEATURE_CQM_OCCUP_LLC),
	RDT_OPT(RDT_FLAG_MBM_TOTAL, "mbmtotal", X86_FEATURE_CQM_MBM_TOTAL),
	RDT_OPT(RDT_FLAG_MBM_LOCAL, "mbmlocal", X86_FEATURE_CQM_MBM_LOCAL),
	RDT_OPT(RDT_FLAG_L3_CAT,    "l3cat",	X86_FEATURE_CAT_L3),
	RDT_OPT(RDT_FLAG_L3_CDP,    "l3cdp",	X86_FEATURE_CDP_L3),
	RDT_OPT(RDT_FLAG_L2_CAT,    "l2cat",	X86_FEATURE_CAT_L2),
	RDT_OPT(RDT_FLAG_L2_CDP,    "l2cdp",	X86_FEATURE_CDP_L2),
	RDT_OPT(RDT_FLAG_MBA,	    "mba",	X86_FEATURE_MBA),
	RDT_OPT(RDT_FLAG_SMBA,	    "smba",	X86_FEATURE_SMBA),
	RDT_OPT(RDT_FLAG_BMEC,	    "bmec",	X86_FEATURE_BMEC),
};
#define NUM_RDT_OPTIONS ARRAY_SIZE(rdt_options)

static int __init set_rdt_options(char *str)
{
	struct rdt_options *o;
	bool force_off;
	char *tok;

	if (*str == '=')
		str++;
	while ((tok = strsep(&str, ",")) != NULL) {
		force_off = *tok == '!';
		if (force_off)
			tok++;
		for (o = rdt_options; o < &rdt_options[NUM_RDT_OPTIONS]; o++) {
			if (strcmp(tok, o->name) == 0) {
				if (force_off)
					o->force_off = true;
				else
					o->force_on = true;
				break;
			}
		}
	}
	return 1;
}
__setup("rdt", set_rdt_options);

bool rdt_cpu_has(int flag)
{
	bool ret = boot_cpu_has(flag);
	struct rdt_options *o;

	if (!ret)
		return ret;

	for (o = rdt_options; o < &rdt_options[NUM_RDT_OPTIONS]; o++) {
		if (flag == o->flag) {
			if (o->force_off)
				ret = false;
			if (o->force_on)
				ret = true;
			break;
		}
	}
	return ret;
}

bool resctrl_arch_is_evt_configurable(enum resctrl_event_id evt)
{
	if (!rdt_cpu_has(X86_FEATURE_BMEC))
		return false;

	switch (evt) {
	case QOS_L3_MBM_TOTAL_EVENT_ID:
		return rdt_cpu_has(X86_FEATURE_CQM_MBM_TOTAL);
	case QOS_L3_MBM_LOCAL_EVENT_ID:
		return rdt_cpu_has(X86_FEATURE_CQM_MBM_LOCAL);
	default:
		return false;
	}
}

static __init bool get_mem_config(void)
{
	struct rdt_hw_resource *hw_res = &rdt_resources_all[RDT_RESOURCE_MBA];

	if (!rdt_cpu_has(X86_FEATURE_MBA))
		return false;

	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL)
		return __get_mem_config_intel(&hw_res->r_resctrl);
	else if (boot_cpu_data.x86_vendor == X86_VENDOR_AMD)
		return __rdt_get_mem_config_amd(&hw_res->r_resctrl);

	return false;
}

static __init bool get_slow_mem_config(void)
{
	struct rdt_hw_resource *hw_res = &rdt_resources_all[RDT_RESOURCE_SMBA];

	if (!rdt_cpu_has(X86_FEATURE_SMBA))
		return false;

	if (boot_cpu_data.x86_vendor == X86_VENDOR_AMD)
		return __rdt_get_mem_config_amd(&hw_res->r_resctrl);

	return false;
}

static __init bool get_rdt_alloc_resources(void)
{
	struct rdt_resource *r;
	bool ret = false;

	if (rdt_alloc_capable)
		return true;

	if (!boot_cpu_has(X86_FEATURE_RDT_A))
		return false;

	if (rdt_cpu_has(X86_FEATURE_CAT_L3)) {
		r = &rdt_resources_all[RDT_RESOURCE_L3].r_resctrl;
		rdt_get_cache_alloc_cfg(1, r);
		if (rdt_cpu_has(X86_FEATURE_CDP_L3))
			rdt_get_cdp_l3_config();
		ret = true;
	}
	if (rdt_cpu_has(X86_FEATURE_CAT_L2)) {
		/* CPUID 0x10.2 fields are same format at 0x10.1 */
		r = &rdt_resources_all[RDT_RESOURCE_L2].r_resctrl;
		rdt_get_cache_alloc_cfg(2, r);
		if (rdt_cpu_has(X86_FEATURE_CDP_L2))
			rdt_get_cdp_l2_config();
		ret = true;
	}

	if (get_mem_config())
		ret = true;

	if (get_slow_mem_config())
		ret = true;

	return ret;
}

static __init bool get_rdt_mon_resources(void)
{
	struct rdt_resource *r = &rdt_resources_all[RDT_RESOURCE_L3].r_resctrl;

	if (rdt_cpu_has(X86_FEATURE_CQM_OCCUP_LLC))
		rdt_mon_features |= (1 << QOS_L3_OCCUP_EVENT_ID);
	if (rdt_cpu_has(X86_FEATURE_CQM_MBM_TOTAL))
		rdt_mon_features |= (1 << QOS_L3_MBM_TOTAL_EVENT_ID);
	if (rdt_cpu_has(X86_FEATURE_CQM_MBM_LOCAL))
		rdt_mon_features |= (1 << QOS_L3_MBM_LOCAL_EVENT_ID);

	if (!rdt_mon_features)
		return false;

	return !rdt_get_mon_l3_config(r);
}

static __init void __check_quirks_intel(void)
{
	switch (boot_cpu_data.x86_vfm) {
	case INTEL_HASWELL_X:
		if (!rdt_options[RDT_FLAG_L3_CAT].force_off)
			cache_alloc_hsw_probe();
		break;
	case INTEL_SKYLAKE_X:
		if (boot_cpu_data.x86_stepping <= 4)
			set_rdt_options("!cmt,!mbmtotal,!mbmlocal,!l3cat");
		else
			set_rdt_options("!l3cat");
		fallthrough;
	case INTEL_BROADWELL_X:
		intel_rdt_mbm_apply_quirk();
		break;
	}
}

static __init void check_quirks(void)
{
	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL)
		__check_quirks_intel();
}

static __init bool get_rdt_resources(void)
{
	rdt_alloc_capable = get_rdt_alloc_resources();
	rdt_mon_capable = get_rdt_mon_resources();

	return (rdt_mon_capable || rdt_alloc_capable);
}

static __init void rdt_init_res_defs_intel(void)
{
	struct rdt_hw_resource *hw_res;
	struct rdt_resource *r;

	for_each_rdt_resource(r) {
		hw_res = resctrl_to_arch_res(r);

		if (r->rid == RDT_RESOURCE_L3 ||
		    r->rid == RDT_RESOURCE_L2) {
			r->cache.arch_has_per_cpu_cfg = false;
			r->cache.min_cbm_bits = 1;
		} else if (r->rid == RDT_RESOURCE_MBA) {
			hw_res->msr_base = MSR_IA32_MBA_THRTL_BASE;
			hw_res->msr_update = mba_wrmsr_intel;
		}
	}
}

static __init void rdt_init_res_defs_amd(void)
{
	struct rdt_hw_resource *hw_res;
	struct rdt_resource *r;

	for_each_rdt_resource(r) {
		hw_res = resctrl_to_arch_res(r);

		if (r->rid == RDT_RESOURCE_L3 ||
		    r->rid == RDT_RESOURCE_L2) {
			r->cache.arch_has_sparse_bitmasks = true;
			r->cache.arch_has_per_cpu_cfg = true;
			r->cache.min_cbm_bits = 0;
		} else if (r->rid == RDT_RESOURCE_MBA) {
			hw_res->msr_base = MSR_IA32_MBA_BW_BASE;
			hw_res->msr_update = mba_wrmsr_amd;
		} else if (r->rid == RDT_RESOURCE_SMBA) {
			hw_res->msr_base = MSR_IA32_SMBA_BW_BASE;
			hw_res->msr_update = mba_wrmsr_amd;
		}
	}
}

static __init void rdt_init_res_defs(void)
{
	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL)
		rdt_init_res_defs_intel();
	else if (boot_cpu_data.x86_vendor == X86_VENDOR_AMD)
		rdt_init_res_defs_amd();
}

static enum cpuhp_state rdt_online;

/* Runs once on the BSP during boot. */
void resctrl_cpu_detect(struct cpuinfo_x86 *c)
{
	if (!cpu_has(c, X86_FEATURE_CQM_LLC)) {
		c->x86_cache_max_rmid  = -1;
		c->x86_cache_occ_scale = -1;
		c->x86_cache_mbm_width_offset = -1;
		return;
	}

	/* will be overridden if occupancy monitoring exists */
	c->x86_cache_max_rmid = cpuid_ebx(0xf);

	if (cpu_has(c, X86_FEATURE_CQM_OCCUP_LLC) ||
	    cpu_has(c, X86_FEATURE_CQM_MBM_TOTAL) ||
	    cpu_has(c, X86_FEATURE_CQM_MBM_LOCAL)) {
		u32 eax, ebx, ecx, edx;

		/* QoS sub-leaf, EAX=0Fh, ECX=1 */
		cpuid_count(0xf, 1, &eax, &ebx, &ecx, &edx);

		c->x86_cache_max_rmid  = ecx;
		c->x86_cache_occ_scale = ebx;
		c->x86_cache_mbm_width_offset = eax & 0xff;

		if (c->x86_vendor == X86_VENDOR_AMD && !c->x86_cache_mbm_width_offset)
			c->x86_cache_mbm_width_offset = MBM_CNTR_WIDTH_OFFSET_AMD;
	}
}

static int __init resctrl_arch_late_init(void)
{
	struct rdt_resource *r;
	int state, ret, i;

	/* for_each_rdt_resource() requires all rid to be initialised. */
	for (i = 0; i < RDT_NUM_RESOURCES; i++)
		rdt_resources_all[i].r_resctrl.rid = i;

	/*
	 * Initialize functions(or definitions) that are different
	 * between vendors here.
	 */
	rdt_init_res_defs();

	check_quirks();

	if (!get_rdt_resources())
		return -ENODEV;

	state = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
				  "x86/resctrl/cat:online:",
				  resctrl_arch_online_cpu,
				  resctrl_arch_offline_cpu);
	if (state < 0)
		return state;

	ret = resctrl_init();
	if (ret) {
		cpuhp_remove_state(state);
		return ret;
	}
	rdt_online = state;

	for_each_alloc_capable_rdt_resource(r)
		pr_info("%s allocation detected\n", r->name);

	for_each_mon_capable_rdt_resource(r)
		pr_info("%s monitoring detected\n", r->name);

	return 0;
}

late_initcall(resctrl_arch_late_init);

static void __exit resctrl_arch_exit(void)
{
	cpuhp_remove_state(rdt_online);

	resctrl_exit();
}

__exitcall(resctrl_arch_exit);

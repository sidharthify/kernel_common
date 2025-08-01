// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 *
 * (C) COPYRIGHT 2014-2024 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 */

/* NOTES:
 * - A default GPU can be compiled in during the build, by defining
 *   CONFIG_MALI_NO_MALI_DEFAULT_GPU. SCons sets this, which means that
 *   insmod'ing mali_kbase.ko with no arguments after a build with "scons
 *   gpu=tXYZ" will yield the expected GPU ID for tXYZ. This can always be
 *   overridden by passing the 'no_mali_gpu' argument to insmod.
 */

#include <mali_kbase.h>
#include <device/mali_kbase_device.h>
#include <hw_access/mali_kbase_hw_access_regmap.h>
#include <hw_access/mali_kbase_hw_access_regmap_legacy.h>
#include <backend/gpu/mali_kbase_model_linux.h>
#include <mali_kbase_mem_linux.h>

#include <asm/arch_timer.h>

#if MALI_USE_CSF
#include <csf/mali_kbase_csf_firmware.h>

/* Index of the last value register for each type of core, with the 1st value
 * register being at index 0.
 */
#define IPA_CTL_MAX_VAL_CNT_IDX (KBASE_IPA_CONTROL_NUM_BLOCK_COUNTERS - 1)

/* Array for storing the value of SELECT register for each type of core */
static u64 ipa_ctl_select_config[KBASE_IPA_CORE_TYPE_NUM];
static u32 ipa_control_timer_enabled;
#endif

#if MALI_USE_CSF
static u32 sysc_alloc_regs[SYSC_ALLOC_COUNT];
#endif

#define LO_MASK(M) ((M)&0xFFFFFFFF)
#define HI_MASK(M) ((M)&0xFFFFFFFF00000000)

/* Construct a value for the THREAD_FEATURES register, *except* the two most
 * significant bits, which are set to THREAD_FEATURES_IMPLEMENTATION_TECHNOLOGY_SOFTWARE in
 * midgard_model_read_reg().
 */
#if MALI_USE_CSF
#define THREAD_FEATURES_PARTIAL(MAX_REGISTERS, MAX_TASK_QUEUE, MAX_TG_SPLIT) \
	((MAX_REGISTERS) | ((MAX_TASK_QUEUE) << 24))
#else
#define THREAD_FEATURES_PARTIAL(MAX_REGISTERS, MAX_TASK_QUEUE, MAX_TG_SPLIT) \
	((MAX_REGISTERS) | ((MAX_TASK_QUEUE) << 16) | ((MAX_TG_SPLIT) << 24))
#endif

struct error_status_t hw_error_status;

/**
 * struct control_reg_values_t - control register values specific to the GPU being 'emulated'
 * @name:			GPU name
 * @gpu_id:			GPU ID to report
 * @as_present:			Bitmap of address spaces present
 * @thread_max_threads:		Maximum number of threads per core
 * @thread_max_workgroup_size:	Maximum number of threads per workgroup
 * @thread_max_barrier_size:	Maximum number of threads per barrier
 * @thread_features:		Thread features, NOT INCLUDING the 2
 *				most-significant bits, which are always set to
 *				THREAD_FEATURES_IMPLEMENTATION_TECHNOLOGY_SOFTWARE.
 * @core_features:		Core features
 * @tiler_features:		Tiler features
 * @mmu_features:		MMU features
 * @gpu_features_lo:		GPU features (low)
 * @gpu_features_hi:		GPU features (high)
 * @shader_present:		Available shader bitmap
 * @stack_present:		Core stack present bitmap
 * @base_present:		Shader core base present bitmap
 * @neural_present:		Neural engine present bitmap
 *
 */
struct control_reg_values_t {
	const char *name;
	u64 gpu_id;
	u32 as_present;
	u32 thread_max_threads;
	u32 thread_max_workgroup_size;
	u32 thread_max_barrier_size;
	u32 thread_features;
	u32 core_features;
	u32 tiler_features;
	u32 mmu_features;
	u32 gpu_features_lo;
	u32 gpu_features_hi;
	u32 shader_present;
	u32 stack_present;
	u64 base_present;
	u64 neural_present;
};

struct job_slot {
	int job_active;
	int job_queued;
	u32 job_complete_irq_asserted;
	u32 job_irq_mask;
	int job_disabled;
};

enum pwr_on_index {
	INDEX_L2,
	INDEX_TILER,
	INDEX_SHADER,
	INDEX_STACK,
#if MALI_USE_CSF
	INDEX_BASE,
	INDEX_NEURAL,
#endif
	INDEX_DOMAIN_COUNT
};

struct dummy_model_t {
	int reset_completed;
	int reset_completed_mask;
#if !MALI_USE_CSF
	int prfcnt_sample_completed;
#endif /* !MALI_USE_CSF */
	int power_changed_mask; /* 2 bits: _ALL,_SINGLE */
	int power_changed; /* 1 bit */
	bool clean_caches_completed;
	bool clean_caches_completed_irq_enabled;
#if MALI_USE_CSF
	bool flush_pa_range_completed;
	bool flush_pa_range_completed_irq_enabled;
	/* Representations of COMMAND_NOT_ALLOWED and COMMAND_INVALID bits in
	 * the PWR_IRQ_* registers.
	 * The _mask variants enable and disable the respective IRQ sources
	 */
	bool command_not_allowed_mask; /* 1 bit */
	bool command_not_allowed; /* 1 bit */
	bool command_invalid_mask; /* 1 bit */
	bool command_invalid; /* 1 bit */

	u64 command_arg; /* PWR_CMDARG register */
	u64 gov_core_mask;
#endif
	uint32_t domain_power_on[INDEX_DOMAIN_COUNT];
	u32 coherency_enable;
	unsigned int job_irq_js_state;
	struct job_slot slots[NUM_SLOTS];
	const struct control_reg_values_t *control_reg_values;
	u32 l2_config;
	struct kbase_device *kbdev;
};

/* Array associating GPU names with control register values. The first
 * one is used in the case of no match.
 */
static const struct control_reg_values_t all_control_reg_values[] = {
	{
		.name = "tMIx",
		.gpu_id = GPU_ID2_MAKE(6, 0, 10, 0, 0, 1, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 10),
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tHEx",
		.gpu_id = GPU_ID2_MAKE(6, 2, 0, 1, 0, 3, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 10),
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tSIx",
		.gpu_id = GPU_ID2_MAKE(7, 0, 0, 0, 1, 1, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x300,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 10),
		.tiler_features = 0x209,
		.mmu_features = 0x2821,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tDVx",
		.gpu_id = GPU_ID2_MAKE(7, 0, 0, 3, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x300,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 10),
		.tiler_features = 0x209,
		.mmu_features = 0x2821,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tNOx",
		.gpu_id = GPU_ID2_MAKE(7, 2, 1, 1, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 10),
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tGOx_r0p0",
		.gpu_id = GPU_ID2_MAKE(7, 2, 2, 2, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 10),
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tGOx_r1p0",
		.gpu_id = GPU_ID2_MAKE(7, 4, 0, 2, 1, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 10),
		.core_features = 0x2,
		.tiler_features = 0x209,
		.mmu_features = 0x2823,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tTRx",
		.gpu_id = GPU_ID2_MAKE(9, 0, 8, 0, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 0),
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tNAx",
		.gpu_id = GPU_ID2_MAKE(9, 0, 8, 1, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 0),
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tBEx",
		.gpu_id = GPU_ID2_MAKE(9, 2, 0, 2, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 0),
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT_TBEX,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tBAx",
		.gpu_id = GPU_ID2_MAKE(9, 14, 4, 5, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 0),
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tODx",
		.gpu_id = GPU_ID2_MAKE(10, 8, 0, 2, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 0),
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT_TODX,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tGRx",
		.gpu_id = GPU_ID2_MAKE(10, 10, 0, 3, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 0),
		.core_features = 0x0, /* core_1e16fma2tex */
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tVAx",
		.gpu_id = GPU_ID2_MAKE(10, 12, 0, 4, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x180,
		.thread_max_workgroup_size = 0x180,
		.thread_max_barrier_size = 0x180,
		.thread_features = THREAD_FEATURES_PARTIAL(0x6000, 4, 0),
		.core_features = 0x0, /* core_1e16fma2tex */
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tTUx",
		.gpu_id = GPU_ID2_MAKE(11, 8, 5, 2, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x800,
		.thread_max_workgroup_size = 0x400,
		.thread_max_barrier_size = 0x400,
		.thread_features = THREAD_FEATURES_PARTIAL(0x10000, 4, 0),
		.core_features = 0x0, /* core_1e32fma2tex */
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0xf,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT_TTUX,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tTIx",
		.gpu_id = GPU_ID2_MAKE(12, 8, 1, 0, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x800,
		.thread_max_workgroup_size = 0x400,
		.thread_max_barrier_size = 0x400,
		.thread_features = THREAD_FEATURES_PARTIAL(0x10000, 16, 0),
		.core_features = 0x1, /* core_1e64fma4tex */
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0xf,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT_TTIX,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tKRx",
		.gpu_id = GPU_ID2_MAKE(13, 8, 1, 0, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x800,
		.thread_max_workgroup_size = 0x400,
		.thread_max_barrier_size = 0x400,
		.thread_features = THREAD_FEATURES_PARTIAL(0x10000, 16, 0),
		.core_features = 0x1, /* core_1e64fma4tex */
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0xf,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT_TKRX,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
	},
	{
		.name = "tDRx",
		.gpu_id = GPU_ID2_MAKE(14, 8, 5, 0, 0, 0, 0),
		.as_present = 0xFF,
		.thread_max_threads = 0x800,
		.thread_max_workgroup_size = 0x400,
		.thread_max_barrier_size = 0x400,
		.thread_features = THREAD_FEATURES_PARTIAL(0x10000, 16, 0),
		.core_features = 0x1, /* core_1e64fma4tex */
		.tiler_features = 0x809,
		.mmu_features = 0x2830,
		.gpu_features_lo = 0x3f,
		.gpu_features_hi = 0,
		.shader_present = DUMMY_IMPLEMENTATION_SHADER_PRESENT_TDRX,
		.stack_present = DUMMY_IMPLEMENTATION_STACK_PRESENT,
		.base_present = DUMMY_IMPLEMENTATION_BASE_PRESENT,
		.neural_present = DUMMY_IMPLEMENTATION_NEURAL_PRESENT,
	},
};

static struct {
	spinlock_t access_lock;
#if !MALI_USE_CSF
	unsigned long prfcnt_base;
#endif /* !MALI_USE_CSF */
	u32 *prfcnt_base_cpu;

	u32 time;

	struct gpu_model_prfcnt_en prfcnt_en;

	u64 l2_present;
	u64 shader_present;

#if !MALI_USE_CSF
	u64 jm_counters[KBASE_DUMMY_MODEL_COUNTER_PER_CORE];
#else
	u64 cshw_counters[KBASE_DUMMY_MODEL_COUNTER_PER_CORE];
#endif /* !MALI_USE_CSF */
	u64 tiler_counters[KBASE_DUMMY_MODEL_COUNTER_PER_CORE];
	u64 l2_counters[KBASE_DUMMY_MODEL_MAX_MEMSYS_BLOCKS * KBASE_DUMMY_MODEL_COUNTER_PER_CORE];
	u64 shader_counters[KBASE_DUMMY_MODEL_MAX_SHADER_CORES * KBASE_DUMMY_MODEL_COUNTER_PER_CORE];
} performance_counters;

/**
 * get_implementation_register - Returns the value of the register
 *
 * @reg: Register address
 * @control_reg_values: Struct containing the implementations of the registers
 *
 * Registers of the dummy model are implemented in the control_reg_values_t struct
 * We are only concerned with the lower 32 bits in the dummy model
 *
 * Return: value of the register for the current control_reg_values_t
 */
static u32 get_implementation_register(u32 reg,
				       const struct control_reg_values_t *const control_reg_values)
{
	switch (reg) {
	case GPU_CONTROL_REG(SHADER_PRESENT_LO):
		return LO_MASK(control_reg_values->shader_present);
	case GPU_CONTROL_REG(TILER_PRESENT_LO):
		return LO_MASK(DUMMY_IMPLEMENTATION_TILER_PRESENT);
	case GPU_CONTROL_REG(L2_PRESENT_LO):
		return LO_MASK(DUMMY_IMPLEMENTATION_L2_PRESENT);
	case GPU_CONTROL_REG(STACK_PRESENT_LO):
		return LO_MASK(control_reg_values->stack_present);
	default:
		return 0;
	}
}

#if MALI_USE_CSF
static u32
hctrl_get_implementation_register(u32 reg,
				  const struct control_reg_values_t *const control_reg_values)
{
	switch (reg) {
	case HOST_POWER_REG(HOST_POWER_SHADER_PRESENT_LO):
		return LO_MASK(control_reg_values->shader_present);
	case HOST_POWER_REG(HOST_POWER_TILER_PRESENT_LO):
		return LO_MASK(DUMMY_IMPLEMENTATION_TILER_PRESENT);
	case HOST_POWER_REG(HOST_POWER_L2_PRESENT_LO):
		return LO_MASK(DUMMY_IMPLEMENTATION_L2_PRESENT);
	case HOST_POWER_REG(HOST_POWER_STACK_PRESENT_LO):
		return LO_MASK(control_reg_values->stack_present);
	case HOST_POWER_REG(HOST_POWER_BASE_PRESENT_LO):
		return LO_MASK(control_reg_values->base_present);
	case HOST_POWER_REG(HOST_POWER_NEURAL_PRESENT_LO):
		if (control_reg_values->gpu_features_lo & GPU_FEATURES_NEURAL_ENGINE_MASK)
			return LO_MASK(control_reg_values->neural_present);
		fallthrough;
	default:
		return 0;
	}
}
#endif

void gpu_device_set_data(void *model, void *data)
{
	struct dummy_model_t *dummy = (struct dummy_model_t *)model;

	dummy->kbdev = data;
}

void *gpu_device_get_data(void *model)
{
	struct dummy_model_t *dummy = (struct dummy_model_t *)model;

	return dummy->kbdev;
}

#define signal_int(m, s) m->slots[(s)].job_complete_irq_asserted = 1u

static char *no_mali_gpu = CONFIG_MALI_NO_MALI_DEFAULT_GPU;
module_param(no_mali_gpu, charp, 0000);
MODULE_PARM_DESC(no_mali_gpu, "GPU to identify as");

#if MALI_USE_CSF
static u32 gpu_model_get_prfcnt_value(enum kbase_ipa_core_type core_type, u32 cnt_idx,
				      bool is_low_word)
{
	u64 *counters_data = NULL;
	u32 core_count = 0;
	u32 event_index;
	u64 value = 0;
	u32 core;
	u32 num_cores = 1;
	unsigned long flags;

	if (WARN_ON(core_type >= KBASE_IPA_CORE_TYPE_NUM))
		return 0;

	if (WARN_ON(cnt_idx >= KBASE_IPA_CONTROL_NUM_BLOCK_COUNTERS))
		return 0;

	event_index = (ipa_ctl_select_config[core_type] >> (cnt_idx * 8)) & 0xFF;

	if (core_type == KBASE_IPA_CORE_TYPE_SHADER)
		num_cores = KBASE_DUMMY_MODEL_MAX_SHADER_CORES;

	if (WARN_ON(event_index >= (KBASE_DUMMY_MODEL_COUNTER_HEADER_DWORDS +
				    KBASE_DUMMY_MODEL_COUNTER_PER_CORE * num_cores)))
		return 0;

	/* The actual events start index 4 onwards. Spec also says PRFCNT_EN,
	 * TIMESTAMP_LO or TIMESTAMP_HI pseudo-counters do not make sense for
	 * IPA counters. If selected, the value returned for them will be zero.
	 */
	if (WARN_ON(event_index < KBASE_DUMMY_MODEL_COUNTER_HEADER_DWORDS))
		return 0;

	event_index -= KBASE_DUMMY_MODEL_COUNTER_HEADER_DWORDS;

	spin_lock_irqsave(&performance_counters.access_lock, flags);

	switch (core_type) {
	case KBASE_IPA_CORE_TYPE_CSHW:
		core_count = 1;
		counters_data = performance_counters.cshw_counters;
		break;
	case KBASE_IPA_CORE_TYPE_MEMSYS:
		core_count = hweight64(performance_counters.l2_present);
		counters_data = performance_counters.l2_counters;
		break;
	case KBASE_IPA_CORE_TYPE_TILER:
		core_count = 1;
		counters_data = performance_counters.tiler_counters;
		break;
	case KBASE_IPA_CORE_TYPE_SHADER:
		core_count = hweight64(performance_counters.shader_present);
		counters_data = performance_counters.shader_counters;
		break;
	default:
		WARN(1, "Invalid core_type %d\n", core_type);
		break;
	}

	if (unlikely(counters_data == NULL))
		return 0;

	for (core = 0; core < core_count; core++) {
		value += counters_data[event_index];
		event_index += KBASE_DUMMY_MODEL_COUNTER_PER_CORE;
	}

	spin_unlock_irqrestore(&performance_counters.access_lock, flags);

	if (is_low_word)
		return (value & U32_MAX);
	else
		return (value >> 32);
}
#endif /* MALI_USE_CSF */

/**
 * gpu_model_clear_prfcnt_values_nolock - Clear performance counter values
 *
 * Sets all performance counter values to zero. The performance counter access
 * lock must be held when calling this function.
 */
static void gpu_model_clear_prfcnt_values_nolock(void)
{
	lockdep_assert_held(&performance_counters.access_lock);
#if !MALI_USE_CSF
	memset(performance_counters.jm_counters, 0, sizeof(performance_counters.jm_counters));
#else
	memset(performance_counters.cshw_counters, 0, sizeof(performance_counters.cshw_counters));
#endif /* !MALI_USE_CSF */
	memset(performance_counters.tiler_counters, 0, sizeof(performance_counters.tiler_counters));
	memset(performance_counters.l2_counters, 0, sizeof(performance_counters.l2_counters));
	memset(performance_counters.shader_counters, 0,
	       sizeof(performance_counters.shader_counters));
}

#if MALI_USE_CSF
void gpu_model_clear_prfcnt_values(void)
{
	unsigned long flags;

	spin_lock_irqsave(&performance_counters.access_lock, flags);
	gpu_model_clear_prfcnt_values_nolock();
	spin_unlock_irqrestore(&performance_counters.access_lock, flags);
}
KBASE_EXPORT_TEST_API(gpu_model_clear_prfcnt_values);
#endif /* MALI_USE_CSF */

/**
 * gpu_model_dump_prfcnt_blocks() - Dump performance counter values to buffer
 *
 * @values:             Array of values to be written out
 * @out_index:          Index into performance counter buffer
 * @block_count:        Number of blocks to dump
 * @prfcnt_enable_mask: Counter enable mask
 * @blocks_present:     Available blocks bit mask
 *
 * The performance counter access lock must be held before calling this
 * function.
 */
static void gpu_model_dump_prfcnt_blocks(u64 *values, u32 *out_index, u32 block_count,
					 u32 prfcnt_enable_mask, u64 blocks_present)
{
	u32 block_idx, counter;
	u32 counter_value = 0;
	u32 *prfcnt_base;
	u32 index = 0;

	lockdep_assert_held(&performance_counters.access_lock);

	prfcnt_base = performance_counters.prfcnt_base_cpu;

	for (block_idx = 0; block_idx < block_count; block_idx++) {
		/* only dump values if core is present */
		if (!(blocks_present & (1U << block_idx))) {
#if MALI_USE_CSF
			/* if CSF dump zeroed out block */
			memset(&prfcnt_base[*out_index], 0, KBASE_DUMMY_MODEL_BLOCK_SIZE);
			*out_index += KBASE_DUMMY_MODEL_VALUES_PER_BLOCK;
#endif /* MALI_USE_CSF */
			continue;
		}

		/* write the header */
		prfcnt_base[*out_index] = performance_counters.time++;
		prfcnt_base[*out_index + 2] = prfcnt_enable_mask;
		*out_index += KBASE_DUMMY_MODEL_COUNTER_HEADER_DWORDS;

		/* write the counters */
		for (counter = 0; counter < KBASE_DUMMY_MODEL_COUNTER_PER_CORE; counter++) {
			/* HW counter values retrieved through
			 * PRFCNT_SAMPLE request are of 32 bits only.
			 */
			counter_value = (u32)values[index++];
			if (KBASE_DUMMY_MODEL_COUNTER_ENABLED(
				    prfcnt_enable_mask,
				    (counter + KBASE_DUMMY_MODEL_COUNTER_HEADER_DWORDS))) {
				prfcnt_base[*out_index + counter] = counter_value;
			}
		}
		*out_index += KBASE_DUMMY_MODEL_COUNTER_PER_CORE;
	}
}

static void gpu_model_dump_nolock(void)
{
	u32 index = 0;

	lockdep_assert_held(&performance_counters.access_lock);

#if !MALI_USE_CSF
	gpu_model_dump_prfcnt_blocks(performance_counters.jm_counters, &index, 1,
				     performance_counters.prfcnt_en.fe, 0x1);
#else
	gpu_model_dump_prfcnt_blocks(performance_counters.cshw_counters, &index, 1,
				     performance_counters.prfcnt_en.fe, 0x1);
#endif /* !MALI_USE_CSF */
	gpu_model_dump_prfcnt_blocks(performance_counters.tiler_counters, &index, 1,
				     performance_counters.prfcnt_en.tiler,
				     DUMMY_IMPLEMENTATION_TILER_PRESENT);
	gpu_model_dump_prfcnt_blocks(performance_counters.l2_counters, &index,
				     KBASE_DUMMY_MODEL_MAX_MEMSYS_BLOCKS,
				     performance_counters.prfcnt_en.l2,
				     performance_counters.l2_present);
	gpu_model_dump_prfcnt_blocks(performance_counters.shader_counters, &index,
				     KBASE_DUMMY_MODEL_MAX_SHADER_CORES,
				     performance_counters.prfcnt_en.shader,
				     performance_counters.shader_present);

	/* Counter values are cleared after each dump */
	gpu_model_clear_prfcnt_values_nolock();

	/* simulate a 'long' time between samples */
	performance_counters.time += 10;
}

static void gpu_model_raise_irq(void *model, u32 irq)
{
	struct kbase_device *kbdev = gpu_device_get_data(model);

	/*
	 * Use unified IRQ handler from GPU Arch version 14.8.0
	 */
	if (kbdev->gpu_props.gpu_id.arch_id >= GPU_ID_ARCH_MAKE(14, 8, 0))
		gpu_device_raise_irq(model, MODEL_LINUX_IRQAW_IRQ);
	else
		gpu_device_raise_irq(model, irq);
}

#if !MALI_USE_CSF
static void midgard_model_dump_prfcnt(void)
{
	unsigned long flags;

	spin_lock_irqsave(&performance_counters.access_lock, flags);
	gpu_model_dump_nolock();
	spin_unlock_irqrestore(&performance_counters.access_lock, flags);
}
#else
void gpu_model_prfcnt_dump_request(u32 *sample_buf, struct gpu_model_prfcnt_en enable_maps)
{
	unsigned long flags;

	if (WARN_ON(!sample_buf))
		return;

	spin_lock_irqsave(&performance_counters.access_lock, flags);
	performance_counters.prfcnt_base_cpu = sample_buf;
	performance_counters.prfcnt_en = enable_maps;
	gpu_model_dump_nolock();
	spin_unlock_irqrestore(&performance_counters.access_lock, flags);
}

void gpu_model_glb_request_job_irq(void *model)
{
	unsigned long flags;

	spin_lock_irqsave(&hw_error_status.access_lock, flags);
	hw_error_status.job_irq_status |= JOB_IRQ_GLOBAL_IF;
	spin_unlock_irqrestore(&hw_error_status.access_lock, flags);
	gpu_model_raise_irq(model, MODEL_LINUX_JOB_IRQ);
}
#endif /* !MALI_USE_CSF */

static void init_register_statuses(struct dummy_model_t *dummy)
{
	int i;

	hw_error_status.errors_mask = 0;
	hw_error_status.gpu_error_irq = 0;
	hw_error_status.gpu_fault_status = 0;
	hw_error_status.job_irq_rawstat = 0;
	hw_error_status.job_irq_status = 0;
	hw_error_status.mmu_irq_rawstat = 0;
	hw_error_status.mmu_irq_mask = 0;

	for (i = 0; i < NUM_SLOTS; i++) {
		hw_error_status.js_status[i] = 0;
		hw_error_status.job_irq_rawstat |= (dummy->slots[i].job_complete_irq_asserted) << i;
		hw_error_status.job_irq_status |= (dummy->slots[i].job_complete_irq_asserted) << i;
	}
	for (i = 0; i < NUM_MMU_AS; i++) {
		hw_error_status.as_command[i] = 0;
		hw_error_status.as_faultstatus[i] = 0;
		hw_error_status.mmu_irq_mask |= (1u << i);
	}

	performance_counters.time = 0;
}

static void update_register_statuses(struct dummy_model_t *dummy, u32 job_slot)
{
	lockdep_assert_held(&hw_error_status.access_lock);

	if (hw_error_status.errors_mask & IS_A_JOB_ERROR) {
		if (job_slot == hw_error_status.current_job_slot) {
#if !MALI_USE_CSF
			if (hw_error_status.js_status[job_slot] == 0) {
				/* status reg is clean; it can be written */

				switch (hw_error_status.errors_mask & IS_A_JOB_ERROR) {
				case KBASE_JOB_INTERRUPTED:
					hw_error_status.js_status[job_slot] = JS_STATUS_INTERRUPTED;
					break;

				case KBASE_JOB_STOPPED:
					hw_error_status.js_status[job_slot] = JS_STATUS_STOPPED;
					break;

				case KBASE_JOB_TERMINATED:
					hw_error_status.js_status[job_slot] = JS_STATUS_TERMINATED;
					break;

				case KBASE_JOB_CONFIG_FAULT:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_CONFIG_FAULT;
					break;

				case KBASE_JOB_POWER_FAULT:
					hw_error_status.js_status[job_slot] = JS_STATUS_POWER_FAULT;
					break;

				case KBASE_JOB_READ_FAULT:
					hw_error_status.js_status[job_slot] = JS_STATUS_READ_FAULT;
					break;

				case KBASE_JOB_WRITE_FAULT:
					hw_error_status.js_status[job_slot] = JS_STATUS_WRITE_FAULT;
					break;

				case KBASE_JOB_AFFINITY_FAULT:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_AFFINITY_FAULT;
					break;

				case KBASE_JOB_BUS_FAULT:
					hw_error_status.js_status[job_slot] = JS_STATUS_BUS_FAULT;
					break;

				case KBASE_INSTR_INVALID_PC:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_INSTR_INVALID_PC;
					break;

				case KBASE_INSTR_INVALID_ENC:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_INSTR_INVALID_ENC;
					break;

				case KBASE_INSTR_TYPE_MISMATCH:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_INSTR_TYPE_MISMATCH;
					break;

				case KBASE_INSTR_OPERAND_FAULT:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_INSTR_OPERAND_FAULT;
					break;

				case KBASE_INSTR_TLS_FAULT:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_INSTR_TLS_FAULT;
					break;

				case KBASE_INSTR_BARRIER_FAULT:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_INSTR_BARRIER_FAULT;
					break;

				case KBASE_INSTR_ALIGN_FAULT:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_INSTR_ALIGN_FAULT;
					break;

				case KBASE_DATA_INVALID_FAULT:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_DATA_INVALID_FAULT;
					break;

				case KBASE_TILE_RANGE_FAULT:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_TILE_RANGE_FAULT;
					break;

				case KBASE_ADDR_RANGE_FAULT:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_ADDRESS_RANGE_FAULT;
					break;

				case KBASE_OUT_OF_MEMORY:
					hw_error_status.js_status[job_slot] =
						JS_STATUS_OUT_OF_MEMORY;
					break;

				case KBASE_UNKNOWN:
					hw_error_status.js_status[job_slot] = JS_STATUS_UNKNOWN;
					break;

				default:
					model_error_log(KBASE_CORE,
							"\nAtom Chain 0x%llx: Invalid Error Mask!",
							hw_error_status.current_jc);
					break;
				}
			}
#endif /* !MALI_USE_CSF */

			/* we set JOB_FAIL_<n> */
			hw_error_status.job_irq_rawstat |=
				(dummy->slots[job_slot].job_complete_irq_asserted)
				<< (job_slot + 16);
			hw_error_status.job_irq_status |=
				(((dummy->slots[job_slot].job_complete_irq_asserted)
				  << (job_slot)) &
				 (dummy->slots[job_slot].job_irq_mask << job_slot))
				<< 16;
		} else {
			hw_error_status.job_irq_rawstat |=
				(dummy->slots[job_slot].job_complete_irq_asserted) << job_slot;
			hw_error_status.job_irq_status |=
				((dummy->slots[job_slot].job_complete_irq_asserted) << (job_slot)) &
				(dummy->slots[job_slot].job_irq_mask << job_slot);
		}
	} else {
		hw_error_status.job_irq_rawstat |=
			(dummy->slots[job_slot].job_complete_irq_asserted) << job_slot;
		hw_error_status.job_irq_status |=
			((dummy->slots[job_slot].job_complete_irq_asserted) << (job_slot)) &
			(dummy->slots[job_slot].job_irq_mask << job_slot);
	} /* end of job register statuses */

	if (hw_error_status.errors_mask & IS_A_MMU_ERROR) {
		u32 i;

		for (i = 0; i < NUM_MMU_AS; i++) {
			if (i == hw_error_status.faulty_mmu_as) {
				if (hw_error_status.as_faultstatus[i] == 0) {
					u32 status = hw_error_status.as_faultstatus[i];
					/* status reg is clean; it can be
					 * written
					 */
					switch (hw_error_status.errors_mask & IS_A_MMU_ERROR) {
					case KBASE_TRANSLATION_FAULT:
						/* 0xCm means TRANSLATION FAULT
						 * (m is mmu_table_level)
						 */
						status = ((1 << 7) | (1 << 6) |
							  hw_error_status.mmu_table_level);
						break;

					case KBASE_PERMISSION_FAULT:
						/*0xC8 means PERMISSION FAULT */
						status = ((1 << 7) | (1 << 6) | (1 << 3));
						break;

					case KBASE_TRANSTAB_BUS_FAULT:
						/* 0xDm means TRANSITION TABLE
						 * BUS FAULT (m is
						 * mmu_table_level)
						 */
						status = ((1 << 7) | (1 << 6) | (1 << 4) |
							  hw_error_status.mmu_table_level);
						break;

					case KBASE_ACCESS_FLAG:
						/* 0xD8 means ACCESS FLAG */
						status =
							((1 << 7) | (1 << 6) | (1 << 4) | (1 << 3));
						break;

					default:
						model_error_log(
							KBASE_CORE,
							"\nAtom Chain 0x%llx: Invalid Error Mask!",
							hw_error_status.current_jc);
						break;
					}
					hw_error_status.as_faultstatus[i] = status;
				}

				if (hw_error_status.errors_mask & KBASE_TRANSTAB_BUS_FAULT)
					hw_error_status.mmu_irq_rawstat |=
						1u << (16 + i); /* bus error */
				else
					hw_error_status.mmu_irq_rawstat |=
						(1u << i); /* page fault */
			}
		}
	} /*end of mmu register statuses */
	if (hw_error_status.errors_mask & IS_A_GPU_ERROR) {
		if (hw_error_status.gpu_fault_status) {
			/* not the first GPU error reported */
			hw_error_status.gpu_error_irq |= (1 << 7);
		} else {
			hw_error_status.gpu_error_irq |= 1;
			switch (hw_error_status.errors_mask & IS_A_GPU_ERROR) {
			case KBASE_DELAYED_BUS_FAULT:
				hw_error_status.gpu_fault_status = (1u << 7);
				break;

			case KBASE_SHAREABILITY_FAULT:
				hw_error_status.gpu_fault_status = (1u << 7) | (1u << 3);
				break;

			default:
				model_error_log(KBASE_CORE,
						"\nAtom Chain 0x%llx: Invalid Error Mask!",
						hw_error_status.current_jc);
				break;
			}
		}
	}
	hw_error_status.errors_mask = 0; /*clear error mask */
}

#if !MALI_USE_CSF
static void update_job_irq_js_state(struct dummy_model_t *dummy, int mask)
{
	int i;

	lockdep_assert_held(&hw_error_status.access_lock);
	pr_debug("%s", "Updating the JS_ACTIVE register");

	for (i = 0; i < NUM_SLOTS; i++) {
		int slot_active = dummy->slots[i].job_active;
		int next_busy = dummy->slots[i].job_queued;

		if ((mask & (1 << i)) || (mask & (1 << (i + 16)))) {
			/* clear the bits we're updating */
			dummy->job_irq_js_state &= ~((1 << (16 + i)) | (1 << i));
			if (hw_error_status.js_status[i]) {
				dummy->job_irq_js_state |= next_busy << (i + 16);
				if (mask & (1 << (i + 16))) {
					/* clear job slot status */
					hw_error_status.js_status[i] = 0;
					/* continue execution of jobchain */
					dummy->slots[i].job_active = dummy->slots[i].job_queued;
				}
			} else {
				/* set bits if needed */
				dummy->job_irq_js_state |=
					((slot_active << i) | (next_busy << (i + 16)));
			}
		}
	}
	pr_debug("The new snapshot is 0x%08X\n", dummy->job_irq_js_state);
}
#endif /* !MALI_USE_CSF */

/**
 * find_control_reg_values() - Look up constant control register values.
 * @gpu:	GPU name
 *
 * Look up the GPU name to find the correct set of control register values for
 * that GPU. If not found, warn and use the first values in the array.
 *
 * Return: Pointer to control register values for that GPU.
 */
static const struct control_reg_values_t *find_control_reg_values(const char *gpu)
{
	size_t i;
	const struct control_reg_values_t *ret = NULL;

	/* Edge case for tGOx, as it has 2 entries in the table for its R0 and R1
	 * revisions respectively. As none of them are named "tGOx" the name comparison
	 * needs to be fixed in these cases. CONFIG_GPU_HWVER should be one of "r0p0"
	 * or "r1p0" and is derived from the DDK's build configuration. In cases
	 * where it is unavailable, it defaults to tGOx r1p0.
	 */
	if (!strcmp(gpu, "tGOx")) {
#ifdef CONFIG_GPU_HWVER
		if (!strcmp(CONFIG_GPU_HWVER, "r0p0"))
			gpu = "tGOx_r0p0";
		else if (!strcmp(CONFIG_GPU_HWVER, "r1p0"))
#endif /* CONFIG_GPU_HWVER defined */
			gpu = "tGOx_r1p0";
	}

	for (i = 0; i < ARRAY_SIZE(all_control_reg_values); ++i) {
		const struct control_reg_values_t *const fcrv = &all_control_reg_values[i];

		if (!strcmp(fcrv->name, gpu)) {
			ret = fcrv;
			pr_debug("Found control register values for %s\n", gpu);
			break;
		}
	}

	if (!ret) {
		ret = &all_control_reg_values[0];
		pr_warn("Couldn't find control register values for GPU %s; using default %s\n", gpu,
			ret->name);
	}

	return ret;
}

void *midgard_model_create(struct kbase_device *kbdev)
{
	struct dummy_model_t *dummy = NULL;

	spin_lock_init(&hw_error_status.access_lock);
	spin_lock_init(&performance_counters.access_lock);

	dummy = kzalloc(sizeof(*dummy), GFP_KERNEL);

	if (dummy) {
		dummy->job_irq_js_state = 0;
		init_register_statuses(dummy);
		dummy->control_reg_values = find_control_reg_values(no_mali_gpu);
#if MALI_USE_CSF
		if (kbdev->pm.backend.has_host_pwr_iface) {
			performance_counters.l2_present = hctrl_get_implementation_register(
				HOST_POWER_REG(HOST_POWER_L2_PRESENT_LO),
				dummy->control_reg_values);
			performance_counters.shader_present = hctrl_get_implementation_register(
				HOST_POWER_REG(HOST_POWER_SHADER_PRESENT_LO),
				dummy->control_reg_values);
		} else
#endif /* MALI_USE_CSF */
		{
			performance_counters.l2_present = get_implementation_register(
				GPU_CONTROL_REG(L2_PRESENT_LO), dummy->control_reg_values);
			performance_counters.shader_present = get_implementation_register(
				GPU_CONTROL_REG(SHADER_PRESENT_LO), dummy->control_reg_values);
		}

		gpu_device_set_data(dummy, kbdev);

		dev_info(kbdev->dev, "Using Dummy Model");
	}

	return dummy;
}

void midgard_model_destroy(void *h)
{
	kfree((void *)h);
}

static void midgard_model_get_outputs(void *h)
{
	struct dummy_model_t *dummy = (struct dummy_model_t *)h;

	lockdep_assert_held(&hw_error_status.access_lock);

	if (hw_error_status.job_irq_status)
		gpu_model_raise_irq(dummy, MODEL_LINUX_JOB_IRQ);

	if ((dummy->power_changed && dummy->power_changed_mask) ||
	    (dummy->reset_completed & dummy->reset_completed_mask) ||
	    hw_error_status.gpu_error_irq ||
#if !MALI_USE_CSF
	    dummy->prfcnt_sample_completed ||
#else
	    (dummy->flush_pa_range_completed && dummy->flush_pa_range_completed_irq_enabled) ||
#endif
	    (dummy->clean_caches_completed && dummy->clean_caches_completed_irq_enabled))
		gpu_model_raise_irq(dummy, MODEL_LINUX_GPU_IRQ);

	if (hw_error_status.mmu_irq_rawstat & hw_error_status.mmu_irq_mask)
		gpu_model_raise_irq(dummy, MODEL_LINUX_MMU_IRQ);
}

static void midgard_model_update(void *h)
{
	struct dummy_model_t *dummy = (struct dummy_model_t *)h;
	u32 i;

	lockdep_assert_held(&hw_error_status.access_lock);

	for (i = 0; i < NUM_SLOTS; i++) {
		if (!dummy->slots[i].job_active)
			continue;

		if (dummy->slots[i].job_disabled) {
			update_register_statuses(dummy, i);
			continue;
		}

		/* If there are any pending interrupts that have not
		 * been cleared we cannot run the job in the next register
		 * as we will overwrite the register status of the job in
		 * the head registers - which has not yet been read
		 */
		if ((hw_error_status.job_irq_rawstat & (1u << (i + 16))) ||
		    (hw_error_status.job_irq_rawstat & (1u << i))) {
			continue;
		}

		/*this job is done assert IRQ lines */
		signal_int(dummy, i);
		update_register_statuses(dummy, i);
		/*if this job slot returned failures we cannot use it */
		if (hw_error_status.job_irq_rawstat & (1u << (i + 16))) {
			dummy->slots[i].job_active = 0;
			continue;
		}
		/*process next job */
		dummy->slots[i].job_active = dummy->slots[i].job_queued;
		dummy->slots[i].job_queued = 0;
		if (dummy->slots[i].job_active) {
			if (hw_error_status.job_irq_rawstat & (1u << (i + 16)))
				model_error_log(KBASE_CORE,
						"\natom %lld running a job on a dirty slot",
						hw_error_status.current_jc);
		}
	}
}

static void invalidate_active_jobs(struct dummy_model_t *dummy)
{
	int i;

	lockdep_assert_held(&hw_error_status.access_lock);

	for (i = 0; i < NUM_SLOTS; i++) {
		if (dummy->slots[i].job_active) {
			hw_error_status.job_irq_rawstat |= (1u << (16 + i));

			hw_error_status.js_status[i] = 0x7f; /*UNKNOWN*/
		}
	}
}

void midgard_model_write_reg(void *h, u32 addr, u32 value)
{
	unsigned long flags;
	struct dummy_model_t *dummy = (struct dummy_model_t *)h;

	spin_lock_irqsave(&hw_error_status.access_lock, flags);

#if !MALI_USE_CSF
	if ((addr >= JOB_CONTROL_REG(JOB_SLOT0)) && (addr < (JOB_CONTROL_REG(JOB_SLOT15) + 0x80))) {
		unsigned int slot_idx = (addr >> 7) & 0xf;

		KBASE_DEBUG_ASSERT(slot_idx < NUM_SLOTS);
		if (addr == JOB_SLOT_REG(slot_idx, JS_HEAD_NEXT_LO)) {
			hw_error_status.current_jc &= ~((u64)(0xFFFFFFFF));
			hw_error_status.current_jc |= (u64)value;
		}
		if (addr == JOB_SLOT_REG(slot_idx, JS_HEAD_NEXT_HI)) {
			hw_error_status.current_jc &= (u64)0xFFFFFFFF;
			hw_error_status.current_jc |= ((u64)value) << 32;
		}
		if (addr == JOB_SLOT_REG(slot_idx, JS_COMMAND_NEXT) && value == 1) {
			pr_debug("%s", "start detected");
			KBASE_DEBUG_ASSERT(!dummy->slots[slot_idx].job_active ||
					   !dummy->slots[slot_idx].job_queued);
			if ((dummy->slots[slot_idx].job_active) ||
			    (hw_error_status.job_irq_rawstat & (1 << (slot_idx + 16)))) {
				pr_debug(
					"~~~~~~~~~~~ Start: job slot is already active or there are IRQ pending  ~~~~~~~~~");
				dummy->slots[slot_idx].job_queued = 1;
			} else {
				dummy->slots[slot_idx].job_active = 1;
			}
		}

		if (addr == JOB_SLOT_REG(slot_idx, JS_COMMAND_NEXT) && value == 0)
			dummy->slots[slot_idx].job_queued = 0;

		if ((addr == JOB_SLOT_REG(slot_idx, JS_COMMAND)) &&
		    (value == JS_COMMAND_SOFT_STOP || value == JS_COMMAND_HARD_STOP)) {
			/*dummy->slots[slot_idx].job_active = 0; */
			hw_error_status.current_job_slot = slot_idx;
			if (value == JS_COMMAND_SOFT_STOP) {
				hw_error_status.errors_mask = KBASE_JOB_STOPPED;
			} else { /*value == 3 */

				if (dummy->slots[slot_idx].job_disabled != 0) {
					pr_debug("enabling slot after HARD_STOP");
					dummy->slots[slot_idx].job_disabled = 0;
				}
				hw_error_status.errors_mask = KBASE_JOB_TERMINATED;
			}
		}
	} else if (addr == JOB_CONTROL_REG(JOB_IRQ_CLEAR)) {
		int i;

		for (i = 0; i < NUM_SLOTS; i++) {
			if (value & ((1u << i) | (1u << (i + 16))))
				dummy->slots[i].job_complete_irq_asserted = 0;
			/* hw_error_status.js_status[i] is cleared in
			 * update_job_irq_js_state
			 */
		}
		pr_debug("%s", "job irq cleared");
		update_job_irq_js_state(dummy, value);
		/*remove error condition for JOB */
		hw_error_status.job_irq_rawstat &= ~(value);
		hw_error_status.job_irq_status &= ~(value);
	} else if (addr == JOB_CONTROL_REG(JOB_IRQ_MASK)) {
		int i;

		for (i = 0; i < NUM_SLOTS; i++)
			dummy->slots[i].job_irq_mask = (value >> i) & 0x01;
		pr_debug("job irq mask to value %x", value);
#else /* MALI_USE_CSF */
	if (addr == JOB_CONTROL_REG(JOB_IRQ_CLEAR)) {
		pr_debug("%s", "job irq cleared");

		hw_error_status.job_irq_rawstat &= ~(value);
		hw_error_status.job_irq_status &= ~(value);
	} else if (addr == JOB_CONTROL_REG(JOB_IRQ_RAWSTAT)) {
		hw_error_status.job_irq_rawstat |= value;
		hw_error_status.job_irq_status |= value;
	} else if (addr == JOB_CONTROL_REG(JOB_IRQ_MASK)) {
		/* ignore JOB_IRQ_MASK as it is handled by CSFFW */
#endif /* !MALI_USE_CSF */
	} else if (addr == GPU_CONTROL_REG(GPU_IRQ_MASK)) {
		pr_debug("GPU_IRQ_MASK set to 0x%x", value);
#if MALI_USE_CSF
		if (!dummy->kbdev->pm.backend.has_host_pwr_iface) {
			dummy->reset_completed_mask = (value >> 8) & 0x01;
			dummy->power_changed_mask = (value >> 9) & 0x03;
		}
#else
		dummy->reset_completed_mask = (value >> 8) & 0x01;
		dummy->power_changed_mask = (value >> 9) & 0x03;
#endif
		dummy->clean_caches_completed_irq_enabled = (value & (1u << 17)) != 0u;
#if MALI_USE_CSF
		dummy->flush_pa_range_completed_irq_enabled = (value & (1u << 20)) != 0u;
#endif
	} else if (addr == GPU_CONTROL_REG(COHERENCY_ENABLE)) {
		dummy->coherency_enable = value;
#if MALI_USE_CSF
	} else if (addr == HOST_POWER_REG(PWR_IRQ_MASK)) {
		pr_debug("PWR_IRQ_MASK set to 0x%x", value);
		dummy->power_changed_mask = (value & PWR_IRQ_POWER_CHANGED_SINGLE) |
					    (value & PWR_IRQ_POWER_CHANGED_ALL);
		dummy->reset_completed_mask = !!(value & PWR_IRQ_RESET_COMPLETED);
		dummy->command_not_allowed_mask = PWR_IRQ_COMMAND_NOT_ALLOWED_GET(value);
		dummy->command_invalid_mask = PWR_IRQ_COMMAND_INVALID_GET(value);
	} else if (addr == HOST_POWER_REG(PWR_IRQ_CLEAR)) {
		if (value & PWR_IRQ_RESET_COMPLETED) {
			pr_debug("%s", "pwr RESET_COMPLETED irq cleared");
			dummy->reset_completed = 0;
		}
		if (value & (PWR_IRQ_POWER_CHANGED_SINGLE | PWR_IRQ_POWER_CHANGED_ALL))
			dummy->power_changed = 0;
		if (value & PWR_IRQ_COMMAND_NOT_ALLOWED_MASK)
			dummy->command_not_allowed = 0;
		if (value & PWR_IRQ_COMMAND_INVALID_MASK)
			dummy->command_invalid = 0;
	} else if (addr == HOST_POWER_REG(PWR_CMDARG_LO)) {
		dummy->command_arg = value | HI_MASK(dummy->command_arg);
	} else if (addr == HOST_POWER_REG(PWR_CMDARG_HI)) {
		dummy->command_arg = ((uint64_t)value << 32) | LO_MASK(dummy->command_arg);
	} else if (addr == HOST_POWER_REG(PWR_COMMAND)) {
		switch (PWR_COMMAND_COMMAND_GET(value)) {
		case PWR_COMMAND_COMMAND_RESET_FAST:
		case PWR_COMMAND_COMMAND_RESET_SOFT:
		case PWR_COMMAND_COMMAND_RESET_HARD:
			pr_debug("GPU reset (%d) requested", value);
			hw_error_status.gpu_fault_status = 0; /* no more fault status */
			dummy->reset_completed = 1; /* completed reset instantly */
			break;
		case PWR_COMMAND_COMMAND_POWER_UP:
			switch (PWR_COMMAND_DOMAIN_GET(value)) {
			case PWR_COMMAND_DOMAIN_L2:
				dummy->domain_power_on[INDEX_L2] |= dummy->command_arg &
								    DUMMY_IMPLEMENTATION_L2_PRESENT;
				dummy->power_changed = 1;
				break;
			case PWR_COMMAND_DOMAIN_TILER:
				dummy->domain_power_on[INDEX_TILER] |=
					dummy->command_arg & DUMMY_IMPLEMENTATION_TILER_PRESENT;
				dummy->power_changed = 1;
				break;
			case PWR_COMMAND_DOMAIN_SHADER:
				/**
				 * We are not concerned with the RTU as it is a subdomain and it's
				 * power status cannot be checked
				 */
				dummy->domain_power_on[INDEX_SHADER] |=
					dummy->command_arg &
					dummy->control_reg_values->shader_present;
				dummy->power_changed = 1;
				break;
			case PWR_COMMAND_DOMAIN_STACK:
				dummy->domain_power_on[INDEX_STACK] |=
					dummy->command_arg &
					dummy->control_reg_values->stack_present;
				dummy->power_changed = 1;
				break;
			case PWR_COMMAND_DOMAIN_BASE:
				dummy->domain_power_on[INDEX_BASE] |=
					dummy->command_arg &
					dummy->control_reg_values->base_present;
				dummy->power_changed = 1;
				break;
			case PWR_COMMAND_DOMAIN_NEURAL:
				dummy->domain_power_on[INDEX_NEURAL] |=
					dummy->command_arg &
					dummy->control_reg_values->neural_present;
				dummy->power_changed = 1;
				break;
			default:
				model_error_log(KBASE_CORE, "\n Domain(%x) is not supported",
						PWR_COMMAND_DOMAIN_GET(value));
				break;
			}
			break;
		case PWR_COMMAND_COMMAND_POWER_DOWN:
			switch (PWR_COMMAND_DOMAIN_GET(value)) {
			case PWR_COMMAND_DOMAIN_L2:
				dummy->domain_power_on[INDEX_L2] &=
					~(dummy->command_arg & DUMMY_IMPLEMENTATION_L2_PRESENT);
				dummy->power_changed = 1;
				break;
			case PWR_COMMAND_DOMAIN_TILER:
				dummy->domain_power_on[INDEX_TILER] &=
					~(dummy->command_arg & DUMMY_IMPLEMENTATION_TILER_PRESENT);
				dummy->power_changed = 1;
				break;
			case PWR_COMMAND_DOMAIN_SHADER:
				dummy->domain_power_on[INDEX_SHADER] &=
					~(dummy->command_arg &
					  dummy->control_reg_values->shader_present);
				dummy->power_changed = 1;
				break;
			case PWR_COMMAND_DOMAIN_STACK:
				dummy->domain_power_on[INDEX_STACK] &=
					~(dummy->command_arg &
					  dummy->control_reg_values->stack_present);
				dummy->power_changed = 1;
				break;
			case PWR_COMMAND_DOMAIN_BASE:
				dummy->domain_power_on[INDEX_BASE] &=
					~(dummy->command_arg &
					  dummy->control_reg_values->base_present);
				dummy->power_changed = 1;
				break;
			case PWR_COMMAND_DOMAIN_NEURAL:
				dummy->domain_power_on[INDEX_NEURAL] &=
					~(dummy->command_arg &
					  dummy->control_reg_values->neural_present);
				dummy->power_changed = 1;
				break;
			default:
				model_error_log(KBASE_CORE, "\n Domain(%x) is not supported",
						PWR_COMMAND_DOMAIN_GET(value));
				break;
			}
			break;
		case PWR_COMMAND_COMMAND_DELEGATE:
		case PWR_COMMAND_COMMAND_RETRACT:
		case PWR_COMMAND_COMMAND_INSPECT:
		default:
			model_error_log(KBASE_CORE, "\n Command(%x) is not supported",
					PWR_COMMAND_COMMAND_GET(value));
			break;
		}
	} else if (addr == GPU_CONTROL_REG(GPU_IRQ_CLEAR)) {
		if (!dummy->kbdev->pm.backend.has_host_pwr_iface) {
			if (value & RESET_COMPLETED) {
				pr_debug("%s", "gpu RESET_COMPLETED irq cleared");
				dummy->reset_completed = 0;
			}
			if (value & (POWER_CHANGED_SINGLE | POWER_CHANGED_ALL))
				dummy->power_changed = 0;
		}
#else
	} else if (addr == GPU_CONTROL_REG(GPU_IRQ_CLEAR)) {
		if (value & RESET_COMPLETED) {
			pr_debug("%s", "gpu RESET_COMPLETED irq cleared");
			dummy->reset_completed = 0;
		}
		if (value & (POWER_CHANGED_SINGLE | POWER_CHANGED_ALL))
			dummy->power_changed = 0;
#endif /* MALI_USE_CSF */

		if (value & CLEAN_CACHES_COMPLETED)
			dummy->clean_caches_completed = false;

#if MALI_USE_CSF
		if (value & (1u << 20))
			dummy->flush_pa_range_completed = false;
#endif /* MALI_USE_CSF */

#if !MALI_USE_CSF
		if (value & PRFCNT_SAMPLE_COMPLETED) /* (1 << 16) */
			dummy->prfcnt_sample_completed = 0;
#endif /* !MALI_USE_CSF */

		/*update error status */
		hw_error_status.gpu_error_irq &= ~(value);
	} else if (addr == GPU_CONTROL_REG(GPU_COMMAND)) {
		switch (value) {
		case GPU_COMMAND_SOFT_RESET:
		case GPU_COMMAND_HARD_RESET:
			pr_debug("gpu reset (%d) requested", value);
			/* no more fault status */
			hw_error_status.gpu_fault_status = 0;
			/* completed reset instantly */
			dummy->reset_completed = 1;
			break;
#if MALI_USE_CSF
		case GPU_COMMAND_CACHE_CLN_INV_L2:
		case GPU_COMMAND_CACHE_CLN_INV_L2_LSC:
		case GPU_COMMAND_CACHE_CLN_INV_FULL:
#else
		case GPU_COMMAND_CLEAN_CACHES:
		case GPU_COMMAND_CLEAN_INV_CACHES:
#endif
			pr_debug("clean caches requested");
			dummy->clean_caches_completed = true;
			break;
#if MALI_USE_CSF
		case GPU_COMMAND_FLUSH_PA_RANGE_CLN_INV_L2:
		case GPU_COMMAND_FLUSH_PA_RANGE_CLN_INV_L2_LSC:
		case GPU_COMMAND_FLUSH_PA_RANGE_CLN_INV_FULL:
			pr_debug("pa range flush requested");
			dummy->flush_pa_range_completed = true;
			break;
#endif /* MALI_USE_CSF */
#if !MALI_USE_CSF
		case GPU_COMMAND_PRFCNT_SAMPLE:
			midgard_model_dump_prfcnt();
			dummy->prfcnt_sample_completed = 1;
#endif /* !MALI_USE_CSF */
		default:
			break;
		}
#if MALI_USE_CSF
	} else if (addr >= GPU_CONTROL_REG(GPU_COMMAND_ARG0_LO) &&
		   addr <= GPU_CONTROL_REG(GPU_COMMAND_ARG1_HI)) {
		/* Writes ignored */
#endif
	} else if (addr == GPU_CONTROL_REG(L2_CONFIG)) {
		dummy->l2_config = value;
#if MALI_USE_CSF
	} else if (addr >= CSF_HW_DOORBELL_PAGE_OFFSET &&
		   addr < CSF_HW_DOORBELL_PAGE_OFFSET +
				   (dummy->kbdev->csf.num_doorbells * CSF_HW_DOORBELL_PAGE_SIZE)) {
		WARN_ON(!dummy->kbdev->csf.num_doorbells);
		if (addr == CSF_HW_DOORBELL_PAGE_OFFSET)
			hw_error_status.job_irq_status = JOB_IRQ_GLOBAL_IF;
	} else if ((addr >= GPU_CONTROL_REG(SYSC_ALLOC0)) &&
		   (addr < GPU_CONTROL_REG(SYSC_ALLOC(SYSC_ALLOC_COUNT)))) {
		u32 alloc_reg = (addr - GPU_CONTROL_REG(SYSC_ALLOC0)) >> 2;

		sysc_alloc_regs[alloc_reg] = value;
	} else if ((addr >= GPU_CONTROL_REG(L2_SLICE_HASH_0)) &&
		   (addr < GPU_CONTROL_REG(L2_SLICE_HASH(L2_SLICE_HASH_COUNT)))) {
		/* Do nothing */
	} else if (addr == IPA_CONTROL_REG(COMMAND) ||
		   addr == IPA_CONTROL_REG(COMMAND) + GPU_GOV_IPA_CONTROL_OFFSET) {
		pr_debug("Received IPA_CONTROL command");
	} else if (addr == IPA_CONTROL_REG(TIMER) ||
		   addr == IPA_CONTROL_REG(TIMER) + GPU_GOV_IPA_CONTROL_OFFSET) {
		ipa_control_timer_enabled = value ? 1U : 0U;
	} else if ((addr >= IPA_CONTROL_REG(SELECT_CSHW_LO)) &&
		   (addr <= IPA_CONTROL_REG(SELECT_SHADER_HI))) {
		enum kbase_ipa_core_type core_type =
			(enum kbase_ipa_core_type)((addr - IPA_CONTROL_REG(SELECT_CSHW_LO)) >> 3);
		bool is_low_word = !((addr - IPA_CONTROL_REG(SELECT_CSHW_LO)) & 7);

		if (is_low_word) {
			ipa_ctl_select_config[core_type] &= ~(u64)U32_MAX;
			ipa_ctl_select_config[core_type] |= value;
		} else {
			ipa_ctl_select_config[core_type] &= U32_MAX;
			ipa_ctl_select_config[core_type] |= ((u64)value << 32);
		}
	} else if ((addr >= IPA_CONTROL_REG(SELECT_CSHW_LO) + GPU_GOV_IPA_CONTROL_OFFSET) &&
		   (addr <= IPA_CONTROL_REG(SELECT_SHADER_HI) + GPU_GOV_IPA_CONTROL_OFFSET)) {
		enum kbase_ipa_core_type core_type = (enum kbase_ipa_core_type)(
			(addr - IPA_CONTROL_REG(SELECT_CSHW_LO) - GPU_GOV_IPA_CONTROL_OFFSET) >> 3);
		bool is_low_word = !(
			(addr - IPA_CONTROL_REG(SELECT_CSHW_LO) - GPU_GOV_IPA_CONTROL_OFFSET) & 7);

		if (is_low_word) {
			ipa_ctl_select_config[core_type] &= ~(u64)U32_MAX;
			ipa_ctl_select_config[core_type] |= value;
		} else {
			ipa_ctl_select_config[core_type] &= U32_MAX;
			ipa_ctl_select_config[core_type] |= ((u64)value << 32);
		}
	} else if (addr == GPU_GOV_CORE_MASK_OFFSET) {
		dummy->gov_core_mask = value;
#endif
	} else if (addr == MMU_CONTROL_REG(MMU_IRQ_MASK)) {
		hw_error_status.mmu_irq_mask = value;
	} else if (addr == MMU_CONTROL_REG(MMU_IRQ_CLEAR)) {
		hw_error_status.mmu_irq_rawstat &= (~value);
	} else if ((addr >= MMU_STAGE1_REG(MMU_AS_REG(0, AS_TRANSTAB_LO))) &&
		   (addr <= MMU_STAGE1_REG(MMU_AS_REG(15, AS_STATUS)))) {
		u32 mem_addr_space = (addr - MMU_STAGE1_REG(MMU_AS_REG(0, AS_TRANSTAB_LO))) >> 6;

		switch (addr & 0x3F) {
		case AS_COMMAND:
			switch (AS_COMMAND_COMMAND_GET(value)) {
			case AS_COMMAND_COMMAND_NOP:
				hw_error_status.as_command[mem_addr_space] = value;
				break;

			case AS_COMMAND_COMMAND_UPDATE:
				hw_error_status.as_command[mem_addr_space] = value;
				if ((hw_error_status.as_faultstatus[mem_addr_space]) &&
				    ((hw_error_status.as_transtab[mem_addr_space] & 0x3) != 0)) {
					model_error_log(
						KBASE_CORE,
						"\n ERROR: AS_COMMAND issued UPDATE on error condition before AS_TRANSTAB been set to unmapped\n");
				} else if ((hw_error_status.as_faultstatus[mem_addr_space]) &&
					   ((hw_error_status.as_transtab[mem_addr_space] & 0x3) ==
					    0)) {
					/*invalidate all active jobs */
					invalidate_active_jobs(dummy);
					/* error handled */
					hw_error_status.as_faultstatus[mem_addr_space] = 0;
				}
				break;

			case AS_COMMAND_COMMAND_LOCK:
			case AS_COMMAND_COMMAND_UNLOCK:
				hw_error_status.as_command[mem_addr_space] = value;
				break;

			case AS_COMMAND_COMMAND_FLUSH_PT:
			case AS_COMMAND_COMMAND_FLUSH_MEM:
				if (hw_error_status.as_command[mem_addr_space] !=
				    AS_COMMAND_COMMAND_LOCK)
					model_error_log(
						KBASE_CORE,
						"\n ERROR: AS_COMMAND issued FLUSH without LOCKING before\n");
				else /* error handled if any */
					hw_error_status.as_faultstatus[mem_addr_space] = 0;
				hw_error_status.as_command[mem_addr_space] = value;
				break;

			default:
				model_error_log(KBASE_CORE,
						"\n WARNING: UNRECOGNIZED AS_COMMAND 0x%x\n",
						value);
				break;
			}
			break;

		case AS_TRANSTAB_LO:
			hw_error_status.as_transtab[mem_addr_space] &= ~((u64)(0xffffffff));
			hw_error_status.as_transtab[mem_addr_space] |= (u64)value;
			break;

		case AS_TRANSTAB_HI:
			hw_error_status.as_transtab[mem_addr_space] &= (u64)0xffffffff;
			hw_error_status.as_transtab[mem_addr_space] |= ((u64)value) << 32;
			break;

		case AS_LOCKADDR_LO:
		case AS_LOCKADDR_HI:
		case AS_MEMATTR_LO:
		case AS_MEMATTR_HI:
		case AS_TRANSCFG_LO:
		case AS_TRANSCFG_HI:
			/* Writes ignored */
			break;

		default:
			model_error_log(
				KBASE_CORE,
				"Dummy model register access: Writing unsupported MMU #%d register 0x%x value 0x%x\n",
				mem_addr_space, addr, value);
			break;
		}
	} else {
		switch (addr) {
#if !MALI_USE_CSF
		case PRFCNT_BASE_LO:
			performance_counters.prfcnt_base =
				HI_MASK(performance_counters.prfcnt_base) | value;
			performance_counters.prfcnt_base_cpu =
				(u32 *)(uintptr_t)performance_counters.prfcnt_base;
			break;
		case PRFCNT_BASE_HI:
			performance_counters.prfcnt_base =
				LO_MASK(performance_counters.prfcnt_base) | (((u64)value) << 32);
			performance_counters.prfcnt_base_cpu =
				(u32 *)(uintptr_t)performance_counters.prfcnt_base;
			break;
		case PRFCNT_JM_EN:
			performance_counters.prfcnt_en.fe = value;
			break;
		case PRFCNT_SHADER_EN:
			performance_counters.prfcnt_en.shader = value;
			break;
		case PRFCNT_TILER_EN:
			performance_counters.prfcnt_en.tiler = value;
			break;
		case PRFCNT_MMU_L2_EN:
			performance_counters.prfcnt_en.l2 = value;
			break;
#endif /* !MALI_USE_CSF */
		case TILER_PWRON_LO:
			dummy->domain_power_on[INDEX_TILER] |= value &
							       DUMMY_IMPLEMENTATION_TILER_PRESENT;
			/* Also ensure L2 is powered on */
			fallthrough;
		case L2_PWRON_LO:
			dummy->domain_power_on[INDEX_L2] |= value & DUMMY_IMPLEMENTATION_L2_PRESENT;
			dummy->power_changed = 1;
			break;
		case SHADER_PWRON_LO:
			dummy->domain_power_on[INDEX_SHADER] |=
				value & dummy->control_reg_values->shader_present;
			dummy->power_changed = 1;
			break;
		case STACK_PWRON_LO:
			dummy->domain_power_on[INDEX_STACK] |=
				value & dummy->control_reg_values->stack_present;
			dummy->power_changed = 1;
			break;

		case L2_PWROFF_LO:
			dummy->domain_power_on[INDEX_L2] &=
				~(value & DUMMY_IMPLEMENTATION_L2_PRESENT);
			/* Also ensure tiler is powered off */
			fallthrough;
		case TILER_PWROFF_LO:
			dummy->domain_power_on[INDEX_TILER] &=
				~(value & DUMMY_IMPLEMENTATION_TILER_PRESENT);
			dummy->power_changed = 1;
			break;
		case SHADER_PWROFF_LO:
			dummy->domain_power_on[INDEX_SHADER] &=
				~(value & dummy->control_reg_values->shader_present);
			dummy->power_changed = 1;
			break;
		case STACK_PWROFF_LO:
			dummy->domain_power_on[INDEX_STACK] &=
				~(value & dummy->control_reg_values->stack_present);
			dummy->power_changed = 1;
			break;

		case TILER_PWRON_HI:
		case SHADER_PWRON_HI:
		case L2_PWRON_HI:
		case TILER_PWROFF_HI:
		case SHADER_PWROFF_HI:
		case L2_PWROFF_HI:
		case PWR_KEY:
		case PWR_OVERRIDE0:
		case PWR_OVERRIDE1:
#if MALI_USE_CSF
		case SHADER_PWRFEATURES:
		case CSF_CONFIG:
#else /* !MALI_USE_CSF */
		case JM_CONFIG:
		case PRFCNT_CONFIG:
#endif /* MALI_USE_CSF */
		case SHADER_CONFIG:
		case TILER_CONFIG:
		case L2_MMU_CONFIG:
			/* Writes ignored */
			break;
		default:
			model_error_log(
				KBASE_CORE,
				"Dummy model register access: Writing unsupported register 0x%x value 0x%x\n",
				addr, value);
			break;
		}
	}

	midgard_model_update(dummy);
	midgard_model_get_outputs(dummy);
	spin_unlock_irqrestore(&hw_error_status.access_lock, flags);
}

void midgard_model_read_reg(void *h, u32 addr, u32 *const value)
{
	unsigned long flags;
	struct dummy_model_t *dummy = (struct dummy_model_t *)h;

	spin_lock_irqsave(&hw_error_status.access_lock, flags);

	*value = 0; /* 0 by default */
#if !MALI_USE_CSF
	if (addr == JOB_CONTROL_REG(JOB_IRQ_JS_STATE)) {
		pr_debug("%s", "JS_ACTIVE being read");

		*value = dummy->job_irq_js_state;
	} else if (addr == GPU_CONTROL_REG(GPU_ID)) {
#else /* !MALI_USE_CSF */
	if (addr == GPU_CONTROL_REG(GPU_ID)) {
#endif /* !MALI_USE_CSF */
		*value = dummy->control_reg_values->gpu_id & U32_MAX;
	} else if (addr == JOB_CONTROL_REG(JOB_IRQ_RAWSTAT)) {
		*value = hw_error_status.job_irq_rawstat;
		pr_debug("%s", "JS_IRQ_RAWSTAT being read");
	} else if (addr == JOB_CONTROL_REG(JOB_IRQ_STATUS)) {
		*value = hw_error_status.job_irq_status;
		pr_debug("JS_IRQ_STATUS being read %x", *value);
#if !MALI_USE_CSF
	} else if (addr == JOB_CONTROL_REG(JOB_IRQ_MASK)) {
		int i;

		*value = 0;
		for (i = 0; i < NUM_SLOTS; i++)
			*value |= dummy->slots[i].job_irq_mask << i;
		pr_debug("JS_IRQ_MASK being read %x", *value);
#else /* !MALI_USE_CSF */
	} else if (addr == JOB_CONTROL_REG(JOB_IRQ_MASK)) {
		/* ignore JOB_IRQ_MASK as it is handled by CSFFW */
#endif /* !MALI_USE_CSF */
	} else if (addr == GPU_CONTROL_REG(GPU_IRQ_MASK)) {
		*value = (dummy->reset_completed_mask << 8) |
			 ((dummy->clean_caches_completed_irq_enabled ? 1u : 0u) << 17) |
#if MALI_USE_CSF
			 ((dummy->flush_pa_range_completed_irq_enabled ? 1u : 0u) << 20) |
#endif
			 (dummy->power_changed_mask << 9) | (1u << 7) | 1u;
		pr_debug("GPU_IRQ_MASK read %x", *value);
	} else if (addr == GPU_CONTROL_REG(GPU_IRQ_RAWSTAT)) {
		*value = ((dummy->clean_caches_completed ? 1u : 0u) << 17) |
#if MALI_USE_CSF
			 ((dummy->flush_pa_range_completed ? 1u : 0u) << 20) |
#else
			 (dummy->prfcnt_sample_completed ? PRFCNT_SAMPLE_COMPLETED : 0) |
#endif
			 hw_error_status.gpu_error_irq;
#if MALI_USE_CSF
		if (!dummy->kbdev->pm.backend.has_host_pwr_iface)
			*value |= (dummy->power_changed << 9) | (dummy->power_changed << 10) |
				  (dummy->reset_completed << 8);
#else
		*value |= (dummy->power_changed << 9) | (dummy->power_changed << 10) |
			  (dummy->reset_completed << 8);
#endif

		pr_debug("GPU_IRQ_RAWSTAT read %x", *value);
	} else if (addr == GPU_CONTROL_REG(GPU_IRQ_STATUS)) {
		*value = (((dummy->clean_caches_completed &&
			    dummy->clean_caches_completed_irq_enabled) ?
					 1u :
					 0u)
			  << 17) |
#if MALI_USE_CSF
			 (((dummy->flush_pa_range_completed &&
			    dummy->flush_pa_range_completed_irq_enabled) ?
					 1u :
					 0u)
			  << 20) |
#else
			 (dummy->prfcnt_sample_completed ? PRFCNT_SAMPLE_COMPLETED : 0) |
#endif
			 hw_error_status.gpu_error_irq;
#if MALI_USE_CSF
		if (!dummy->kbdev->pm.backend.has_host_pwr_iface)
			*value |=
				((dummy->power_changed && (dummy->power_changed_mask & 0x1)) << 9) |
				((dummy->power_changed && (dummy->power_changed_mask & 0x2))
				 << 10) |
				((dummy->reset_completed & dummy->reset_completed_mask) << 8);
#else
		*value |= ((dummy->power_changed && (dummy->power_changed_mask & 0x1)) << 9) |
			  ((dummy->power_changed && (dummy->power_changed_mask & 0x2)) << 10) |
			  ((dummy->reset_completed & dummy->reset_completed_mask) << 8);
#endif
		pr_debug("GPU_IRQ_STAT read %x", *value);
	} else if (addr == GPU_CONTROL_REG(GPU_STATUS)) {
		*value = 0;
#if !MALI_USE_CSF
	} else if (addr == GPU_CONTROL_REG(LATEST_FLUSH)) {
		*value = 0;
#endif
	} else if (addr == GPU_CONTROL_REG(GPU_FAULTSTATUS)) {
		*value = hw_error_status.gpu_fault_status;
	} else if (addr == GPU_CONTROL_REG(L2_CONFIG)) {
		*value = dummy->l2_config;
#if MALI_USE_CSF
	} else if ((addr >= GPU_CONTROL_REG(SYSC_ALLOC0)) &&
		   (addr < GPU_CONTROL_REG(SYSC_ALLOC(SYSC_ALLOC_COUNT)))) {
		u32 alloc_reg = (addr - GPU_CONTROL_REG(SYSC_ALLOC0)) >> 2;
		*value = sysc_alloc_regs[alloc_reg];
	} else if ((addr >= GPU_CONTROL_REG(L2_SLICE_HASH_0)) &&
		   (addr < GPU_CONTROL_REG(L2_SLICE_HASH(L2_SLICE_HASH_COUNT)))) {
		*value = 0;
	} else if (addr == HOST_POWER_REG(PWR_IRQ_RAWSTAT)) {
		*value = (dummy->power_changed << PWR_IRQ_POWER_CHANGED_SINGLE_SHIFT) |
			 (dummy->power_changed << PWR_IRQ_POWER_CHANGED_ALL_SHIFT) |
			 (dummy->reset_completed << PWR_IRQ_RESET_COMPLETED_SHIFT) |
			 (dummy->command_not_allowed << PWR_IRQ_COMMAND_NOT_ALLOWED_SHIFT) |
			 (dummy->command_invalid << PWR_IRQ_COMMAND_INVALID_SHIFT);
		pr_debug("PWR_IRQ_RAWSTAT read %x", *value);
	} else if (addr == HOST_POWER_REG(PWR_IRQ_STATUS)) {
		*value = ((dummy->power_changed &&
			   (dummy->power_changed_mask & PWR_IRQ_POWER_CHANGED_SINGLE))
			  << PWR_IRQ_POWER_CHANGED_SINGLE_SHIFT) |
			 ((dummy->power_changed &&
			   (dummy->power_changed_mask & PWR_IRQ_POWER_CHANGED_ALL))
			  << PWR_IRQ_POWER_CHANGED_ALL_SHIFT) |
			 ((dummy->reset_completed && dummy->reset_completed_mask)
			  << PWR_IRQ_RESET_COMPLETED_SHIFT) |
			 ((dummy->command_not_allowed && dummy->command_not_allowed_mask)
			  << PWR_IRQ_COMMAND_NOT_ALLOWED_SHIFT) |
			 ((dummy->command_invalid && dummy->command_invalid_mask)
			  << PWR_IRQ_COMMAND_INVALID_SHIFT);
		pr_debug("PWR_IRQ_STATUS read %x", *value);
	} else if (addr == HOST_POWER_REG(PWR_STATUS_LO)) {
		*value = PWR_STATUS_ALLOW_L2_MASK | PWR_STATUS_ALLOW_TILER_MASK |
			 PWR_STATUS_ALLOW_SHADER_MASK | PWR_STATUS_ALLOW_NEURAL_MASK |
			 PWR_STATUS_ALLOW_BASE_MASK | PWR_STATUS_ALLOW_STACK_MASK;
	} else if (addr == HOST_POWER_REG(PWR_STATUS_HI)) {
		*value = (PWR_STATUS_ALLOW_HARD_RESET_MASK | PWR_STATUS_ALLOW_SOFT_RESET_MASK) >>
			 PWR_STATUS_ALLOW_HARD_RESET_SHIFT;
	} else if (addr >= HOST_POWER_REG(HOST_POWER_L2_PRESENT_LO) &&
		   addr <= HOST_POWER_REG(HOST_POWER_STACK_PWRTRANS_HI)) {
		switch (addr) {
		case HOST_POWER_REG(HOST_POWER_SHADER_PRESENT_LO):
		case HOST_POWER_REG(HOST_POWER_TILER_PRESENT_LO):
		case HOST_POWER_REG(HOST_POWER_L2_PRESENT_LO):
		case HOST_POWER_REG(HOST_POWER_STACK_PRESENT_LO):
		case HOST_POWER_REG(HOST_POWER_NEURAL_PRESENT_LO):
		case HOST_POWER_REG(HOST_POWER_BASE_PRESENT_LO):
			*value = hctrl_get_implementation_register(addr, dummy->control_reg_values);
			break;

		case HOST_POWER_REG(HOST_POWER_L2_READY_LO):
			*value = dummy->domain_power_on[INDEX_L2] &
				 hctrl_get_implementation_register(
					 HOST_POWER_REG(HOST_POWER_L2_PRESENT_LO),
					 dummy->control_reg_values);
			break;
		case HOST_POWER_REG(HOST_POWER_TILER_READY_LO):
			*value = dummy->domain_power_on[INDEX_TILER] &
				 hctrl_get_implementation_register(
					 HOST_POWER_REG(HOST_POWER_TILER_PRESENT_LO),
					 dummy->control_reg_values);
			break;
		case HOST_POWER_REG(HOST_POWER_SHADER_READY_LO):
			*value = dummy->domain_power_on[INDEX_SHADER] &
				 hctrl_get_implementation_register(
					 HOST_POWER_REG(HOST_POWER_SHADER_PRESENT_LO),
					 dummy->control_reg_values);
			break;
		case HOST_POWER_REG(HOST_POWER_STACK_READY_LO):
			*value = dummy->domain_power_on[INDEX_STACK] &
				 hctrl_get_implementation_register(
					 HOST_POWER_REG(HOST_POWER_STACK_PRESENT_LO),
					 dummy->control_reg_values);
			break;
		case HOST_POWER_REG(HOST_POWER_BASE_READY_LO):
			*value = dummy->domain_power_on[INDEX_BASE] &
				 hctrl_get_implementation_register(
					 HOST_POWER_REG(HOST_POWER_BASE_PRESENT_LO),
					 dummy->control_reg_values);
			break;
		case HOST_POWER_REG(HOST_POWER_NEURAL_READY_LO):
			*value = dummy->domain_power_on[INDEX_NEURAL] &
				 hctrl_get_implementation_register(
					 HOST_POWER_REG(HOST_POWER_NEURAL_PRESENT_LO),
					 dummy->control_reg_values);
			break;

		case HOST_POWER_REG(HOST_POWER_L2_READY_HI):
		case HOST_POWER_REG(HOST_POWER_TILER_READY_HI):
		case HOST_POWER_REG(HOST_POWER_SHADER_READY_HI):
		case HOST_POWER_REG(HOST_POWER_STACK_READY_HI):
		case HOST_POWER_REG(HOST_POWER_BASE_READY_HI):
		case HOST_POWER_REG(HOST_POWER_NEURAL_READY_HI):

		case HOST_POWER_REG(HOST_POWER_SHADER_PRESENT_HI):
		case HOST_POWER_REG(HOST_POWER_TILER_PRESENT_HI):
		case HOST_POWER_REG(HOST_POWER_L2_PRESENT_HI):
		case HOST_POWER_REG(HOST_POWER_STACK_PRESENT_HI):
		case HOST_POWER_REG(HOST_POWER_NEURAL_PRESENT_HI):
		case HOST_POWER_REG(HOST_POWER_BASE_PRESENT_HI):

		case HOST_POWER_REG(HOST_POWER_L2_PWRTRANS_LO):
		case HOST_POWER_REG(HOST_POWER_L2_PWRTRANS_HI):
		case HOST_POWER_REG(HOST_POWER_TILER_PWRTRANS_LO):
		case HOST_POWER_REG(HOST_POWER_TILER_PWRTRANS_HI):
		case HOST_POWER_REG(HOST_POWER_SHADER_PWRTRANS_LO):
		case HOST_POWER_REG(HOST_POWER_SHADER_PWRTRANS_HI):
		case HOST_POWER_REG(HOST_POWER_STACK_PWRTRANS_LO):
		case HOST_POWER_REG(HOST_POWER_STACK_PWRTRANS_HI):
		case HOST_POWER_REG(HOST_POWER_BASE_PWRTRANS_LO):
		case HOST_POWER_REG(HOST_POWER_BASE_PWRTRANS_HI):
		case HOST_POWER_REG(HOST_POWER_NEURAL_PWRTRANS_LO):
		case HOST_POWER_REG(HOST_POWER_NEURAL_PWRTRANS_HI):

		case HOST_POWER_REG(HOST_POWER_L2_PWRACTIVE_LO):
		case HOST_POWER_REG(HOST_POWER_L2_PWRACTIVE_HI):
		case HOST_POWER_REG(HOST_POWER_TILER_PWRACTIVE_LO):
		case HOST_POWER_REG(HOST_POWER_TILER_PWRACTIVE_HI):
		case HOST_POWER_REG(HOST_POWER_SHADER_PWRACTIVE_LO):
		case HOST_POWER_REG(HOST_POWER_SHADER_PWRACTIVE_HI):
		case HOST_POWER_REG(HOST_POWER_BASE_PWRACTIVE_LO):
		case HOST_POWER_REG(HOST_POWER_BASE_PWRACTIVE_HI):
		case HOST_POWER_REG(HOST_POWER_NEURAL_PWRACTIVE_LO):
		case HOST_POWER_REG(HOST_POWER_NEURAL_PWRACTIVE_HI):
		case GPU_CONTROL_REG(THREAD_TLS_ALLOC):
			*value = 0;
			break;

		case GPU_CONTROL_REG(COHERENCY_FEATURES):
			*value = BIT(0) | BIT(1); /* ace_lite and ace, respectively. */
			break;
		case GPU_CONTROL_REG(COHERENCY_ENABLE):
			*value = dummy->coherency_enable;
			break;
		default:
			*value = 0;
			model_error_log(
				KBASE_CORE,
				"Dummy model register access: Reading unknown control reg 0x%x\n",
				addr);
			break;
		}
#endif
	} else if ((addr >= GPU_CONTROL_REG(SHADER_PRESENT_LO)) &&
		   (addr <= GPU_CONTROL_REG(L2_MMU_CONFIG))) {
		switch (addr) {
		case GPU_CONTROL_REG(SHADER_PRESENT_LO):
		case GPU_CONTROL_REG(SHADER_PRESENT_HI):
		case GPU_CONTROL_REG(TILER_PRESENT_LO):
		case GPU_CONTROL_REG(TILER_PRESENT_HI):
		case GPU_CONTROL_REG(L2_PRESENT_LO):
		case GPU_CONTROL_REG(L2_PRESENT_HI):
		case GPU_CONTROL_REG(STACK_PRESENT_LO):
		case GPU_CONTROL_REG(STACK_PRESENT_HI):
			*value = get_implementation_register(addr, dummy->control_reg_values);
			break;
		case GPU_CONTROL_REG(SHADER_READY_LO):
			*value = (dummy->domain_power_on[INDEX_SHADER]) &
				 get_implementation_register(GPU_CONTROL_REG(SHADER_PRESENT_LO),
							     dummy->control_reg_values);
			break;
		case GPU_CONTROL_REG(TILER_READY_LO):
			*value = (dummy->domain_power_on[INDEX_TILER]) &
				 get_implementation_register(GPU_CONTROL_REG(TILER_PRESENT_LO),
							     dummy->control_reg_values);
			break;
		case GPU_CONTROL_REG(L2_READY_LO):
			*value = dummy->domain_power_on[INDEX_L2] &
				 get_implementation_register(GPU_CONTROL_REG(L2_PRESENT_LO),
							     dummy->control_reg_values);
			break;
		case GPU_CONTROL_REG(STACK_READY_LO):
			*value = dummy->domain_power_on[INDEX_STACK] &
				 get_implementation_register(GPU_CONTROL_REG(STACK_PRESENT_LO),
							     dummy->control_reg_values);
			break;

		case GPU_CONTROL_REG(SHADER_READY_HI):
		case GPU_CONTROL_REG(TILER_READY_HI):
		case GPU_CONTROL_REG(L2_READY_HI):
		case GPU_CONTROL_REG(STACK_READY_HI):

		case GPU_CONTROL_REG(L2_PWRTRANS_LO):
		case GPU_CONTROL_REG(L2_PWRTRANS_HI):
		case GPU_CONTROL_REG(TILER_PWRTRANS_LO):
		case GPU_CONTROL_REG(TILER_PWRTRANS_HI):
		case GPU_CONTROL_REG(SHADER_PWRTRANS_LO):
		case GPU_CONTROL_REG(SHADER_PWRTRANS_HI):
		case GPU_CONTROL_REG(STACK_PWRTRANS_LO):
		case GPU_CONTROL_REG(STACK_PWRTRANS_HI):

		case GPU_CONTROL_REG(L2_PWRACTIVE_LO):
		case GPU_CONTROL_REG(L2_PWRACTIVE_HI):
		case GPU_CONTROL_REG(TILER_PWRACTIVE_LO):
		case GPU_CONTROL_REG(TILER_PWRACTIVE_HI):
		case GPU_CONTROL_REG(SHADER_PWRACTIVE_LO):
		case GPU_CONTROL_REG(SHADER_PWRACTIVE_HI):

#if MALI_USE_CSF
		case GPU_CONTROL_REG(SHADER_PWRFEATURES):
		case GPU_CONTROL_REG(CSF_CONFIG):
#else /* !MALI_USE_CSF */
		case GPU_CONTROL_REG(JM_CONFIG):
#endif /* MALI_USE_CSF */
		case GPU_CONTROL_REG(SHADER_CONFIG):
		case GPU_CONTROL_REG(TILER_CONFIG):
		case GPU_CONTROL_REG(L2_MMU_CONFIG):
		case GPU_CONTROL_REG(THREAD_TLS_ALLOC):
			*value = 0;
			break;

		case GPU_CONTROL_REG(COHERENCY_FEATURES):
			*value = BIT(0) | BIT(1); /* ace_lite and ace, respectively. */
			break;
		case GPU_CONTROL_REG(COHERENCY_ENABLE):
			*value = dummy->coherency_enable;
			break;

		default:
			model_error_log(
				KBASE_CORE,
				"Dummy model register access: Reading unknown control reg 0x%x\n",
				addr);
			break;
		}
#if !MALI_USE_CSF
	} else if ((addr >= JOB_CONTROL_REG(JOB_SLOT0)) &&
		   (addr < (JOB_CONTROL_REG(JOB_SLOT15) + 0x80))) {
		int slot_idx = (addr >> 7) & 0xf;
		int sub_reg = addr & 0x7F;

		KBASE_DEBUG_ASSERT(slot_idx < NUM_SLOTS);
		switch (sub_reg) {
		case JS_HEAD_NEXT_LO:
			*value = (u32)((hw_error_status.current_jc) & 0xFFFFFFFF);
			break;
		case JS_HEAD_NEXT_HI:
			*value = (u32)(hw_error_status.current_jc >> 32);
			break;
		case JS_STATUS:
			if (hw_error_status.js_status[slot_idx])
				*value = hw_error_status.js_status[slot_idx];
			else /* 0x08 means active, 0x00 idle */
				*value = (dummy->slots[slot_idx].job_active) << 3;
			break;
		case JS_COMMAND_NEXT:
			*value = dummy->slots[slot_idx].job_queued;
			break;

		/**
		 * The dummy model does not implement these registers
		 * avoid printing error messages
		 */
		case JS_HEAD_HI:
		case JS_HEAD_LO:
		case JS_TAIL_HI:
		case JS_TAIL_LO:
		case JS_FLUSH_ID_NEXT:
			break;

		default:
			model_error_log(
				KBASE_CORE,
				"Dummy model register access: unknown job slot reg 0x%02X being read\n",
				sub_reg);
			break;
		}
	} else if (addr == GPU_CONTROL_REG(JS_PRESENT)) {
		*value = 0x7;
#endif /* !MALI_USE_CSF */
	} else if (addr == GPU_CONTROL_REG(AS_PRESENT)) {
		*value = dummy->control_reg_values->as_present;
	} else if (addr >= GPU_CONTROL_REG(TEXTURE_FEATURES_0) &&
		   addr <= GPU_CONTROL_REG(TEXTURE_FEATURES_3)) {
		switch (addr) {
		case GPU_CONTROL_REG(TEXTURE_FEATURES_0):
			*value = 0xfffff;
			break;

		case GPU_CONTROL_REG(TEXTURE_FEATURES_1):
			*value = 0xffff;
			break;

		case GPU_CONTROL_REG(TEXTURE_FEATURES_2):
			*value = 0x9f81ffff;
			break;

		case GPU_CONTROL_REG(TEXTURE_FEATURES_3):
			*value = 0;
			break;
		}
#if !MALI_USE_CSF
	} else if (addr >= GPU_CONTROL_REG(JS0_FEATURES) &&
		   addr <= GPU_CONTROL_REG(JS15_FEATURES)) {
		switch (addr) {
		case GPU_CONTROL_REG(JS0_FEATURES):
			*value = 0x20e;
			break;

		case GPU_CONTROL_REG(JS1_FEATURES):
			*value = 0x1fe;
			break;

		case GPU_CONTROL_REG(JS2_FEATURES):
			*value = 0x7e;
			break;

		default:
			*value = 0;
			break;
		}
#endif /* !MALI_USE_CSF */
	} else if (addr >= GPU_CONTROL_REG(L2_FEATURES) && addr <= GPU_CONTROL_REG(MMU_FEATURES)) {
		switch (addr) {
		case GPU_CONTROL_REG(L2_FEATURES):
			*value = 0x6100206;
			break;

		case GPU_CONTROL_REG(CORE_FEATURES):
			*value = dummy->control_reg_values->core_features;
			break;

		case GPU_CONTROL_REG(TILER_FEATURES):
			*value = dummy->control_reg_values->tiler_features;
			break;

		case GPU_CONTROL_REG(MEM_FEATURES):
			/* Bit 0: Core group is coherent */
			*value = 0x01;
			/* Bits 11:8: L2 slice count - 1 */
			*value |= (hweight64(DUMMY_IMPLEMENTATION_L2_PRESENT) - 1) << 8;
			break;

		case GPU_CONTROL_REG(MMU_FEATURES):
			*value = dummy->control_reg_values->mmu_features;
			break;
		}
	} else if (addr >= GPU_CONTROL_REG(THREAD_MAX_THREADS) &&
		   addr <= GPU_CONTROL_REG(THREAD_FEATURES)) {
		switch (addr) {
		case GPU_CONTROL_REG(THREAD_FEATURES):
			*value = dummy->control_reg_values->thread_features |
				 (THREAD_FEATURES_IMPLEMENTATION_TECHNOLOGY_SOFTWARE << 30);
			break;
		case GPU_CONTROL_REG(THREAD_MAX_BARRIER_SIZE):
			*value = dummy->control_reg_values->thread_max_barrier_size;
			break;
		case GPU_CONTROL_REG(THREAD_MAX_WORKGROUP_SIZE):
			*value = dummy->control_reg_values->thread_max_workgroup_size;
			break;
		case GPU_CONTROL_REG(THREAD_MAX_THREADS):
			*value = dummy->control_reg_values->thread_max_threads;
			break;
		}
#if MALI_USE_CSF
#endif /* MALI_USE_CSF */
	} else if (addr >= GPU_CONTROL_REG(CYCLE_COUNT_LO) &&
		   addr <= GPU_CONTROL_REG(TIMESTAMP_HI)) {
		*value = 0;
	} else if (addr >= MMU_STAGE1_REG(MMU_AS_REG(0, AS_TRANSTAB_LO)) &&
		   addr <= MMU_STAGE1_REG(MMU_AS_REG(15, AS_STATUS))) {
		u32 mem_addr_space = (addr - MMU_STAGE1_REG(MMU_AS_REG(0, AS_TRANSTAB_LO))) >> 6;

		switch (addr & 0x3F) {
		case AS_TRANSTAB_LO:
			*value = (u32)(hw_error_status.as_transtab[mem_addr_space] & 0xffffffff);
			break;

		case AS_TRANSTAB_HI:
			*value = (u32)(hw_error_status.as_transtab[mem_addr_space] >> 32);
			break;

		case AS_STATUS:
			*value = 0;
			break;

		case AS_FAULTSTATUS:
			if (mem_addr_space == hw_error_status.faulty_mmu_as)
				*value = hw_error_status
						 .as_faultstatus[hw_error_status.faulty_mmu_as];
			else
				*value = 0;
			break;

		case AS_LOCKADDR_LO:
		case AS_LOCKADDR_HI:
		case AS_MEMATTR_LO:
		case AS_MEMATTR_HI:
		case AS_TRANSCFG_LO:
		case AS_TRANSCFG_HI:
			/* Read ignored */
			*value = 0;
			break;

		default:
			model_error_log(
				KBASE_CORE,
				"Dummy model register access: Reading unsupported MMU #%u register 0x%x. Returning 0\n",
				mem_addr_space, addr);
			*value = 0;
			break;
		}
	} else if (addr == MMU_CONTROL_REG(MMU_IRQ_MASK)) {
		*value = hw_error_status.mmu_irq_mask;
	} else if (addr == MMU_CONTROL_REG(MMU_IRQ_RAWSTAT)) {
		*value = hw_error_status.mmu_irq_rawstat;
	} else if (addr == MMU_CONTROL_REG(MMU_IRQ_STATUS)) {
		*value = hw_error_status.mmu_irq_mask & hw_error_status.mmu_irq_rawstat;
#if MALI_USE_CSF
	} else if (addr == IPA_CONTROL_REG(STATUS) ||
		   addr == IPA_CONTROL_REG(STATUS) + GPU_GOV_IPA_CONTROL_OFFSET) {
		*value = (ipa_control_timer_enabled << 31);
	} else if ((addr >= IPA_CONTROL_REG(VALUE_CSHW_REG_LO(0))) &&
		   (addr <= IPA_CONTROL_REG(VALUE_CSHW_REG_HI(IPA_CTL_MAX_VAL_CNT_IDX)))) {
		u32 counter_index = (addr - IPA_CONTROL_REG(VALUE_CSHW_REG_LO(0))) >> 3;
		bool is_low_word = !((addr - IPA_CONTROL_REG(VALUE_CSHW_REG_LO(0))) & 7);

		*value = gpu_model_get_prfcnt_value(KBASE_IPA_CORE_TYPE_CSHW, counter_index,
						    is_low_word);
	} else if ((addr >= IPA_CONTROL_REG(VALUE_MEMSYS_REG_LO(0))) &&
		   (addr <= IPA_CONTROL_REG(VALUE_MEMSYS_REG_HI(IPA_CTL_MAX_VAL_CNT_IDX)))) {
		u32 counter_index = (addr - IPA_CONTROL_REG(VALUE_MEMSYS_REG_LO(0))) >> 3;
		bool is_low_word = !((addr - IPA_CONTROL_REG(VALUE_MEMSYS_REG_LO(0))) & 7);

		*value = gpu_model_get_prfcnt_value(KBASE_IPA_CORE_TYPE_MEMSYS, counter_index,
						    is_low_word);
	} else if ((addr >= IPA_CONTROL_REG(VALUE_TILER_REG_LO(0))) &&
		   (addr <= IPA_CONTROL_REG(VALUE_TILER_REG_HI(IPA_CTL_MAX_VAL_CNT_IDX)))) {
		u32 counter_index = (addr - IPA_CONTROL_REG(VALUE_TILER_REG_LO(0))) >> 3;
		bool is_low_word = !((addr - IPA_CONTROL_REG(VALUE_TILER_REG_LO(0))) & 7);

		*value = gpu_model_get_prfcnt_value(KBASE_IPA_CORE_TYPE_TILER, counter_index,
						    is_low_word);
	} else if ((addr >= IPA_CONTROL_REG(VALUE_SHADER_REG_LO(0))) &&
		   (addr <= IPA_CONTROL_REG(VALUE_SHADER_REG_HI(IPA_CTL_MAX_VAL_CNT_IDX)))) {
		u32 counter_index = (addr - IPA_CONTROL_REG(VALUE_SHADER_REG_LO(0))) >> 3;
		bool is_low_word = !((addr - IPA_CONTROL_REG(VALUE_SHADER_REG_LO(0))) & 7);

		*value = gpu_model_get_prfcnt_value(KBASE_IPA_CORE_TYPE_SHADER, counter_index,
						    is_low_word);

	} else if ((addr >= IPA_CONTROL_REG(VALUE_CSHW_REG_LO(0)) + GPU_GOV_IPA_CONTROL_OFFSET) &&
		   (addr <= IPA_CONTROL_REG(VALUE_CSHW_REG_HI(IPA_CTL_MAX_VAL_CNT_IDX)) +
				    GPU_GOV_IPA_CONTROL_OFFSET)) {
		u32 counter_index = (addr - IPA_CONTROL_REG(VALUE_CSHW_REG_LO(0)) -
				     GPU_GOV_IPA_CONTROL_OFFSET) >>
				    3;
		bool is_low_word = !((addr - IPA_CONTROL_REG(VALUE_CSHW_REG_LO(0)) -
				      GPU_GOV_IPA_CONTROL_OFFSET) &
				     7);

		*value = gpu_model_get_prfcnt_value(KBASE_IPA_CORE_TYPE_CSHW, counter_index,
						    is_low_word);
	} else if ((addr >= IPA_CONTROL_REG(VALUE_MEMSYS_REG_LO(0)) + GPU_GOV_IPA_CONTROL_OFFSET) &&
		   (addr <= IPA_CONTROL_REG(VALUE_MEMSYS_REG_HI(IPA_CTL_MAX_VAL_CNT_IDX)) +
				    GPU_GOV_IPA_CONTROL_OFFSET)) {
		u32 counter_index = (addr - IPA_CONTROL_REG(VALUE_MEMSYS_REG_LO(0)) -
				     GPU_GOV_IPA_CONTROL_OFFSET) >>
				    3;
		bool is_low_word = !((addr - IPA_CONTROL_REG(VALUE_MEMSYS_REG_LO(0)) -
				      GPU_GOV_IPA_CONTROL_OFFSET) &
				     7);

		*value = gpu_model_get_prfcnt_value(KBASE_IPA_CORE_TYPE_MEMSYS, counter_index,
						    is_low_word);
	} else if ((addr >= IPA_CONTROL_REG(VALUE_TILER_REG_LO(0)) + GPU_GOV_IPA_CONTROL_OFFSET) &&
		   (addr <= IPA_CONTROL_REG(VALUE_TILER_REG_HI(IPA_CTL_MAX_VAL_CNT_IDX)) +
				    GPU_GOV_IPA_CONTROL_OFFSET)) {
		u32 counter_index = (addr - IPA_CONTROL_REG(VALUE_TILER_REG_LO(0)) -
				     GPU_GOV_IPA_CONTROL_OFFSET) >>
				    3;
		bool is_low_word = !((addr - IPA_CONTROL_REG(VALUE_TILER_REG_LO(0)) -
				      GPU_GOV_IPA_CONTROL_OFFSET) &
				     7);

		*value = gpu_model_get_prfcnt_value(KBASE_IPA_CORE_TYPE_TILER, counter_index,
						    is_low_word);
	} else if ((addr >= IPA_CONTROL_REG(VALUE_SHADER_REG_LO(0)) + GPU_GOV_IPA_CONTROL_OFFSET) &&
		   (addr <= IPA_CONTROL_REG(VALUE_SHADER_REG_HI(IPA_CTL_MAX_VAL_CNT_IDX)) +
				    GPU_GOV_IPA_CONTROL_OFFSET)) {
		u32 counter_index = (addr - IPA_CONTROL_REG(VALUE_SHADER_REG_LO(0)) -
				     GPU_GOV_IPA_CONTROL_OFFSET) >>
				    3;
		bool is_low_word = !((addr - IPA_CONTROL_REG(VALUE_SHADER_REG_LO(0)) -
				      GPU_GOV_IPA_CONTROL_OFFSET) &
				     7);

		*value = gpu_model_get_prfcnt_value(KBASE_IPA_CORE_TYPE_SHADER, counter_index,
						    is_low_word);
	} else if ((addr >= IPA_CONTROL_REG(VALUE_NEURAL_REG_LO(0)) + GPU_GOV_IPA_CONTROL_OFFSET) &&
		   (addr <= IPA_CONTROL_REG(VALUE_NEURAL_REG_HI(IPA_CTL_MAX_VAL_CNT_IDX)) +
				    GPU_GOV_IPA_CONTROL_OFFSET)) {
		u32 counter_index = (addr - IPA_CONTROL_REG(VALUE_NEURAL_REG_LO(0)) -
				     GPU_GOV_IPA_CONTROL_OFFSET) >>
				    3;
		bool is_low_word = !((addr - IPA_CONTROL_REG(VALUE_NEURAL_REG_LO(0)) -
				      GPU_GOV_IPA_CONTROL_OFFSET) &
				     7);

		*value = gpu_model_get_prfcnt_value(KBASE_IPA_CORE_TYPE_NEURAL, counter_index,
						    is_low_word);
#endif /* MALI_USE_CSF */
	} else if (addr == GPU_CONTROL_REG(GPU_FEATURES_LO)) {
		*value = dummy->control_reg_values->gpu_features_lo;
	} else if (addr == GPU_CONTROL_REG(GPU_FEATURES_HI)) {
		*value = dummy->control_reg_values->gpu_features_hi;
	}
	else {
		model_error_log(
			KBASE_CORE,
			"Dummy model register access: Reading unsupported register 0x%x. Returning 0\n",
			addr);
		*value = 0;
	}

	spin_unlock_irqrestore(&hw_error_status.access_lock, flags);
	CSTD_UNUSED(dummy);
}

static u32 set_user_sample_core_type(u64 *counters, u32 *usr_data_start, u32 usr_data_offset,
				     u32 usr_data_size, u32 core_count)
{
	u32 sample_size;
	u32 *usr_data = NULL;

	lockdep_assert_held(&performance_counters.access_lock);

	sample_size = core_count * KBASE_DUMMY_MODEL_COUNTER_PER_CORE * sizeof(u32);

	if ((usr_data_size >= usr_data_offset) && (sample_size <= usr_data_size - usr_data_offset))
		usr_data = usr_data_start + (usr_data_offset / sizeof(u32));

	if (!usr_data)
		model_error_log(KBASE_CORE, "Unable to set counter sample 1");
	else {
		u32 loop_cnt = core_count * KBASE_DUMMY_MODEL_COUNTER_PER_CORE;
		u32 i;

		for (i = 0; i < loop_cnt; i++) {
			counters[i] = usr_data[i];
		}
	}

	return usr_data_offset + sample_size;
}

static u32 set_kernel_sample_core_type(u64 *counters, u64 *usr_data_start, u32 usr_data_offset,
				       u32 usr_data_size, u32 core_count)
{
	u32 sample_size;
	u64 *usr_data = NULL;

	lockdep_assert_held(&performance_counters.access_lock);

	sample_size = core_count * KBASE_DUMMY_MODEL_COUNTER_PER_CORE * sizeof(u64);

	if ((usr_data_size >= usr_data_offset) && (sample_size <= usr_data_size - usr_data_offset))
		usr_data = usr_data_start + (usr_data_offset / sizeof(u64));

	if (!usr_data)
		model_error_log(KBASE_CORE, "Unable to set kernel counter sample 1");
	else
		memcpy(counters, usr_data, sample_size);

	return usr_data_offset + sample_size;
}

/* Counter values injected through ioctl are of 32 bits */
int gpu_model_set_dummy_prfcnt_user_sample(u32 __user *data, u32 size)
{
	unsigned long flags;
	u32 *user_data;
	u32 offset = 0;

	if (data == NULL || size == 0 || size > KBASE_DUMMY_MODEL_COUNTER_TOTAL * sizeof(u32))
		return -EINVAL;

	/* copy_from_user might sleep so can't be called from inside a spinlock
	 * allocate a temporary buffer for user data and copy to that before taking
	 * the lock
	 */
	user_data = kmalloc(size, GFP_KERNEL);
	if (!user_data)
		return -ENOMEM;

	if (copy_from_user(user_data, data, size)) {
		model_error_log(KBASE_CORE, "Unable to copy prfcnt data from userspace");
		kfree(user_data);
		return -EINVAL;
	}

	spin_lock_irqsave(&performance_counters.access_lock, flags);
#if !MALI_USE_CSF
	offset = set_user_sample_core_type(performance_counters.jm_counters, user_data, offset,
					   size, 1);
#else
	offset = set_user_sample_core_type(performance_counters.cshw_counters, user_data, offset,
					   size, 1);
#endif /* !MALI_USE_CSF */
	offset = set_user_sample_core_type(performance_counters.tiler_counters, user_data, offset,
					   size, hweight64(DUMMY_IMPLEMENTATION_TILER_PRESENT));
	offset = set_user_sample_core_type(performance_counters.l2_counters, user_data, offset,
					   size, KBASE_DUMMY_MODEL_MAX_MEMSYS_BLOCKS);
	offset = set_user_sample_core_type(performance_counters.shader_counters, user_data, offset,
					   size, KBASE_DUMMY_MODEL_MAX_SHADER_CORES);
	spin_unlock_irqrestore(&performance_counters.access_lock, flags);

	kfree(user_data);
	return 0;
}

/* Counter values injected through kutf are of 64 bits */
void gpu_model_set_dummy_prfcnt_kernel_sample(u64 *data, u32 size)
{
	unsigned long flags;
	u32 offset = 0;

	spin_lock_irqsave(&performance_counters.access_lock, flags);
#if !MALI_USE_CSF
	offset = set_kernel_sample_core_type(performance_counters.jm_counters, data, offset, size,
					     1);
#else
	offset = set_kernel_sample_core_type(performance_counters.cshw_counters, data, offset, size,
					     1);
#endif /* !MALI_USE_CSF */
	offset = set_kernel_sample_core_type(performance_counters.tiler_counters, data, offset,
					     size, hweight64(DUMMY_IMPLEMENTATION_TILER_PRESENT));
	offset = set_kernel_sample_core_type(performance_counters.l2_counters, data, offset, size,
					     hweight64(performance_counters.l2_present));
	offset = set_kernel_sample_core_type(performance_counters.shader_counters, data, offset,
					     size, hweight64(performance_counters.shader_present));
	spin_unlock_irqrestore(&performance_counters.access_lock, flags);
}
KBASE_EXPORT_TEST_API(gpu_model_set_dummy_prfcnt_kernel_sample);

void gpu_model_get_dummy_prfcnt_cores(struct kbase_device *kbdev, u64 *l2_present,
				      u64 *shader_present)
{
	if (shader_present)
		*shader_present = performance_counters.shader_present;
	if (l2_present)
		*l2_present = performance_counters.l2_present;
}
KBASE_EXPORT_TEST_API(gpu_model_get_dummy_prfcnt_cores);

void gpu_model_set_dummy_prfcnt_cores(struct kbase_device *kbdev, u64 l2_present,
				      u64 shader_present)
{
	if (WARN_ON(!l2_present || !shader_present ||
		    hweight64(l2_present) > KBASE_DUMMY_MODEL_MAX_MEMSYS_BLOCKS ||
		    hweight64(shader_present) > KBASE_DUMMY_MODEL_MAX_SHADER_CORES))
		return;

	performance_counters.l2_present = l2_present;
	performance_counters.shader_present = shader_present;

	/* Update the GPU properties used by vinstr to calculate the counter
	 * dump buffer size.
	 */
	kbdev->gpu_props.num_l2_slices = hweight64(l2_present);
	kbdev->gpu_props.coherency_info.group.core_mask = shader_present;
	kbdev->gpu_props.curr_config.l2_slices = hweight64(l2_present);
	kbdev->gpu_props.curr_config.shader_present = shader_present;
}
KBASE_EXPORT_TEST_API(gpu_model_set_dummy_prfcnt_cores);

int gpu_model_control(void *model, struct kbase_model_control_params *params)
{
	struct dummy_model_t *dummy = (struct dummy_model_t *)model;
	int i;
	unsigned long flags;

	if (params->command == KBASE_MC_DISABLE_JOBS) {
		for (i = 0; i < NUM_SLOTS; i++)
			dummy->slots[i].job_disabled = params->value;
	} else {
		return -EINVAL;
	}

	spin_lock_irqsave(&hw_error_status.access_lock, flags);
	midgard_model_update(dummy);
	midgard_model_get_outputs(dummy);
	spin_unlock_irqrestore(&hw_error_status.access_lock, flags);

	return 0;
}

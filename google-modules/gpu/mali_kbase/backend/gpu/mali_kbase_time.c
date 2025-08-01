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

#include <mali_kbase.h>
#include <mali_kbase_hwaccess_time.h>
#if MALI_USE_CSF
#include <linux/gcd.h>
#include <csf/mali_kbase_csf_timeout.h>
#endif
#include <device/mali_kbase_device.h>
#include <backend/gpu/mali_kbase_pm_internal.h>
#include <mali_kbase_config_defaults.h>
#include <mali_kbase_io.h>
#include <linux/version_compat_defs.h>
#include <asm/arch_timer.h>
#include <linux/mali_hw_access.h>

struct kbase_timeout_info {
	char *selector_str;
	u64 timeout_cycles;
};

#if MALI_USE_CSF

#define GPU_TIMESTAMP_OFFSET_INVALID S64_MAX

static struct kbase_timeout_info timeout_info[KBASE_TIMEOUT_SELECTOR_COUNT] = {
	[CSF_FIRMWARE_TIMEOUT] = { "CSF_FIRMWARE_TIMEOUT", MIN(CSF_FIRMWARE_TIMEOUT_CYCLES,
							       CSF_FIRMWARE_PING_TIMEOUT_CYCLES) },
	[CSF_PM_TIMEOUT] = { "CSF_PM_TIMEOUT", CSF_PM_TIMEOUT_CYCLES },
	[CSF_GPU_RESET_TIMEOUT] = { "CSF_GPU_RESET_TIMEOUT", CSF_GPU_RESET_TIMEOUT_CYCLES },
	[CSF_CSG_TERM_TIMEOUT] = { "CSF_CSG_TERM_TIMEOUT", CSF_CSG_TERM_TIMEOUT_CYCLES },
	[CSF_FIRMWARE_BOOT_TIMEOUT] = { "CSF_FIRMWARE_BOOT_TIMEOUT",
					CSF_FIRMWARE_BOOT_TIMEOUT_CYCLES },
	[CSF_FIRMWARE_WAKE_UP_TIMEOUT] = { "CSF_FIRMWARE_WAKE_UP_TIMEOUT",
					   CSF_FIRMWARE_WAKE_UP_TIMEOUT_CYCLES },
	[CSF_FIRMWARE_SOI_HALT_TIMEOUT] = { "CSF_FIRMWARE_SOI_HALT_TIMEOUT",
					    CSF_FIRMWARE_SOI_HALT_TIMEOUT_CYCLES },
	[CSF_FIRMWARE_PING_TIMEOUT] = { "CSF_FIRMWARE_PING_TIMEOUT",
					CSF_FIRMWARE_PING_TIMEOUT_CYCLES },
	[CSF_SCHED_PROTM_PROGRESS_TIMEOUT] = { "CSF_SCHED_PROTM_PROGRESS_TIMEOUT",
					       DEFAULT_PROGRESS_TIMEOUT_CYCLES },
	[MMU_AS_INACTIVE_WAIT_TIMEOUT] = { "MMU_AS_INACTIVE_WAIT_TIMEOUT",
					   MMU_AS_INACTIVE_WAIT_TIMEOUT_CYCLES },
	[KCPU_FENCE_SIGNAL_TIMEOUT] = { "KCPU_FENCE_SIGNAL_TIMEOUT",
					KCPU_FENCE_SIGNAL_TIMEOUT_CYCLES },
	[KBASE_PRFCNT_ACTIVE_TIMEOUT] = { "KBASE_PRFCNT_ACTIVE_TIMEOUT",
					  KBASE_PRFCNT_ACTIVE_TIMEOUT_CYCLES },
	[KBASE_CLEAN_CACHE_TIMEOUT] = { "KBASE_CLEAN_CACHE_TIMEOUT",
					KBASE_CLEAN_CACHE_TIMEOUT_CYCLES },
	[KBASE_AS_INACTIVE_TIMEOUT] = { "KBASE_AS_INACTIVE_TIMEOUT",
					KBASE_AS_INACTIVE_TIMEOUT_CYCLES },
	[IPA_INACTIVE_TIMEOUT] = { "IPA_INACTIVE_TIMEOUT", IPA_INACTIVE_TIMEOUT_CYCLES },
	[CSF_FIRMWARE_STOP_TIMEOUT] = { "CSF_FIRMWARE_STOP_TIMEOUT",
					CSF_FIRMWARE_STOP_TIMEOUT_CYCLES },
	[CSF_PWR_DELEGATE_TIMEOUT] = { "CSF_PWR_DELEGATE_TIMEOUT",
				       CSF_PWR_DELEGATE_TIMEOUT_CYCLES },
	[CSF_PWR_INSPECT_TIMEOUT] = { "CSF_PWR_INSPECT_TIMEOUT", CSF_PWR_INSPECT_TIMEOUT_CYCLES },
	[CSF_GPU_SUSPEND_TIMEOUT] = { "CSF_GPU_SUSPEND_TIMEOUT", CSF_GPU_SUSPEND_TIMEOUT_CYCLES },
};
#else
static struct kbase_timeout_info timeout_info[KBASE_TIMEOUT_SELECTOR_COUNT] = {
	[MMU_AS_INACTIVE_WAIT_TIMEOUT] = { "MMU_AS_INACTIVE_WAIT_TIMEOUT",
					   MMU_AS_INACTIVE_WAIT_TIMEOUT_CYCLES },
	[JM_DEFAULT_JS_FREE_TIMEOUT] = { "JM_DEFAULT_JS_FREE_TIMEOUT",
					 JM_DEFAULT_JS_FREE_TIMEOUT_CYCLES },
	[KBASE_PRFCNT_ACTIVE_TIMEOUT] = { "KBASE_PRFCNT_ACTIVE_TIMEOUT",
					  KBASE_PRFCNT_ACTIVE_TIMEOUT_CYCLES },
	[KBASE_CLEAN_CACHE_TIMEOUT] = { "KBASE_CLEAN_CACHE_TIMEOUT",
					KBASE_CLEAN_CACHE_TIMEOUT_CYCLES },
	[KBASE_AS_INACTIVE_TIMEOUT] = { "KBASE_AS_INACTIVE_TIMEOUT",
					KBASE_AS_INACTIVE_TIMEOUT_CYCLES },
};
#endif

#if MALI_USE_CSF
void kbase_backend_invalidate_gpu_timestamp_offset(struct kbase_device *kbdev)
{
	kbdev->backend_time.gpu_timestamp_offset = GPU_TIMESTAMP_OFFSET_INVALID;
}
KBASE_EXPORT_TEST_API(kbase_backend_invalidate_gpu_timestamp_offset);

/**
 * kbase_backend_compute_gpu_ts_offset() - Compute GPU TS offset.
 *
 * @kbdev:	Kbase device.
 *
 * This function compute the value of GPU and CPU TS offset:
 *   - set to zero current TIMESTAMP_OFFSET register
 *   - read CPU TS and convert it to ticks
 *   - read GPU TS
 *   - calculate diff between CPU and GPU ticks
 *   - cache the diff as the GPU TS offset
 *
 * To reduce delays, preemption must be disabled during reads of both CPU and GPU TS
 * this function require access to GPU register to be enabled
 */
static inline void kbase_backend_compute_gpu_ts_offset(struct kbase_device *kbdev)
{
	s64 cpu_ts_ticks = 0;
	s64 gpu_ts_ticks = 0;

	if (kbdev->backend_time.gpu_timestamp_offset != GPU_TIMESTAMP_OFFSET_INVALID)
		return;

	kbase_reg_write64(kbdev, GPU_CONTROL_ENUM(TIMESTAMP_OFFSET), 0);

	gpu_ts_ticks = kbase_reg_read64_coherent(kbdev, GPU_CONTROL_ENUM(TIMESTAMP));
	cpu_ts_ticks = ktime_get_raw_ns();
	cpu_ts_ticks = div64_u64(cpu_ts_ticks * kbdev->backend_time.divisor,
				 kbdev->backend_time.multiplier);
	kbdev->backend_time.gpu_timestamp_offset = cpu_ts_ticks - gpu_ts_ticks;
}

void kbase_backend_update_gpu_timestamp_offset(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->pm.lock);

	kbase_backend_compute_gpu_ts_offset(kbdev);

	dev_dbg(kbdev->dev, "Setting GPU timestamp offset register to %lld (%lld ns)",
		kbdev->backend_time.gpu_timestamp_offset,
		div64_s64(kbdev->backend_time.gpu_timestamp_offset *
				  (s64)kbdev->backend_time.multiplier,
			  (s64)kbdev->backend_time.divisor));
	kbase_reg_write64(kbdev, GPU_CONTROL_ENUM(TIMESTAMP_OFFSET),
			  kbdev->backend_time.gpu_timestamp_offset);
}
#if MALI_UNIT_TEST
u64 kbase_backend_read_gpu_timestamp_offset_reg(struct kbase_device *kbdev)
{
	return kbase_reg_read64_coherent(kbdev, GPU_CONTROL_ENUM(TIMESTAMP_OFFSET));
}
KBASE_EXPORT_TEST_API(kbase_backend_read_gpu_timestamp_offset_reg);
#endif
#endif

void kbase_backend_get_gpu_time_norequest(struct kbase_device *kbdev, u64 *cycle_counter,
					  u64 *system_time, struct timespec64 *ts)
{
	if (cycle_counter)
		*cycle_counter = kbase_backend_get_cycle_cnt(kbdev);

	if (system_time) {
		*system_time = kbase_reg_read64_coherent(kbdev, GPU_CONTROL_ENUM(TIMESTAMP));
	}

	/* Record the CPU's idea of current time */
	if (ts != NULL)
#if (KERNEL_VERSION(4, 17, 0) > LINUX_VERSION_CODE)
		*ts = ktime_to_timespec64(ktime_get_raw());
#else
		ktime_get_raw_ts64(ts);
#endif
}
KBASE_EXPORT_TEST_API(kbase_backend_get_gpu_time_norequest);

#if !MALI_USE_CSF
/**
 * timedwait_cycle_count_active() - Timed wait till CYCLE_COUNT_ACTIVE is active
 *
 * @kbdev: Kbase device
 *
 * Return: 0 if CYCLE_COUNT_ACTIVE is active within the timeout,
 *         otherwise a negative error code.
 */
static int timedwait_cycle_count_active(struct kbase_device *kbdev)
{
#if IS_ENABLED(CONFIG_MALI_NO_MALI)
	return 0;
#else
	const unsigned int timeout = 100;
	const unsigned long remaining = jiffies + msecs_to_jiffies(timeout);

	while (time_is_after_jiffies(remaining)) {
		if (kbase_io_is_gpu_lost(kbdev))
			return -ENODEV;

		if ((kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(GPU_STATUS)) &
		     GPU_STATUS_CYCLE_COUNT_ACTIVE))
			return 0;
	}

	return -ETIMEDOUT;
#endif
}
#endif

void kbase_backend_get_gpu_time(struct kbase_device *kbdev, u64 *cycle_counter, u64 *system_time,
				struct timespec64 *ts)
{
#if !MALI_USE_CSF
	kbase_pm_request_gpu_cycle_counter(kbdev);
	if (!kbase_io_is_gpu_lost(kbdev))
		WARN_ONCE(kbdev->pm.backend.l2_state != KBASE_L2_ON, "L2 not powered up");

	WARN_ONCE(timedwait_cycle_count_active(kbdev) == -ETIMEDOUT,
		  "Timed out on CYCLE_COUNT_ACTIVE");
#endif
	kbase_backend_get_gpu_time_norequest(kbdev, cycle_counter, system_time, ts);
#if !MALI_USE_CSF
	kbase_pm_release_gpu_cycle_counter(kbdev);
#endif
}
KBASE_EXPORT_TEST_API(kbase_backend_get_gpu_time);

static u64 kbase_device_get_scaling_frequency(struct kbase_device *kbdev)
{
	u64 freq_khz = kbdev->lowest_gpu_freq_khz;

	if (!freq_khz) {
		dev_dbg(kbdev->dev,
			"Lowest frequency uninitialized! Using reference frequency for scaling");
		return DEFAULT_REF_TIMEOUT_FREQ_KHZ;
	}

	return freq_khz;
}

void kbase_device_set_timeout_ms(struct kbase_device *kbdev, enum kbase_timeout_selector selector,
				 unsigned int timeout_ms)
{
	char *selector_str;

	if (unlikely(selector >= KBASE_TIMEOUT_SELECTOR_COUNT)) {
		selector = KBASE_DEFAULT_TIMEOUT;
		selector_str = timeout_info[selector].selector_str;
		dev_warn(kbdev->dev,
			 "Unknown timeout selector passed, falling back to default: %s\n",
			 timeout_info[selector].selector_str);
	}
	selector_str = timeout_info[selector].selector_str;

#if MALI_USE_CSF
	if ((kbdev->gpu_props.impl_tech <= THREAD_FEATURES_IMPLEMENTATION_TECHNOLOGY_SILICON) &&
	    unlikely(timeout_ms >= MAX_TIMEOUT_MS)) {
		/* [PIXEL-MOD] - Most default timeouts at 100MHz hit this warning.
		dev_warn(kbdev->dev, "%s is capped from %dms to %dms\n",
			 timeout_info[selector].selector_str, timeout_ms, MAX_TIMEOUT_MS);
		*/
		timeout_ms = MAX_TIMEOUT_MS;
	}
#endif

	kbdev->backend_time.device_scaled_timeouts[selector] = timeout_ms;
	dev_dbg(kbdev->dev, "\t%-35s: %ums\n", selector_str, timeout_ms);
}

void kbase_device_set_timeout(struct kbase_device *kbdev, enum kbase_timeout_selector selector,
			      u64 timeout_cycles, u32 cycle_multiplier)
{
	u64 final_cycles;
	u64 timeout;
	u64 freq_khz = kbase_device_get_scaling_frequency(kbdev);

	if (unlikely(selector >= KBASE_TIMEOUT_SELECTOR_COUNT)) {
		selector = KBASE_DEFAULT_TIMEOUT;
		dev_warn(kbdev->dev,
			 "Unknown timeout selector passed, falling back to default: %s\n",
			 timeout_info[selector].selector_str);
	}

	/* If the multiplication overflows, we will have unsigned wrap-around, and so might
	 * end up with a shorter timeout. In those cases, we then want to have the largest
	 * timeout possible that will not run into these issues. Note that this will not
	 * wait for U64_MAX/frequency ms, as it will be clamped to a max of UINT_MAX
	 * milliseconds by subsequent steps.
	 */
	if (check_mul_overflow(timeout_cycles, (u64)cycle_multiplier, &final_cycles))
		final_cycles = U64_MAX;

	/* Timeout calculation:
	 * dividing number of cycles by freq in KHz automatically gives value
	 * in milliseconds. nr_cycles will have to be multiplied by 1e3 to
	 * get result in microseconds, and 1e6 to get result in nanoseconds.
	 */
	timeout = div_u64(final_cycles, freq_khz);

	if (unlikely(timeout > UINT_MAX)) {
		dev_dbg(kbdev->dev,
			"Capping excessive timeout %llums for %s at freq %llukHz to UINT_MAX ms",
			timeout, timeout_info[selector].selector_str,
			kbase_device_get_scaling_frequency(kbdev));
		timeout = UINT_MAX;
	}

	kbase_device_set_timeout_ms(kbdev, selector, (unsigned int)timeout);
}

/**
 * kbase_timeout_scaling_init - Initialize the table of scaled timeout
 *                              values associated with a @kbase_device.
 *
 * @kbdev:	KBase device pointer.
 *
 * Return: 0 on success, negative error code otherwise.
 */
static int kbase_timeout_scaling_init(struct kbase_device *kbdev)
{
	int err;
	enum kbase_timeout_selector selector;

	/* First, we initialize the minimum and maximum device frequencies, which
	 * are used to compute the timeouts.
	 */
	err = kbase_pm_gpu_freq_init(kbdev);
	if (unlikely(err < 0)) {
		dev_dbg(kbdev->dev, "Could not initialize GPU frequency\n");
		return err;
	}

	dev_dbg(kbdev->dev, "Scaling kbase timeouts:\n");
	for (selector = 0; selector < KBASE_TIMEOUT_SELECTOR_COUNT; selector++) {
		u32 cycle_multiplier = 1;
		u64 nr_cycles = timeout_info[selector].timeout_cycles;
#if MALI_USE_CSF
		/* Special case: the scheduler progress timeout can be set manually,
		 * and does not have a canonical length defined in the headers. Hence,
		 * we query it once upon startup to get a baseline, and change it upon
		 * every invocation of the appropriate functions
		 */
		if (selector == CSF_SCHED_PROTM_PROGRESS_TIMEOUT)
			nr_cycles = kbase_csf_timeout_get(kbdev);

		if (selector == KCPU_FENCE_SIGNAL_TIMEOUT) {
			if ((kbdev->gpu_props.impl_tech ==
			     THREAD_FEATURES_IMPLEMENTATION_TECHNOLOGY_FPGA) ||
			    (kbdev->gpu_props.impl_tech ==
			     THREAD_FEATURES_IMPLEMENTATION_TECHNOLOGY_SOFTWARE)) {
				nr_cycles = KCPU_FENCE_SIGNAL_TIMEOUT_CYCLES_FPGA;
			}
		}
#endif

		/* Since we are in control of the iteration bounds for the selector,
		 * we don't have to worry about bounds checking when setting the timeout.
		 */
		kbase_device_set_timeout(kbdev, selector, nr_cycles, cycle_multiplier);
	}
	return 0;
}

unsigned int kbase_get_timeout_ms(struct kbase_device *kbdev, enum kbase_timeout_selector selector)
{
	if (unlikely(selector >= KBASE_TIMEOUT_SELECTOR_COUNT)) {
		dev_warn(kbdev->dev, "Querying wrong selector, falling back to default\n");
		selector = KBASE_DEFAULT_TIMEOUT;
	}

	return kbdev->backend_time.device_scaled_timeouts[selector];
}
KBASE_EXPORT_TEST_API(kbase_get_timeout_ms);

u64 kbase_backend_get_cycle_cnt(struct kbase_device *kbdev)
{
	return kbase_reg_read64_coherent(kbdev, GPU_CONTROL_ENUM(CYCLE_COUNT));
}

#if MALI_USE_CSF
u64 __maybe_unused kbase_backend_time_convert_gpu_to_cpu(struct kbase_device *kbdev, u64 gpu_ts)
{
	if (WARN_ON(!kbdev))
		return 0;

	return div64_u64(gpu_ts * kbdev->backend_time.multiplier, kbdev->backend_time.divisor);
}
KBASE_EXPORT_TEST_API(kbase_backend_time_convert_gpu_to_cpu);
#endif

u64 kbase_arch_timer_get_cntfrq(struct kbase_device *kbdev)
{
	u64 freq = mali_arch_timer_get_cntfrq();

	dev_dbg(kbdev->dev, "System Timer Freq = %lluHz", freq);

	return freq;
}

int kbase_backend_time_init(struct kbase_device *kbdev)
{
	int err = 0;
#if MALI_USE_CSF
	u64 freq;
	u64 common_factor;

	kbase_pm_register_access_enable(kbdev);
	freq = kbase_arch_timer_get_cntfrq(kbdev);

	if (!freq) {
		dev_warn(kbdev->dev, "arch_timer_get_rate() is zero!");
		err = -EINVAL;
		goto disable_registers;
	}

	common_factor = gcd(NSEC_PER_SEC, freq);

	kbdev->backend_time.multiplier = div64_u64(NSEC_PER_SEC, common_factor);
	kbdev->backend_time.divisor = div64_u64(freq, common_factor);

	if (!kbdev->backend_time.divisor) {
		dev_warn(kbdev->dev, "CPU to GPU divisor is zero!");
		err = -EINVAL;
		goto disable_registers;
	}

	kbase_backend_invalidate_gpu_timestamp_offset(
		kbdev); /* force computation of GPU Timestamp offset */
#endif

	if (kbase_timeout_scaling_init(kbdev)) {
		dev_warn(kbdev->dev, "Could not initialize timeout scaling");
		err = -EINVAL;
	}

#if MALI_USE_CSF
disable_registers:
	kbase_pm_register_access_disable(kbdev);
#endif

	return err;
}

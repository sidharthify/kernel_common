// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 *
 * (C) COPYRIGHT 2019-2024 ARM Limited. All rights reserved.
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
#include <linux/sched/rt.h>
#include <uapi/linux/sched/types.h>

#include <device/mali_kbase_device_internal.h>
#include <device/mali_kbase_device.h>
#include <mali_kbase_hwaccess_instr.h>

#include <mali_kbase_config_defaults.h>
#include <mali_kbase_hwaccess_backend.h>
#include <mali_kbase_ctx_sched.h>
#include <mali_kbase_reset_gpu.h>
#include <hwcnt/mali_kbase_hwcnt_watchdog_if_timer.h>
#include <hwcnt/backend/mali_kbase_hwcnt_backend_jm.h>
#include <hwcnt/backend/mali_kbase_hwcnt_backend_jm_watchdog.h>
#include <backend/gpu/mali_kbase_model_linux.h>

#include <mali_kbase.h>
#include <mali_kbase_io.h>
#include <backend/gpu/mali_kbase_irq_internal.h>
#include <backend/gpu/mali_kbase_jm_internal.h>
#include <backend/gpu/mali_kbase_js_internal.h>
#include <backend/gpu/mali_kbase_pm_internal.h>
#include <mali_kbase_dummy_job_wa.h>
#include <backend/gpu/mali_kbase_clk_rate_trace_mgr.h>
#if IS_ENABLED(CONFIG_MALI_TRACE_POWER_GPU_WORK_PERIOD)
#include <mali_kbase_gpu_metrics.h>
#endif

/**
 * kbase_backend_late_init - Perform any backend-specific initialization.
 * @kbdev:	Device pointer
 *
 * Return: 0 on success, or an error code on failure.
 */
static int kbase_backend_late_init(struct kbase_device *kbdev)
{
	int err;

	err = kbase_hwaccess_pm_init(kbdev);
	if (err)
		return err;

	err = kbase_reset_gpu_init(kbdev);
	if (err)
		goto fail_reset_gpu_init;

	err = kbase_hwaccess_pm_powerup(kbdev, PM_HW_ISSUES_DETECT);
	if (err)
		goto fail_pm_powerup;

	err = kbase_backend_timer_init(kbdev);
	if (err)
		goto fail_timer;

#ifdef CONFIG_MALI_DEBUG
#if IS_ENABLED(CONFIG_MALI_REAL_HW)
	if (kbase_validate_interrupts(kbdev) != 0) {
		dev_err(kbdev->dev, "Interrupt validation failed.\n");
		err = -EINVAL;
		goto fail_interrupt_test;
	}
#endif /* IS_ENABLED(CONFIG_MALI_REAL_HW) */
#endif /* CONFIG_MALI_DEBUG */

	err = kbase_job_slot_init(kbdev);
	if (err)
		goto fail_job_slot;

	/* Do the initialisation of devfreq.
	 * Devfreq needs backend_timer_init() for completion of its
	 * initialisation and it also needs to catch the first callback
	 * occurrence of the runtime_suspend event for maintaining state
	 * coherence with the backend power management, hence needs to be
	 * placed before the kbase_pm_context_idle().
	 */
	err = kbase_backend_devfreq_init(kbdev);
	if (err)
		goto fail_devfreq_init;

	/* Update gpuprops with L2_FEATURES if applicable */
	err = kbase_gpuprops_update_l2_features(kbdev);
	if (err)
		goto fail_update_l2_features;

	init_waitqueue_head(&kbdev->hwaccess.backend.reset_wait);

	/* Idle the GPU and/or cores, if the policy wants it to */
	kbase_pm_context_idle(kbdev);

	mutex_init(&kbdev->fw_load_lock);

	return 0;

fail_update_l2_features:
	kbase_backend_devfreq_term(kbdev);
fail_devfreq_init:
	kbase_job_slot_term(kbdev);
fail_job_slot:

#ifdef CONFIG_MALI_DEBUG
#if IS_ENABLED(CONFIG_MALI_REAL_HW)
fail_interrupt_test:
#endif /* IS_ENABLED(CONFIG_MALI_REAL_HW) */
#endif /* CONFIG_MALI_DEBUG */

	kbase_backend_timer_term(kbdev);
fail_timer:
	kbase_pm_context_idle(kbdev);
	kbase_hwaccess_pm_halt(kbdev);
fail_pm_powerup:
	kbase_reset_gpu_term(kbdev);
fail_reset_gpu_init:
	kbase_hwaccess_pm_term(kbdev);

	return err;
}

/**
 * kbase_backend_late_term - Perform any backend-specific termination.
 * @kbdev:	Device pointer
 */
static void kbase_backend_late_term(struct kbase_device *kbdev)
{
	kbase_backend_devfreq_term(kbdev);
	kbase_job_slot_halt(kbdev);
	kbase_job_slot_term(kbdev);
	kbase_backend_timer_term(kbdev);
	kbase_hwaccess_pm_halt(kbdev);
	kbase_reset_gpu_term(kbdev);
	kbase_hwaccess_pm_term(kbdev);
}

/**
 * kbase_device_hwcnt_watchdog_if_init - Create hardware counter watchdog
 *                                       interface.
 * @kbdev:	Device pointer
 * Return: 0 on success, or an error code on failure.
 */
static int kbase_device_hwcnt_watchdog_if_init(struct kbase_device *kbdev)
{
	return kbase_hwcnt_watchdog_if_timer_create(&kbdev->hwcnt_watchdog_timer);
}

/**
 * kbase_device_hwcnt_watchdog_if_term - Terminate hardware counter watchdog
 *                                       interface.
 * @kbdev:	Device pointer
 */
static void kbase_device_hwcnt_watchdog_if_term(struct kbase_device *kbdev)
{
	kbase_hwcnt_watchdog_if_timer_destroy(&kbdev->hwcnt_watchdog_timer);
}

/**
 * kbase_device_hwcnt_backend_jm_init - Create hardware counter backend.
 * @kbdev:	Device pointer
 * Return: 0 on success, or an error code on failure.
 */
static int kbase_device_hwcnt_backend_jm_init(struct kbase_device *kbdev)
{
	return kbase_hwcnt_backend_jm_create(kbdev, &kbdev->hwcnt_gpu_jm_backend);
}

/**
 * kbase_device_hwcnt_backend_jm_term - Terminate hardware counter backend.
 * @kbdev:	Device pointer
 */
static void kbase_device_hwcnt_backend_jm_term(struct kbase_device *kbdev)
{
	kbase_hwcnt_backend_jm_destroy(&kbdev->hwcnt_gpu_jm_backend);
}

/**
 * kbase_device_hwcnt_backend_jm_watchdog_init - Create hardware counter watchdog backend.
 * @kbdev:	Device pointer
 * Return: 0 on success, or an error code on failure.
 */
static int kbase_device_hwcnt_backend_jm_watchdog_init(struct kbase_device *kbdev)
{
	const u32 timer_interval =
		(kbdev->gpu_props.impl_tech == THREAD_FEATURES_IMPLEMENTATION_TECHNOLOGY_FPGA) ||
				(kbdev->gpu_props.impl_tech ==
				 THREAD_FEATURES_IMPLEMENTATION_TECHNOLOGY_SOFTWARE) ?
			      HWCNT_BACKEND_WATCHDOG_TIMER_INTERVAL_FPGA_MS :
			      HWCNT_BACKEND_WATCHDOG_TIMER_INTERVAL_MS;
	return kbase_hwcnt_backend_jm_watchdog_create(&kbdev->hwcnt_gpu_jm_backend,
						      &kbdev->hwcnt_watchdog_timer,
						      &kbdev->hwcnt_gpu_iface, timer_interval);
}

/**
 * kbase_device_hwcnt_backend_jm_watchdog_term - Terminate hardware counter watchdog backend.
 * @kbdev:	Device pointer
 */
static void kbase_device_hwcnt_backend_jm_watchdog_term(struct kbase_device *kbdev)
{
	kbase_hwcnt_backend_jm_watchdog_destroy(&kbdev->hwcnt_gpu_iface);
}

static const struct kbase_device_init dev_init[] = {
#if !IS_ENABLED(CONFIG_MALI_REAL_HW)
	{ kbase_gpu_device_create, kbase_gpu_device_destroy, "Dummy model initialization failed" },
#else /* !IS_ENABLED(CONFIG_MALI_REAL_HW) */
	{ kbase_get_irqs, NULL, "IRQ search failed" },
	{ registers_map, registers_unmap, "Register map failed" },
#endif /* !IS_ENABLED(CONFIG_MALI_REAL_HW) */
#if IS_ENABLED(CONFIG_MALI_TRACE_POWER_GPU_WORK_PERIOD)
	{ kbase_gpu_metrics_init, kbase_gpu_metrics_term, "GPU metrics initialization failed" },
#endif /* IS_ENABLED(CONFIG_MALI_TRACE_POWER_GPU_WORK_PERIOD) */
	{ power_control_init, power_control_term, "Power control initialization failed" },
	{ kbase_io_init, kbase_io_term, "Kbase IO initialization failed" },
	{ kbase_device_io_history_init, kbase_device_io_history_term,
	  "Register access history initialization failed" },
	{ kbase_device_early_init, kbase_device_early_term, "Early device initialization failed" },
	{ kbase_backend_time_init, NULL, "Time backend initialization failed" },
	{ kbase_device_misc_init, kbase_device_misc_term,
	  "Miscellaneous device initialization failed" },
	{ kbase_device_pcm_dev_init, kbase_device_pcm_dev_term,
	  "Priority control manager initialization failed" },
	{ kbase_ctx_sched_init, kbase_ctx_sched_term, "Context scheduler initialization failed" },
	{ kbase_mem_init, kbase_mem_term, "Memory subsystem initialization failed" },
	{ kbase_device_coherency_init, NULL, "Device coherency init failed" },
	{ kbase_protected_mode_init, kbase_protected_mode_term,
	  "Protected mode subsystem initialization failed" },
	{ kbase_device_list_init, kbase_device_list_term, "Device list setup failed" },
	{ kbasep_js_devdata_init, kbasep_js_devdata_term, "Job JS devdata initialization failed" },
	{ kbase_device_timeline_init, kbase_device_timeline_term,
	  "Timeline stream initialization failed" },
	{ kbase_clk_rate_trace_manager_init, kbase_clk_rate_trace_manager_term,
	  "Clock rate trace manager initialization failed" },
	{ kbase_instr_backend_init, kbase_instr_backend_term,
	  "Instrumentation backend initialization failed" },
	{ kbase_device_hwcnt_watchdog_if_init, kbase_device_hwcnt_watchdog_if_term,
	  "GPU hwcnt backend watchdog interface creation failed" },
	{ kbase_device_hwcnt_backend_jm_init, kbase_device_hwcnt_backend_jm_term,
	  "GPU hwcnt backend creation failed" },
	{ kbase_device_hwcnt_backend_jm_watchdog_init, kbase_device_hwcnt_backend_jm_watchdog_term,
	  "GPU hwcnt watchdog backend creation failed" },
	{ kbase_device_hwcnt_context_init, kbase_device_hwcnt_context_term,
	  "GPU hwcnt context initialization failed" },
	{ kbase_device_hwcnt_virtualizer_init, kbase_device_hwcnt_virtualizer_term,
	  "GPU hwcnt virtualizer initialization failed" },
	{ kbase_device_kinstr_prfcnt_init, kbase_device_kinstr_prfcnt_term,
	  "Performance counter instrumentation initialization failed" },
	{ kbase_backend_late_init, kbase_backend_late_term, "Late backend initialization failed" },
	{ kbase_debug_job_fault_dev_init, kbase_debug_job_fault_dev_term,
	  "Job fault debug initialization failed" },
	{ kbase_device_debugfs_init, kbase_device_debugfs_term, "DebugFS initialization failed" },
	/* Sysfs init needs to happen before registering the device with
	 * misc_register(), otherwise it causes a race condition between
	 * registering the device and a uevent event being generated for
	 * userspace, causing udev rules to run which might expect certain
	 * sysfs attributes present. As a result of the race condition
	 * we avoid, some Mali sysfs entries may have appeared to udev
	 * to not exist.
	 * For more information, see
	 * https://www.kernel.org/doc/Documentation/driver-model/device.txt, the
	 * paragraph that starts with "Word of warning", currently the
	 * second-last paragraph.
	 */
	{ kbase_sysfs_init, kbase_sysfs_term, "SysFS group creation failed" },
	{ kbase_device_misc_register, kbase_device_misc_deregister,
	  "Misc device registration failed" },
	{ kbase_gpuprops_populate_user_buffer, kbase_gpuprops_free_user_buffer,
	  "GPU property population failed" },
	{ NULL, kbase_dummy_job_wa_cleanup, NULL },
	{ kbase_device_late_init, kbase_device_late_term, "Late device initialization failed" },
	{ kbase_pm_apc_init, kbase_pm_apc_term,
	  "Asynchronous power control initialization failed" },
};

static void kbase_device_term_partial(struct kbase_device *kbdev, unsigned int i)
{
	while (i-- > 0) {
		if (dev_init[i].term)
			dev_init[i].term(kbdev);
	}
}

void kbase_device_term(struct kbase_device *kbdev)
{
	kbase_device_term_partial(kbdev, ARRAY_SIZE(dev_init));
	kbasep_js_devdata_halt(kbdev);
	kbase_mem_halt(kbdev);
}

int kbase_device_init(struct kbase_device *kbdev)
{
	int err = 0;
	unsigned int i = 0;

	dev_info(kbdev->dev, "Kernel DDK version %s", MALI_RELEASE_NAME);

	kbase_device_id_init(kbdev);
	kbase_disjoint_init(kbdev);

	for (i = 0; i < ARRAY_SIZE(dev_init); i++) {
		if (dev_init[i].init) {
			err = dev_init[i].init(kbdev);
			if (err) {
				if (err != -EPROBE_DEFER)
					dev_err(kbdev->dev, "%s error = %d\n", dev_init[i].err_mes,
						err);
				kbase_device_term_partial(kbdev, i);
				break;
			}
		}
	}

	if (err)
		return err;

	err = kbase_kthread_run_worker_rt(kbdev, &kbdev->job_done_worker, "mali_jd_thread");
	if (err)
		return err;

	kthread_init_worker(&kbdev->event_worker);
	kbdev->event_worker.task =
		kthread_run(kthread_worker_fn, &kbdev->event_worker, "mali_event_thread");
	if (IS_ERR(kbdev->event_worker.task)) {
		err = -ENOMEM;
	}

	return err;
}

int kbase_device_firmware_init_once(struct kbase_device *kbdev)
{
	int ret = 0;

	mutex_lock(&kbdev->fw_load_lock);

	if (!kbdev->dummy_job_wa_loaded) {
		ret = kbase_dummy_job_wa_load(kbdev);
		if (!ret)
			kbdev->dummy_job_wa_loaded = true;
	}

	mutex_unlock(&kbdev->fw_load_lock);

	return ret;
}
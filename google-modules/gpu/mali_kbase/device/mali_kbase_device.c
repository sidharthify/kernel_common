// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 *
 * (C) COPYRIGHT 2010-2024 ARM Limited. All rights reserved.
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

/*
 * Base kernel device APIs
 */

#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/seq_file.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/types.h>
#include <linux/oom.h>

#include <mali_kbase.h>
#include <mali_kbase_defs.h>
#include <mali_kbase_hwaccess_instr.h>
#include <mali_kbase_hwaccess_time.h>
#include <mali_kbase_hw.h>
#include <mali_kbase_config_defaults.h>
#include <linux/priority_control_manager.h>

#include <tl/mali_kbase_timeline.h>
#include "mali_kbase_kinstr_prfcnt.h"
#include "hwcnt/mali_kbase_hwcnt_context.h"
#include "hwcnt/mali_kbase_hwcnt_virtualizer.h"

#include "mali_kbase_device.h"
#include "mali_kbase_device_internal.h"
#include "backend/gpu/mali_kbase_pm_internal.h"
#include "backend/gpu/mali_kbase_irq_internal.h"
#include "mali_kbase_pbha.h"
#include "arbiter/mali_kbase_arbiter_pm.h"
#include <mali_kbase_io.h>

#if defined(CONFIG_DEBUG_FS) && !IS_ENABLED(CONFIG_MALI_NO_MALI)

/* Number of register accesses for the buffer that we allocate during
 * initialization time. The buffer size can be changed later via debugfs.
 */
#define KBASEP_DEFAULT_REGISTER_HISTORY_SIZE ((u16)512)

#endif /* defined(CONFIG_DEBUG_FS) && !IS_ENABLED(CONFIG_MALI_NO_MALI) */

static DEFINE_MUTEX(kbase_dev_list_lock);
static LIST_HEAD(kbase_dev_list);
static unsigned int kbase_dev_nr;

static unsigned int mma_wa_id;

static int set_mma_wa_id(const char *val, const struct kernel_param *kp)
{
	return kbase_param_set_uint_minmax(val, kp, 1, 15);
}

static const struct kernel_param_ops mma_wa_id_ops = {
	.set = set_mma_wa_id,
	.get = param_get_uint,
};

module_param_cb(mma_wa_id, &mma_wa_id_ops, &mma_wa_id, 0444);
__MODULE_PARM_TYPE(mma_wa_id, "uint");
MODULE_PARM_DESC(mma_wa_id, "PBHA ID for MMA workaround. Valid range is from 1 to 15.");

struct kbase_device *kbase_device_alloc(void)
{
	return vzalloc(sizeof(struct kbase_device));
}

/**
 * kbase_device_all_as_init() - Initialise address space objects of the device.
 *
 * @kbdev: Pointer to kbase device.
 *
 * Return: 0 on success otherwise non-zero.
 */
static int kbase_device_all_as_init(struct kbase_device *kbdev)
{
	unsigned int i;
	int err = 0;

	for (i = 0; i < (unsigned int)kbdev->nr_hw_address_spaces; i++) {
		err = kbase_mmu_as_init(kbdev, i);
		if (err)
			break;
	}

	if (err) {
		while (i-- > 0)
			kbase_mmu_as_term(kbdev, i);
	}

	return err;
}

static void kbase_device_all_as_term(struct kbase_device *kbdev)
{
	unsigned int i;

	for (i = 0; i < (unsigned int)kbdev->nr_hw_address_spaces; i++)
		kbase_mmu_as_term(kbdev, i);
}

static int pcm_prioritized_process_cb(struct notifier_block *nb, unsigned long action, void *data)
{
#if MALI_USE_CSF

	struct kbase_device *const kbdev =
		container_of(nb, struct kbase_device, pcm_prioritized_process_nb);
	struct pcm_prioritized_process_notifier_data *const notifier_data = data;
	int ret = -EINVAL;

	switch (action) {
	case ADD_PRIORITIZED_PROCESS:
		if (kbasep_adjust_prioritized_process(kbdev, true, notifier_data->pid))
			ret = 0;
		break;
	case REMOVE_PRIORITIZED_PROCESS:
		if (kbasep_adjust_prioritized_process(kbdev, false, notifier_data->pid))
			ret = 0;
		break;
	}

	return ret;

#endif /* MALI_USE_CSF */

	return 0;
}

int kbase_device_pcm_dev_init(struct kbase_device *const kbdev)
{
	int err = 0;

#if IS_ENABLED(CONFIG_OF)
	struct device_node *prio_ctrl_node;

	/* Check to see whether or not a platform specific priority control manager
	 * is available.
	 */
	prio_ctrl_node = of_parse_phandle(kbdev->dev->of_node, "priority-control-manager", 0);
	if (!prio_ctrl_node) {
		dev_info(kbdev->dev, "No priority control manager is configured");
	} else {
		struct platform_device *const pdev = of_find_device_by_node(prio_ctrl_node);

		if (!pdev) {
			dev_err(kbdev->dev,
				"The configured priority control manager was not found");
		} else {
			struct priority_control_manager_device *pcm_dev =
				platform_get_drvdata(pdev);
			if (!pcm_dev) {
				dev_info(kbdev->dev, "Priority control manager is not ready");
				err = -EPROBE_DEFER;
			} else if (!try_module_get(pcm_dev->owner)) {
				dev_err(kbdev->dev,
					"Failed to get priority control manager module");
				err = -ENODEV;
			} else {
				dev_info(kbdev->dev,
					 "Priority control manager successfully loaded");
				kbdev->pcm_dev = pcm_dev;

				kbdev->pcm_prioritized_process_nb = (struct notifier_block){
					.notifier_call = &pcm_prioritized_process_cb,
				};
				if (pcm_dev->ops.pcm_prioritized_process_notifier_register !=
				    NULL) {
					if (pcm_dev->ops.pcm_prioritized_process_notifier_register(
						    pcm_dev, &kbdev->pcm_prioritized_process_nb))
						dev_warn(
							kbdev->dev,
							"Failed to register for changes in prioritized processes");
				}
			}
		}
		of_node_put(prio_ctrl_node);
	}
#endif /* CONFIG_OF */

	return err;
}

void kbase_device_pcm_dev_term(struct kbase_device *const kbdev)
{
	struct priority_control_manager_device *const pcm_dev = kbdev->pcm_dev;

	if (pcm_dev) {
		if (pcm_dev->ops.pcm_prioritized_process_notifier_unregister != NULL)
			pcm_dev->ops.pcm_prioritized_process_notifier_unregister(
				pcm_dev, &kbdev->pcm_prioritized_process_nb);
		module_put(pcm_dev->owner);
	}
}

#define KBASE_PAGES_TO_KIB(pages) (((unsigned int)pages) << (PAGE_SHIFT - 10))

/**
 * mali_oom_notifier_handler - Mali driver out-of-memory handler
 *
 * @nb: notifier block - used to retrieve kbdev pointer
 * @action: action (unused)
 * @data: data pointer (unused)
 *
 * This function simply lists memory usage by the Mali driver, per GPU device,
 * for diagnostic purposes.
 *
 * Return: NOTIFY_OK on success, NOTIFY_BAD otherwise.
 */
static int mali_oom_notifier_handler(struct notifier_block *nb, unsigned long action, void *data)
{
	struct kbase_device *kbdev;
	struct kbase_context *kctx = NULL;
	unsigned long kbdev_alloc_total;

	CSTD_UNUSED(action);
	CSTD_UNUSED(data);

	if (WARN_ON(nb == NULL))
		return NOTIFY_BAD;

	kbdev = container_of(nb, struct kbase_device, oom_notifier_block);

	kbdev_alloc_total = KBASE_PAGES_TO_KIB(atomic_read(&(kbdev->memdev.used_pages)));

	dev_info(kbdev->dev,
		"System reports low memory, GPU memory usage summary:\n");

	mutex_lock(&kbdev->kctx_list_lock);

	list_for_each_entry(kctx, &kbdev->kctx_list, kctx_list_link) {
		struct task_struct *task = kctx->task;
		unsigned long task_alloc_total =
			KBASE_PAGES_TO_KIB(atomic_read(&(kctx->used_pages)));

		dev_info(kbdev->dev,
			" tsk %s tgid %u pid %u has allocated %lu kB GPU memory\n",
			task ? task->comm : "[null task]", kctx->tgid, kctx->pid,
			task_alloc_total);
	}

	dev_info(kbdev->dev, "End of summary, device usage is %lu kB\n",
		kbdev_alloc_total);

	mutex_unlock(&kbdev->kctx_list_lock);
	return NOTIFY_OK;
}

int kbase_device_misc_init(struct kbase_device *const kbdev)
{
	int err;
#if IS_ENABLED(CONFIG_ARM64)
	struct device_node *np = NULL;
#endif /* CONFIG_ARM64 */

	spin_lock_init(&kbdev->mmu_mask_change);
	mutex_init(&kbdev->mmu_hw_mutex);
#if IS_ENABLED(CONFIG_ARM64)
	np = kbdev->dev->of_node;
	if (np != NULL) {
		/* Read "-" versions of the properties and fallback to "_"
		 * if these are not found
		 */
		if (of_property_read_u32(np, "snoop-enable-smc", &kbdev->snoop_enable_smc) &&
		    of_property_read_u32(np, "snoop_enable_smc", &kbdev->snoop_enable_smc))
			kbdev->snoop_enable_smc = 0;
		if (of_property_read_u32(np, "snoop-disable-smc", &kbdev->snoop_disable_smc) &&
		    of_property_read_u32(np, "snoop_disable_smc", &kbdev->snoop_disable_smc))
			kbdev->snoop_disable_smc = 0;
		/* Either both or none of the calls should be provided. */
		if (!((kbdev->snoop_disable_smc == 0 && kbdev->snoop_enable_smc == 0) ||
		      (kbdev->snoop_disable_smc != 0 && kbdev->snoop_enable_smc != 0))) {
			WARN_ON(1);
			return -EINVAL;
		}
	}
#endif /* CONFIG_ARM64 */

	/* Workaround a pre-3.13 Linux issue, where dma_mask is NULL when our
	 * device structure was created by device-tree
	 */
	if (!kbdev->dev->dma_mask)
		kbdev->dev->dma_mask = &kbdev->dev->coherent_dma_mask;

	err = dma_set_mask(kbdev->dev, DMA_BIT_MASK(kbdev->gpu_props.mmu.pa_bits));
	if (err)
		goto dma_set_mask_failed;

	err = dma_set_coherent_mask(kbdev->dev, DMA_BIT_MASK(kbdev->gpu_props.mmu.pa_bits));
	if (err)
		goto dma_set_mask_failed;


	/* There is no limit for Mali, so set to max. */
	if (kbdev->dev->dma_parms)
		err = dma_set_max_seg_size(kbdev->dev, UINT_MAX);
	if (err)
		goto dma_set_mask_failed;

	kbdev->nr_hw_address_spaces = (s8)kbdev->gpu_props.num_address_spaces;

	err = kbase_device_all_as_init(kbdev);
	if (err)
		goto dma_set_mask_failed;

	/* Set mma_wa_id if it has been passed in as a module parameter */
	if ((kbdev->gpu_props.gpu_id.arch_id >= GPU_ID_ARCH_MAKE(14, 8, 0)) && mma_wa_id != 0)
		kbdev->mma_wa_id = mma_wa_id;

	err = kbase_pbha_read_dtb(kbdev);
	if (err)
		goto term_as;

	init_waitqueue_head(&kbdev->cache_clean_wait);

	kbase_debug_assert_register_hook(&kbase_ktrace_hook_wrapper, kbdev);

	kbdev->pm.dvfs_period = DEFAULT_PM_DVFS_PERIOD;

#if MALI_USE_CSF
	kbdev->reset_timeout_ms = kbase_get_timeout_ms(kbdev, CSF_GPU_RESET_TIMEOUT);
#else /* MALI_USE_CSF */
	kbdev->reset_timeout_ms = JM_DEFAULT_RESET_TIMEOUT_MS;
#endif /* !MALI_USE_CSF */

	kbdev->mmu_mode = kbase_mmu_mode_get_aarch64();
	mutex_init(&kbdev->kctx_list_lock);
	INIT_LIST_HEAD(&kbdev->kctx_list);

	dev_dbg(kbdev->dev, "Registering mali_oom_notifier_handlern");
	kbdev->oom_notifier_block.notifier_call = mali_oom_notifier_handler;
	err = register_oom_notifier(&kbdev->oom_notifier_block);

	if (err) {
		dev_err(kbdev->dev,
			"Unable to register OOM notifier for Mali - but will continue\n");
		kbdev->oom_notifier_block.notifier_call = NULL;
	}

#if MALI_USE_CSF
	atomic_set(&kbdev->fence_signal_timeout_enabled, 1);
#endif

	return 0;

term_as:
	kbase_device_all_as_term(kbdev);
dma_set_mask_failed:
	return err;
}

void kbase_device_misc_term(struct kbase_device *kbdev)
{
	KBASE_DEBUG_ASSERT(kbdev);

	WARN_ON(!list_empty(&kbdev->kctx_list));

#if KBASE_KTRACE_ENABLE
	kbase_debug_assert_register_hook(NULL, NULL);
#endif
	kbase_device_all_as_term(kbdev);


	if (kbdev->oom_notifier_block.notifier_call)
		unregister_oom_notifier(&kbdev->oom_notifier_block);

#if MALI_USE_CSF && IS_ENABLED(CONFIG_SYNC_FILE)
	if (atomic_read(&kbdev->live_fence_metadata) > 0)
		dev_warn(kbdev->dev, "Terminating Kbase device with live fence metadata!");
#endif
}

void kbase_device_free(struct kbase_device *kbdev)
{
	vfree(kbdev);
}

void kbase_device_id_init(struct kbase_device *kbdev)
{
	scnprintf(kbdev->devname, DEVNAME_SIZE, "%s%d", KBASE_DRV_NAME, kbase_dev_nr);
	kbdev->id = kbase_dev_nr;
}

void kbase_increment_device_id(void)
{
	kbase_dev_nr++;
}

int kbase_device_hwcnt_context_init(struct kbase_device *kbdev)
{
	return kbase_hwcnt_context_init(&kbdev->hwcnt_gpu_iface, &kbdev->hwcnt_gpu_ctx);
}

void kbase_device_hwcnt_context_term(struct kbase_device *kbdev)
{
	kbase_hwcnt_context_term(kbdev->hwcnt_gpu_ctx);
}

int kbase_device_hwcnt_virtualizer_init(struct kbase_device *kbdev)
{
	return kbase_hwcnt_virtualizer_init(kbdev->hwcnt_gpu_ctx,
					    KBASE_HWCNT_GPU_VIRTUALIZER_DUMP_THRESHOLD_NS,
					    &kbdev->hwcnt_gpu_virt);
}

void kbase_device_hwcnt_virtualizer_term(struct kbase_device *kbdev)
{
	kbase_hwcnt_virtualizer_term(kbdev->hwcnt_gpu_virt);
}

int kbase_device_timeline_init(struct kbase_device *kbdev)
{
	return kbase_timeline_init(&kbdev->timeline, &kbdev->timeline_flags);
}

void kbase_device_timeline_term(struct kbase_device *kbdev)
{
	kbase_timeline_term(kbdev->timeline);
}

int kbase_device_kinstr_prfcnt_init(struct kbase_device *kbdev)
{
	return kbase_kinstr_prfcnt_init(kbdev->hwcnt_gpu_virt, &kbdev->kinstr_prfcnt_ctx);
}

void kbase_device_kinstr_prfcnt_term(struct kbase_device *kbdev)
{
	kbase_kinstr_prfcnt_term(kbdev->kinstr_prfcnt_ctx);
}

int kbase_device_io_history_init(struct kbase_device *kbdev)
{
	return kbase_io_history_init(&kbdev->io_history, KBASEP_DEFAULT_REGISTER_HISTORY_SIZE);
}

void kbase_device_io_history_term(struct kbase_device *kbdev)
{
	kbase_io_history_term(&kbdev->io_history);
}

int kbase_device_misc_register(struct kbase_device *kbdev)
{
	return misc_register(&kbdev->mdev);
}

void kbase_device_misc_deregister(struct kbase_device *kbdev)
{
	misc_deregister(&kbdev->mdev);
}

int kbase_device_list_init(struct kbase_device *kbdev)
{
	const struct list_head *dev_list;

	dev_list = kbase_device_get_list();
	list_add(&kbdev->entry, &kbase_dev_list);
	kbase_device_put_list(dev_list);

	return 0;
}

void kbase_device_list_term(struct kbase_device *kbdev)
{
	const struct list_head *dev_list;

	dev_list = kbase_device_get_list();
	list_del(&kbdev->entry);
	kbase_device_put_list(dev_list);
}

const struct list_head *kbase_device_get_list(void)
{
	mutex_lock(&kbase_dev_list_lock);
	return &kbase_dev_list;
}
KBASE_EXPORT_TEST_API(kbase_device_get_list);

void kbase_device_put_list(const struct list_head *dev_list)
{
	CSTD_UNUSED(dev_list);
	mutex_unlock(&kbase_dev_list_lock);
}
KBASE_EXPORT_TEST_API(kbase_device_put_list);

int kbase_device_early_init(struct kbase_device *kbdev)
{
	int err;

	err = kbase_ktrace_init(kbdev);
	if (err)
		return err;

	err = kbasep_platform_device_init(kbdev);
	if (err)
		goto ktrace_term;

	err = kbase_pm_runtime_init(kbdev);
	if (err)
		goto platform_device_term;

	/* This spinlock is initialized before doing the first access to GPU
	 * registers and installing interrupt handlers.
	 */
	spin_lock_init(&kbdev->hwaccess_lock);

	/* Ensure we can access the GPU registers */
	kbase_pm_register_access_enable(kbdev);

	/*
	 * If -EPERM is returned, it means the device backend is not supported, but
	 * device initialization can continue.
	 */
	err = kbase_device_backend_init(kbdev);
	if (err != 0 && err != -EPERM)
		goto pm_runtime_term;

	/*
	 * Initialize register mapping LUTs. This would have been initialized on HW
	 * Arbitration but not on PV or non-arbitration devices.
	 */
	if (!kbase_reg_is_init(kbdev)) {
		/* Initialize GPU_ID props */
		kbase_gpuprops_parse_gpu_id(&kbdev->gpu_props.gpu_id, kbase_reg_get_gpu_id(kbdev));

		err = kbase_regmap_init(kbdev);
		if (err)
			goto backend_term;
	}

	/* Set the list of features available on the current HW
	 * (identified by the GPU_ID register)
	 */
	kbase_hw_set_features_mask(kbdev);

	/* Find out GPU properties based on the GPU feature registers. */
	err = kbase_gpuprops_init(kbdev);
	if (err)
		goto backend_term;

	/* Get the list of workarounds for issues on the current HW
	 * (identified by the GPU_ID register and impl_tech in THREAD_FEATURES)
	 */
	err = kbase_hw_set_issues_mask(kbdev);
	if (err)
		goto gpuprops_term;

	/* We're done accessing the GPU registers for now. */
	kbase_pm_register_access_disable(kbdev);

	if (kbase_has_arbiter(kbdev)) {
		if (kbdev->pm.arb_vm_state)
			err = kbase_arbiter_pm_install_interrupts(kbdev);
	} else {
		err = kbase_install_interrupts(kbdev);
	}
	if (err)
		goto gpuprops_term;

	return 0;

gpuprops_term:
	kbase_gpuprops_term(kbdev);
backend_term:
	kbase_device_backend_term(kbdev);
	kbase_regmap_term(kbdev);
pm_runtime_term:
	if (kbase_io_is_gpu_powered(kbdev))
		kbase_pm_register_access_disable(kbdev);

	kbase_pm_runtime_term(kbdev);
platform_device_term:
	kbasep_platform_device_term(kbdev);
ktrace_term:
	kbase_ktrace_term(kbdev);

	return err;
}

void kbase_device_early_term(struct kbase_device *kbdev)
{
	if (kbase_has_arbiter(kbdev))
		kbase_arbiter_pm_release_interrupts(kbdev);
	else
		kbase_release_interrupts(kbdev);
	kbase_gpuprops_term(kbdev);
	kbase_device_backend_term(kbdev);
	kbase_regmap_term(kbdev);
	kbase_pm_runtime_term(kbdev);
	kbasep_platform_device_term(kbdev);
	kbase_ktrace_term(kbdev);
}

int kbase_device_late_init(struct kbase_device *kbdev)
{
	int err;

	err = kbasep_platform_device_late_init(kbdev);

	return err;
}

void kbase_device_late_term(struct kbase_device *kbdev)
{
	kbasep_platform_device_late_term(kbdev);
}

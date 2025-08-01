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
 * Base kernel Power Management hardware control
 */

#include <mali_kbase.h>
#include <mali_kbase_config_defaults.h>
#include <hw_access/mali_kbase_hw_access_regmap.h>
#include <tl/mali_kbase_tracepoints.h>
#include <mali_kbase_pm.h>
#include <mali_kbase_config_defaults.h>
#include <mali_kbase_smc.h>
#include <mali_kbase_io.h>

#if MALI_USE_CSF
#include <csf/ipa_control/mali_kbase_csf_ipa_control.h>
#else
#include <mali_kbase_hwaccess_jm.h>
#endif /* !MALI_USE_CSF */

#include <mali_kbase_reset_gpu.h>
#include <mali_kbase_ctx_sched.h>
#include <hwcnt/mali_kbase_hwcnt_context.h>
#include <mali_kbase_pbha.h>
#include <backend/gpu/mali_kbase_cache_policy_backend.h>
#include <device/mali_kbase_device.h>
#include <backend/gpu/mali_kbase_irq_internal.h>
#include <backend/gpu/mali_kbase_pm_internal.h>
#include <backend/gpu/mali_kbase_l2_mmu_config.h>
#include <backend/gpu/mali_kbase_pm_event_log.h>
#include <mali_kbase_dummy_job_wa.h>
#include <arbiter/mali_kbase_arbiter_pm.h>

#if MALI_USE_CSF
#include <linux/delay.h>
#include "pixel_gpu_debug.h"
#endif

#include <linux/of.h>

#include <trace/hooks/systrace.h>

#ifdef CONFIG_MALI_CORESTACK
bool corestack_driver_control = true;
#else
bool corestack_driver_control; /* Default value of 0/false */
#endif
module_param(corestack_driver_control, bool, 0444);
MODULE_PARM_DESC(corestack_driver_control,
		 "Let the driver power on/off the GPU core stack independently "
		 "without involving the Power Domain Controller. This should "
		 "only be enabled on platforms for which integration of the PDC "
		 "to the Mali GPU is known to be problematic.");
KBASE_EXPORT_TEST_API(corestack_driver_control);

/**
 * enum kbase_gpu_state - The state of data in the GPU.
 *
 * @GPU_STATE_INTACT: The GPU state is intact
 * @GPU_STATE_LOST: The GPU state is lost
 * @GPU_STATE_IN_RESET: The GPU is in reset state
 *
 * This enumeration is private to the file. It is used as
 * the return values of platform specific PM
 * callback (*power_on_callback).
 */
enum kbase_gpu_state { GPU_STATE_INTACT = 0, GPU_STATE_LOST, GPU_STATE_IN_RESET };

/**
 * enum kbasep_pm_action - Actions that can be performed on a core.
 *
 * @ACTION_PRESENT: The cores that are present
 * @ACTION_READY: The cores that are ready
 * @ACTION_PWRON: Power on the cores specified
 * @ACTION_PWROFF: Power off the cores specified
 * @ACTION_PWRTRANS: The cores that are transitioning
 * @ACTION_PWRACTIVE: The cores that are active
 *
 * This enumeration is private to the file. Its values are set to allow
 * core_type_to_reg() function, which decodes this enumeration, to be simpler
 * and more efficient.
 */
enum kbasep_pm_action {
	ACTION_PRESENT,
	ACTION_READY,
	ACTION_PWRON,
	ACTION_PWROFF,
	ACTION_PWRTRANS,
	ACTION_PWRACTIVE
};

static u64 kbase_pm_get_state(struct kbase_device *kbdev, enum kbase_pm_core_type core_type,
			      enum kbasep_pm_action action);

static void kbase_pm_hw_issues_apply(struct kbase_device *kbdev);

#if MALI_USE_CSF

static bool pwr_status_gpu_reset_allowed(struct kbase_device *kbdev)
{
	const u64 pwr_status = kbase_reg_read64(kbdev, HOST_POWER_ENUM(PWR_STATUS));

	/* Check if Host is allowed to do the GPU reset */
	if (!PWR_STATUS_ALLOW_SOFT_RESET_GET(pwr_status) ||
	    !PWR_STATUS_ALLOW_HARD_RESET_GET(pwr_status)) {
		dev_err(kbdev->dev, "PWR_STATUS value (%llx) doesn't allow GPU reset", pwr_status);
		return false;
	}

	return true;
}

static bool pwr_status_value_valid_post_reset(struct kbase_device *kbdev)
{
	const u64 pwr_status = kbase_reg_read64(kbdev, HOST_POWER_ENUM(PWR_STATUS));

	/* Check if Host is allowed to control the power for all power domains and none
	 * of the controls are delegated.
	 */
	if (!PWR_STATUS_ALLOW_L2_GET(pwr_status) || PWR_STATUS_DELEGATED_L2_GET(pwr_status) ||
	    !PWR_STATUS_ALLOW_TILER_GET(pwr_status) || PWR_STATUS_DELEGATED_TILER_GET(pwr_status) ||
	    !PWR_STATUS_ALLOW_SHADER_GET(pwr_status) ||
	    PWR_STATUS_DELEGATED_SHADER_GET(pwr_status) ||
	    (kbase_csf_dev_has_ne(kbdev) && (!PWR_STATUS_ALLOW_NEURAL_GET(pwr_status) ||
					     PWR_STATUS_DELEGATED_NEURAL_GET(pwr_status)))) {
		dev_err(kbdev->dev, "Unexpected value (%llx) of PWR_STATUS register post reset",
			pwr_status);
		return false;
	}

	return true;
}

bool kbase_pm_is_mcu_desired(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);

	if (unlikely(!kbdev->csf.firmware_inited))
		return false;

	if (kbdev->pm.backend.l2_force_off_after_mcu_halt)
		return false;

	/* Check if policy changing transition needs MCU to be off. */
	if (unlikely(kbdev->pm.backend.policy_change_clamp_state_to_off))
		return false;

	/* Check if MCU should be ON */
	if (kbdev->pm.backend.mcu_desired)
		return true;

	/* For always_on policy, the MCU needs to be kept on even if not required
	 * by Scheduler.
	 * But if System suspend or unload of Kbase is being done then the whole
	 * GPU needs to be turned off. To know that check for the l2_desired flag.
	 */
	if (kbase_pm_no_mcu_core_pwroff(kbdev) && kbdev->pm.backend.l2_desired)
		return true;

#ifdef KBASE_PM_RUNTIME
	if (kbdev->pm.backend.gpu_wakeup_override ||
	    kbdev->pm.backend.runtime_suspend_abort_reason != ABORT_REASON_NONE)
		return true;
#endif

	return false;
}
#endif /* MALI_USE_CSF */

bool kbase_pm_is_l2_desired(struct kbase_device *kbdev)
{
#if !MALI_USE_CSF
	if (kbdev->pm.backend.protected_entry_transition_override)
		return false;

	if (kbdev->pm.backend.protected_transition_override &&
	    kbdev->pm.backend.protected_l2_override)
		return true;

	if (kbdev->pm.backend.protected_transition_override && !kbdev->pm.backend.shaders_desired)
		return false;
#else
	if (unlikely(kbdev->pm.backend.policy_change_clamp_state_to_off))
		return false;

	/* We need to power up the L2 when the MCU is desired */
	if (likely(kbdev->csf.firmware_inited))
		return kbase_pm_is_mcu_desired(kbdev);
#endif

	return kbdev->pm.backend.l2_desired;
}

#if !MALI_USE_CSF
void kbase_pm_protected_override_enable(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);

	kbdev->pm.backend.protected_transition_override = true;
}
void kbase_pm_protected_override_disable(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);

	kbdev->pm.backend.protected_transition_override = false;
}

int kbase_pm_protected_entry_override_enable(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);

	WARN_ON(!kbdev->protected_mode_transition);

	if (kbdev->pm.backend.l2_always_on && (kbdev->system_coherency == COHERENCY_ACE)) {
		WARN_ON(kbdev->pm.backend.protected_entry_transition_override);

		/*
		 * If there is already a GPU reset pending then wait for it to
		 * complete before initiating a special reset for protected
		 * mode entry.
		 */
		if (kbase_reset_gpu_silent(kbdev))
			return -EAGAIN;

		kbdev->pm.backend.protected_entry_transition_override = true;
	}

	return 0;
}

void kbase_pm_protected_entry_override_disable(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);

	WARN_ON(!kbdev->protected_mode_transition);

	if (kbdev->pm.backend.l2_always_on && (kbdev->system_coherency == COHERENCY_ACE)) {
		WARN_ON(!kbdev->pm.backend.protected_entry_transition_override);

		kbdev->pm.backend.protected_entry_transition_override = false;
	}
}

void kbase_pm_protected_l2_override(struct kbase_device *kbdev, bool override)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);

	if (override) {
		kbdev->pm.backend.protected_l2_override++;
		WARN_ON(kbdev->pm.backend.protected_l2_override <= 0);
	} else {
		kbdev->pm.backend.protected_l2_override--;
		WARN_ON(kbdev->pm.backend.protected_l2_override < 0);
	}

	kbase_pm_update_state(kbdev);
}
#endif

#if MALI_USE_CSF
#define ACTION_TYPE_TO_REG_HOST_POWER(core_name)                      \
	{                                                             \
		switch (action) {                                     \
		case ACTION_PRESENT:                                  \
			reg = HOST_POWER_ENUM(core_name##_PRESENT);   \
			break;                                        \
		case ACTION_READY:                                    \
			reg = HOST_POWER_ENUM(core_name##_READY);     \
			break;                                        \
		case ACTION_PWRON:                                    \
			fallthrough;                                  \
		case ACTION_PWROFF:                                   \
			reg = HOST_POWER_ENUM(PWR_CMDARG);            \
			break;                                        \
		case ACTION_PWRTRANS:                                 \
			reg = HOST_POWER_ENUM(core_name##_PWRTRANS);  \
			break;                                        \
		case ACTION_PWRACTIVE:                                \
			reg = HOST_POWER_ENUM(core_name##_PWRACTIVE); \
			break;                                        \
		default:                                              \
			dev_err(kbdev->dev, "Invalid action");        \
			break;                                        \
		}                                                     \
	}
#endif

#define ACTION_TYPE_TO_REG_GPU_CONTROL(core_name)                      \
	{                                                              \
		switch (action) {                                      \
		case ACTION_PRESENT:                                   \
			reg = GPU_CONTROL_ENUM(core_name##_PRESENT);   \
			break;                                         \
		case ACTION_READY:                                     \
			reg = GPU_CONTROL_ENUM(core_name##_READY);     \
			break;                                         \
		case ACTION_PWRON:                                     \
			reg = GPU_CONTROL_ENUM(core_name##_PWRON);     \
			break;                                         \
		case ACTION_PWROFF:                                    \
			reg = GPU_CONTROL_ENUM(core_name##_PWROFF);    \
			break;                                         \
		case ACTION_PWRTRANS:                                  \
			reg = GPU_CONTROL_ENUM(core_name##_PWRTRANS);  \
			break;                                         \
		case ACTION_PWRACTIVE:                                 \
			reg = GPU_CONTROL_ENUM(core_name##_PWRACTIVE); \
			break;                                         \
		default:                                               \
			dev_err(kbdev->dev, "Invalid action");         \
			break;                                         \
		}                                                      \
	}

/**
 * core_type_to_reg - Decode a core type and action to a register.
 *
 * @kbdev:     The kbase device for the core
 * @core_type: The type of core
 * @action:    The type of action
 *
 * Given a core type (defined by kbase_pm_core_type) and an action (defined
 * by kbasep_pm_action) this function will return the register offset that
 * will perform the action on the core type. The register returned is the _LO
 * register and an offset must be applied to use the _HI register.
 *
 * Return: The register offset of the _LO register that performs an action of
 * type @action on a core of type @core_type.
 */
static u32 core_type_to_reg(struct kbase_device *kbdev, enum kbase_pm_core_type core_type,
			    enum kbasep_pm_action action)
{
	u32 reg = 0x0;

#if MALI_USE_CSF
	if (kbdev->pm.backend.has_host_pwr_iface) {
		switch (core_type) {
		case KBASE_PM_CORE_L2:
			ACTION_TYPE_TO_REG_HOST_POWER(L2);
			break;
		case KBASE_PM_CORE_SHADER:
			ACTION_TYPE_TO_REG_HOST_POWER(SHADER);
			break;
		case KBASE_PM_CORE_TILER:
			ACTION_TYPE_TO_REG_HOST_POWER(TILER);
			break;
		case KBASE_PM_CORE_STACK:
			if (corestack_driver_control) {
				switch (action) {
				case ACTION_PRESENT:
					reg = HOST_POWER_ENUM(STACK_PRESENT);
					break;
				case ACTION_READY:
					reg = HOST_POWER_ENUM(STACK_READY);
					break;
				case ACTION_PWRON:
					fallthrough;
				case ACTION_PWROFF:
					reg = HOST_POWER_ENUM(PWR_CMDARG);
					break;
				case ACTION_PWRTRANS:
					reg = HOST_POWER_ENUM(STACK_PWRTRANS);
					break;
				default:
					dev_err(kbdev->dev, "Invalid action for core stack");
				}
			}
			break;
		case KBASE_PM_CORE_NEURAL:
			ACTION_TYPE_TO_REG_HOST_POWER(NEURAL);
			break;
		case KBASE_PM_CORE_BASE:
			ACTION_TYPE_TO_REG_HOST_POWER(BASE);
			break;
		default:
			dev_err(kbdev->dev, "Invalid core type for host power control");
			break;
		}
	} else
#endif
	{
		switch (core_type) {
		case KBASE_PM_CORE_L2:
			ACTION_TYPE_TO_REG_GPU_CONTROL(L2);
			break;
		case KBASE_PM_CORE_SHADER:
			ACTION_TYPE_TO_REG_GPU_CONTROL(SHADER);
			break;
		case KBASE_PM_CORE_TILER:
			ACTION_TYPE_TO_REG_GPU_CONTROL(TILER);
			break;
		case KBASE_PM_CORE_STACK:
			if (corestack_driver_control) {
				switch (action) {
				case ACTION_PRESENT:
					reg = GPU_CONTROL_ENUM(STACK_PRESENT);
					break;
				case ACTION_READY:
					reg = GPU_CONTROL_ENUM(STACK_READY);
					break;
				case ACTION_PWRON:
					reg = GPU_CONTROL_ENUM(STACK_PWRON);
					break;
				case ACTION_PWROFF:
					reg = GPU_CONTROL_ENUM(STACK_PWROFF);
					break;
				case ACTION_PWRTRANS:
					reg = GPU_CONTROL_ENUM(STACK_PWRTRANS);
					break;
				default:
					dev_err(kbdev->dev, "Invalid action for core stack");
				}
			}
			break;
		default:
			dev_err(kbdev->dev, "Invalid core type for gpu control");
			break;
		}
	}

	WARN_ON(!reg);
	return reg;
}

#if !MALI_USE_CSF
/**
 * map_core_type_to_tl_pm_state - Map core type to TL_PM_STATE.
 *
 * @kbdev:      The kbase device for the core
 * @core_type:  The type of core
 *
 * Given a core_type (defined by kbase_pm_core_type) this function will return
 * TL_PM_STATE_*, which is a mapping of core_type to respective core type timeline value.
 *
 * Return: Core type timeline value.
 */
__pure static u32 map_core_type_to_tl_pm_state(struct kbase_device *kbdev,
					       enum kbase_pm_core_type core_type)
{
	u32 map = 0;

	switch (core_type) {
	case KBASE_PM_CORE_L2:
		map = TL_PM_STATE_L2;
		break;
	case KBASE_PM_CORE_SHADER:
		map = TL_PM_STATE_SHADER;
		break;
	case KBASE_PM_CORE_TILER:
		map = TL_PM_STATE_TILER;
		break;
	case KBASE_PM_CORE_STACK:
		if (corestack_driver_control)
			map = TL_PM_STATE_STACK;
		break;
	default:
		dev_err(kbdev->dev, "Invalid core type");
	}

	/* Core stack might not change default value */
	WARN_ON(!map);
	return map;
}
#endif

#if IS_ENABLED(CONFIG_ARM64) && !MALI_USE_CSF

static void mali_cci_flush_l2(struct kbase_device *kbdev)
{
	u32 val;
	const u32 mask = CLEAN_CACHES_COMPLETED | RESET_COMPLETED;
	const u32 timeout_us =
		kbase_get_timeout_ms(kbdev, KBASE_CLEAN_CACHE_TIMEOUT) * USEC_PER_MSEC;

	/*
	 * Note that we don't take the cache flush mutex here since
	 * we expect to be the last user of the L2, all other L2 users
	 * would have dropped their references, to initiate L2 power
	 * down, L2 power down being the only valid place for this
	 * to be called from.
	 */

	kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_COMMAND), GPU_COMMAND_CACHE_CLN_INV_L2);

	/* Wait for cache flush to complete before continuing, exit on
	 * gpu resets or loop expiry.
	 */
	kbase_reg_poll32_timeout(kbdev, GPU_CONTROL_ENUM(GPU_IRQ_RAWSTAT), val, val & mask, 0,
				 timeout_us, false);
}
#endif

#if MALI_USE_CSF
/**
 * pwr_cmd_constructor - Construct an on or off power command for a core type.
 *
 * @kbdev: The kbase device for the core
 * @core_type: The type of core to control
 * @action: ACTION_PWRON or ACTION_PWROFF
 *
 * Creates the 64 bit number to be written into the POWER_COMMAND register
 * for the specified action, and domain to control.
 * Also controls the ray tracing unit subdomain when controlling the shader.
 *
 * Return: The command to be written to the POWER_COMMAND register.
 */
static u64 pwr_cmd_constructor(struct kbase_device *kbdev, enum kbase_pm_core_type core_type,
			       enum kbasep_pm_action action)
{
	u64 pwr_cmd = 0x0;

	/* Set the command portion of the PWR_COMMAND */
	if (action == ACTION_PWRON)
		pwr_cmd = PWR_COMMAND_COMMAND_SET(pwr_cmd, PWR_COMMAND_COMMAND_POWER_UP);
	else if (action == ACTION_PWROFF)
		pwr_cmd = PWR_COMMAND_COMMAND_SET(pwr_cmd, PWR_COMMAND_COMMAND_POWER_DOWN);
	else
		dev_err(kbdev->dev, "Action invalid, must be ACTION_PWRON or ACTION_PWROFF.");

	/* Set the domain and sub-domain portion of the PWR_COMMAND */
	switch (core_type) {
	case KBASE_PM_CORE_L2:
		pwr_cmd = PWR_COMMAND_DOMAIN_SET(pwr_cmd, PWR_COMMAND_DOMAIN_L2);
		break;
	case KBASE_PM_CORE_TILER:
		pwr_cmd = PWR_COMMAND_DOMAIN_SET(pwr_cmd, PWR_COMMAND_DOMAIN_TILER);
		break;
	case KBASE_PM_CORE_SHADER:
		pwr_cmd = PWR_COMMAND_DOMAIN_SET(pwr_cmd, PWR_COMMAND_DOMAIN_SHADER);
		/* Enable RTU subdomain for the shader core */
		if (kbase_csf_dev_has_rtu(kbdev))
			pwr_cmd = PWR_COMMAND_SUBDOMAIN_SET(pwr_cmd, 0x01);
		break;
	case KBASE_PM_CORE_NEURAL:
		pwr_cmd = PWR_COMMAND_DOMAIN_SET(pwr_cmd, PWR_COMMAND_DOMAIN_NEURAL);
		break;
	default:
		dev_err(kbdev->dev, "Invalid core type for PWR_COMMAND use.");
		break;
	}
	return pwr_cmd;
}

#endif /* MALI_USE_CSF */

/**
 * kbase_pm_invoke - Invokes an action on a core set
 *
 * @kbdev:     The kbase device structure of the device
 * @core_type: The type of core that the action should be performed on
 * @cores:     A bit mask of cores to perform the action on (low 32 bits)
 * @action:    The action to perform on the cores
 *
 * This function performs the action given by @action on a set of cores of a
 * type given by @core_type.
 */
static void kbase_pm_invoke(struct kbase_device *kbdev, enum kbase_pm_core_type core_type,
			    u64 cores, enum kbasep_pm_action action)
{
	u32 reg;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	reg = core_type_to_reg(kbdev, core_type, action);

#if !MALI_USE_CSF
	if (cores) {
		u64 state = kbase_pm_get_state(kbdev, core_type, ACTION_READY);

		if (action == ACTION_PWRON)
			state |= cores;
		else if (action == ACTION_PWROFF)
			state &= ~cores;

		KBASE_TLSTREAM_AUX_PM_STATE(kbdev, map_core_type_to_tl_pm_state(kbdev, core_type),
					    state);
	}

#endif
	/* Tracing */
	if (cores) {
		if (action == ACTION_PWRON)
			switch (core_type) {
			case KBASE_PM_CORE_SHADER:
				KBASE_KTRACE_ADD(kbdev, PM_PWRON, NULL, cores);
				break;
			case KBASE_PM_CORE_TILER:
				KBASE_KTRACE_ADD(kbdev, PM_PWRON_TILER, NULL, cores);
				break;
			case KBASE_PM_CORE_L2:
				KBASE_KTRACE_ADD(kbdev, PM_PWRON_L2, NULL, cores);
				break;
#if MALI_USE_CSF
			case KBASE_PM_CORE_NEURAL:
				KBASE_KTRACE_ADD(kbdev, PM_PWRON_NEURAL, NULL, cores);
				break;
#endif
			default:
				break;
			}
		else if (action == ACTION_PWROFF)
			switch (core_type) {
			case KBASE_PM_CORE_SHADER:
				KBASE_KTRACE_ADD(kbdev, PM_PWROFF, NULL, cores);
				break;
			case KBASE_PM_CORE_TILER:
				KBASE_KTRACE_ADD(kbdev, PM_PWROFF_TILER, NULL, cores);
				break;
			case KBASE_PM_CORE_L2:
				KBASE_KTRACE_ADD(kbdev, PM_PWROFF_L2, NULL, cores);
				/* disable snoops before L2 is turned off */
				kbase_pm_cache_snoop_disable(kbdev);
				break;
#if MALI_USE_CSF
			case KBASE_PM_CORE_NEURAL:
				KBASE_KTRACE_ADD(kbdev, PM_PWROFF_NEURAL, NULL, cores);
				break;
#endif
			default:
				break;
			}
	}

	if (kbase_dummy_job_wa_enabled(kbdev) && action == ACTION_PWRON &&
	    core_type == KBASE_PM_CORE_SHADER &&
	    !(kbdev->dummy_job_wa.flags & KBASE_DUMMY_JOB_WA_FLAG_LOGICAL_SHADER_POWER)) {
		kbase_dummy_job_wa_execute(kbdev, cores);
	}
#if MALI_USE_CSF
	else if (kbdev->pm.backend.has_host_pwr_iface) {
		kbase_reg_write64(kbdev, reg, cores);

		if (reg == HOST_POWER_ENUM(PWR_CMDARG)) {
			u64 pwr_cmd = pwr_cmd_constructor(kbdev, core_type, action);

			kbase_reg_write32(kbdev, HOST_POWER_ENUM(PWR_COMMAND), pwr_cmd);
		}
	}
#endif /* MALI_USE_CSF */
	else
		kbase_reg_write64(kbdev, reg, cores);
}

/**
 * kbase_pm_get_state - Get information about a core set
 *
 * @kbdev:     The kbase device structure of the device
 * @core_type: The type of core that the should be queried
 * @action:    The property of the cores to query
 *
 * This function gets information (chosen by @action) about a set of cores of
 * a type given by @core_type. It is a static function used by
 * kbase_pm_get_active_cores(), kbase_pm_get_trans_cores() and
 * kbase_pm_get_ready_cores().
 *
 * Return: A bit mask specifying the state of the cores
 */
static u64 kbase_pm_get_state(struct kbase_device *kbdev, enum kbase_pm_core_type core_type,
			      enum kbasep_pm_action action)
{
	u32 reg = core_type_to_reg(kbdev, core_type, action);

	return kbase_reg_read64(kbdev, reg);
}

/**
 * kbase_pm_get_present_cores - Get the cores that are present
 *
 * @kbdev: Kbase device
 * @core_type: The type of cores to query
 *
 * Return: Bitmask of the cores that are present
 */
u64 kbase_pm_get_present_cores(struct kbase_device *kbdev, enum kbase_pm_core_type core_type)
{
	if (WARN_ON(!kbdev))
		return 0;

	switch (core_type) {
	case KBASE_PM_CORE_L2:
		return kbdev->gpu_props.curr_config.l2_present;
	case KBASE_PM_CORE_SHADER:
		return kbdev->gpu_props.curr_config.shader_present;
	case KBASE_PM_CORE_TILER:
		return kbdev->gpu_props.tiler_present;
	case KBASE_PM_CORE_STACK:
		return kbdev->gpu_props.stack_present;
#if MALI_USE_CSF
	case KBASE_PM_CORE_NEURAL:
		return kbdev->gpu_props.neural_present;
	case KBASE_PM_CORE_BASE:
		return kbdev->gpu_props.base_present;
#endif
	default:
		break;
	}
	WARN_ON(1);

	return 0;
}

KBASE_EXPORT_TEST_API(kbase_pm_get_present_cores);

/**
 * kbase_pm_get_active_cores - Get the cores that are "active"
 *                             (busy processing work)
 *
 * @kbdev: Kbase device
 * @core_type: The type of cores to query
 *
 * Return: Bitmask of cores that are active
 */
u64 kbase_pm_get_active_cores(struct kbase_device *kbdev, enum kbase_pm_core_type core_type)
{
	return kbase_pm_get_state(kbdev, core_type, ACTION_PWRACTIVE);
}

KBASE_EXPORT_TEST_API(kbase_pm_get_active_cores);

/**
 * kbase_pm_get_trans_cores - Get the cores that are transitioning between
 *                            power states
 *
 * @kbdev: Kbase device
 * @core_type: The type of cores to query
 *
 * Return: Bitmask of cores that are transitioning
 */
u64 kbase_pm_get_trans_cores(struct kbase_device *kbdev, enum kbase_pm_core_type core_type)
{
	return kbase_pm_get_state(kbdev, core_type, ACTION_PWRTRANS);
}

KBASE_EXPORT_TEST_API(kbase_pm_get_trans_cores);

/**
 * kbase_pm_get_ready_cores - Get the cores that are powered on
 *
 * @kbdev: Kbase device
 * @core_type: The type of cores to query
 *
 * Return: Bitmask of cores that are ready (powered on)
 */
u64 kbase_pm_get_ready_cores(struct kbase_device *kbdev, enum kbase_pm_core_type core_type)
{
	u64 result;

	result = kbase_pm_get_state(kbdev, core_type, ACTION_READY);

	switch (core_type) {
	case KBASE_PM_CORE_SHADER:
		KBASE_KTRACE_ADD(kbdev, PM_CORES_POWERED, NULL, result);
		break;
	case KBASE_PM_CORE_TILER:
		KBASE_KTRACE_ADD(kbdev, PM_CORES_POWERED_TILER, NULL, result);
		break;
	case KBASE_PM_CORE_L2:
		KBASE_KTRACE_ADD(kbdev, PM_CORES_POWERED_L2, NULL, result);
		break;
#if MALI_USE_CSF
	case KBASE_PM_CORE_NEURAL:
		KBASE_KTRACE_ADD(kbdev, PM_CORES_POWERED_NEURAL, NULL, result);
		break;
#endif
	default:
		break;
	}

	return result;
}

KBASE_EXPORT_TEST_API(kbase_pm_get_ready_cores);

static void kbase_pm_trigger_hwcnt_disable(struct kbase_device *kbdev)
{
	struct kbase_pm_backend_data *backend = &kbdev->pm.backend;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	/* See if we can get away with disabling hwcnt
	 * atomically, otherwise kick off a worker.
	 */
	if (kbase_hwcnt_context_disable_atomic(kbdev->hwcnt_gpu_ctx)) {
		backend->hwcnt_disabled = true;

	} else {
		kbase_hwcnt_context_queue_work(kbdev->hwcnt_gpu_ctx, &backend->hwcnt_disable_work);
	}
}

static void kbase_pm_l2_config_override(struct kbase_device *kbdev)
{
	u32 val;

	/*
	 * Skip if it is not supported
	 */
	if (!kbase_hw_has_feature(kbdev, KBASE_HW_FEATURE_L2_CONFIG))
		return;

#if MALI_USE_CSF
	if (kbase_hw_has_feature(kbdev, KBASE_HW_FEATURE_PBHA_HWU)) {
		val = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(L2_CONFIG));
		kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(L2_CONFIG),
				  L2_CONFIG_PBHA_HWU_SET(val, kbdev->pbha_propagate_bits));
	}
#endif /* MALI_USE_CSF */

	/*
	 * Skip if size and hash are not given explicitly,
	 * which means default values are used.
	 */
	if ((kbdev->l2_size_override == 0) && (kbdev->l2_hash_override == 0) &&
	    (!kbdev->l2_hash_values_override))
		return;

	val = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(L2_CONFIG));

	if (kbdev->l2_size_override) {
		val &= ~L2_CONFIG_SIZE_MASK;
		val |= (kbdev->l2_size_override << L2_CONFIG_SIZE_SHIFT);
	}

	if (kbdev->l2_hash_override) {
		WARN_ON(kbase_hw_has_l2_slice_hash_feature(kbdev));
		val &= ~L2_CONFIG_HASH_MASK;
		val |= (kbdev->l2_hash_override << L2_CONFIG_HASH_SHIFT);
	} else if (kbdev->l2_hash_values_override) {
#if MALI_USE_CSF
		uint i;

		WARN_ON(!kbase_hw_has_l2_slice_hash_feature(kbdev));

		val &= ~L2_CONFIG_L2_SLICE_HASH_ENABLE_MASK;
		val |= (0x1 << L2_CONFIG_L2_SLICE_HASH_ENABLE_SHIFT);
		for (i = 0; i < GPU_L2_SLICE_HASH_COUNT; i++) {
			/* L2_SLICE_HASH, L2C_SLICE_HASH and ASN_HASH alias each other */
			if (kbdev->gpu_props.gpu_id.arch_id >= GPU_ID_ARCH_MAKE(14, 8, 4))
				dev_dbg(kbdev->dev, "Program 0x%x to L2C_SLICE_HASH[%u]\n",
					kbdev->l2_hash_values[i], i);
			else
				dev_dbg(kbdev->dev, "Program 0x%x to ASN_HASH[%u]\n",
					kbdev->l2_hash_values[i], i);
			kbase_reg_write32(kbdev, GPU_L2_SLICE_HASH_OFFSET(i),
					  kbdev->l2_hash_values[i]);
		}
#endif /* MALI_USE_CSF */
	}

	dev_dbg(kbdev->dev, "Program 0x%x to L2_CONFIG\n", val);
	kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(L2_CONFIG), val);
}

static void kbase_pm_control_gpu_clock(struct kbase_device *kbdev)
{
	struct kbase_pm_backend_data *const backend = &kbdev->pm.backend;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	queue_work(system_wq, &backend->gpu_clock_control_work);
}

#if MALI_USE_CSF
static const char *kbase_mcu_state_to_string(enum kbase_mcu_state state)
{
	const char *const strings[] = {
#define KBASEP_MCU_STATE(n) #n,
#include "mali_kbase_pm_mcu_states.h"
#undef KBASEP_MCU_STATE
	};
	if (WARN_ON((size_t)state >= ARRAY_SIZE(strings)))
		return "Bad MCU state";
	else
		return strings[state];
}

static void kbase_ktrace_log_mcu_state(struct kbase_device *kbdev, enum kbase_mcu_state state)
{
#if KBASE_KTRACE_ENABLE
	switch (state) {
#define KBASEP_MCU_STATE(n)                                       \
	case KBASE_MCU_##n:                                       \
		KBASE_KTRACE_ADD(kbdev, PM_MCU_##n, NULL, state); \
		break;
#include "mali_kbase_pm_mcu_states.h"
#undef KBASEP_MCU_STATE
	}
#else
	CSTD_UNUSED(kbdev);
	CSTD_UNUSED(state);
#endif
}

static inline bool kbase_pm_handle_mcu_core_attr_update(struct kbase_device *kbdev)
{
	struct kbase_pm_backend_data *backend = &kbdev->pm.backend;
	bool timer_update;
	bool core_mask_update;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	WARN_ON(backend->mcu_state != KBASE_MCU_ON);

	/* This function is only for cases where the MCU managing Cores, if
	 * the firmware mode is with host control, do nothing here.
	 */
	if (unlikely(kbdev->csf.firmware_hctl_core_pwr))
		return false;

	core_mask_update = backend->shaders_avail != backend->shaders_desired_mask;

	timer_update = kbdev->csf.mcu_core_pwroff_dur_count !=
		       kbdev->csf.mcu_core_pwroff_reg_shadow;

	if (core_mask_update || timer_update)
		kbase_csf_firmware_update_core_attr(kbdev, timer_update, core_mask_update,
						    backend->shaders_desired_mask);

	return (core_mask_update || timer_update);
}

bool kbase_pm_is_mcu_inactive(struct kbase_device *kbdev, enum kbase_mcu_state state)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);

	return ((state == KBASE_MCU_OFF) || (state == KBASE_MCU_IN_SLEEP));
}

#ifdef KBASE_PM_RUNTIME

void kbase_pm_enable_mcu_db_notification(struct kbase_device *kbdev)
{
	u32 val = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(MCU_CONTROL));

	lockdep_assert_held(&kbdev->hwaccess_lock);

	val &= ~MCU_CNTRL_DOORBELL_DISABLE_MASK;
	kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(MCU_CONTROL), val);
}

static int wait_mcu_active(struct kbase_device *kbdev)
{
	int err = 0;
	const u64 timeout_us =
		kbase_get_timeout_ms(kbdev, CSF_FIRMWARE_WAKE_UP_TIMEOUT) * USEC_PER_MSEC;

	if (kbdev->pm.backend.has_host_pwr_iface) {
		u32 glb_ack;
		err = read_poll_timeout_atomic(kbase_csf_fw_io_global_read, glb_ack,
					       GLB_ACK_STATE_GET(glb_ack) == GLB_ACK_STATE_ACTIVE,
					       1, timeout_us, true, &kbdev->csf.fw_io, GLB_ACK);
		if (unlikely(err))
			dev_warn(kbdev->dev, "GLB_ACK_STATE didn't change to active");
	}

	return err;
}

static void handle_sleep_initiate_state(struct kbase_device *kbdev)
{
	struct kbase_pm_backend_data *backend = &kbdev->pm.backend;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	if (likely(test_bit(KBASE_GPU_SUPPORTS_FW_SLEEP_ON_IDLE, &backend->gpu_sleep_allowed))) {
		bool db_notif_disabled = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(MCU_CONTROL)) &
					 MCU_CNTRL_DOORBELL_DISABLE_MASK;

		if (atomic_read(&kbdev->csf.scheduler.gpu_idle_timer_enabled) &&
		    atomic_read(&kbdev->csf.scheduler.fw_soi_enabled)) {
			if (unlikely(!db_notif_disabled))
				goto pend_soi_sleep;
			backend->mcu_state = KBASE_MCU_ON_PEND_SLEEP;
			return;
		}

		WARN_ON_ONCE(db_notif_disabled);
		/* Wait for FW to be active (in case it was waking up
		 * from sleep) before sending the SLEEP request.
		 * If FW won't wake up after this time, we will cancel
		 * the sleep attempt. Transitioning to
		 * ON_PEND_SOI_SLEEP state allows us to roll back.
		 */
		if (wait_mcu_active(kbdev))
			goto pend_soi_sleep;
	}

	/* SoI is disabled or unsupported, so send a sleep request to FW.*/
	kbase_csf_firmware_trigger_mcu_sleep(kbdev);
	backend->mcu_state = KBASE_MCU_ON_PEND_SLEEP;
	return;
pend_soi_sleep:
	backend->exit_gpu_sleep_mode = true;
	wake_up(&backend->gpu_in_desired_state_wait);
	kbase_csf_scheduler_invoke_tick(kbdev);
	backend->mcu_state = KBASE_MCU_ON_PEND_SOI_SLEEP;
}

/**
 * wait_mcu_as_inactive - Wait for AS used by MCU FW to get configured
 *
 * @kbdev: Pointer to the device.
 *
 * This function is called to wait for the AS used by MCU FW to get configured
 * before DB notification on MCU is enabled, as a workaround for HW issue.
 */
static void wait_mcu_as_inactive(struct kbase_device *kbdev)
{
	u32 val;
	int err;
	const u32 timeout_us =
		kbase_get_timeout_ms(kbdev, KBASE_AS_INACTIVE_TIMEOUT) * USEC_PER_MSEC;
	lockdep_assert_held(&kbdev->hwaccess_lock);

	if (!kbase_hw_has_issue(kbdev, KBASE_HW_ISSUE_TURSEHW_2716))
		return;

	/* Wait for the AS_ACTIVE_INT bit to become 0 for the AS used by MCU FW */
	err = kbase_reg_poll32_timeout(kbdev, MMU_AS_OFFSET(MCU_AS_NR, STATUS), val,
				       !(val & AS_STATUS_AS_ACTIVE_INT_MASK), 10, timeout_us,
				       false);
	if (!WARN_ON_ONCE(err == -ETIMEDOUT))
		return;

	dev_err(kbdev->dev, "AS_ACTIVE_INT bit stuck for AS %d used by MCU FW", MCU_AS_NR);

	if (kbase_prepare_to_reset_gpu(kbdev, 0))
		kbase_reset_gpu(kbdev);
}
#endif

/**
 * kbasep_pm_toggle_power_interrupt - Toggles the IRQ mask for power interrupts
 *                                    from the firmware
 *
 * @kbdev:  Pointer to the device
 * @enable: boolean indicating to enable interrupts or not
 *
 * The POWER_CHANGED_ALL interrupt can be disabled after L2 has been turned on
 * when FW is controlling the power for the shader cores. Correspondingly, the
 * interrupts can be re-enabled after the MCU has been disabled before the
 * power down of L2.
 */
static void kbasep_pm_toggle_power_interrupt(struct kbase_device *kbdev, bool enable)
{
	u32 irq_mask;

	lockdep_assert_held(&kbdev->hwaccess_lock);

#if MALI_USE_CSF
	/* No toggling is needed when Host control power interface is there, as PM actions
	 * done by the firmware for Tiler, shader, neural won't generate the POWER_CHANGED
	 * irq on Host side.
	 */
	if (kbdev->pm.backend.has_host_pwr_iface)
		return;
#endif

	irq_mask = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(GPU_IRQ_MASK));

	if (enable) {
		irq_mask |= POWER_CHANGED_ALL;
		kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_IRQ_CLEAR), POWER_CHANGED_ALL);
	} else {
		irq_mask &= ~POWER_CHANGED_ALL;
	}

	kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_IRQ_MASK), irq_mask);
}

#if MALI_USE_CSF
/**
 * hctl_neural_engines_active - Check the neural engines are active
 *
 * @kbdev:         Pointer to the device
 *
 * This function is called to check if the required neural engines are active, after
 * shading engines became inactive. Check is done for the neural engine on all the
 * shader cores are not desired.
 *
 * Return: true if required neural engines are present and are active, otherwise false.
 */
static bool hctl_neural_engines_active(struct kbase_device *kbdev)
{
	u64 engines_active, engines_to_disable, engines_ready;

	if (!kbase_csf_dev_has_ne(kbdev))
		return false;

	engines_active = kbase_pm_get_active_cores(kbdev, KBASE_PM_CORE_NEURAL);
	engines_ready = kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_NEURAL);
	engines_to_disable = engines_ready & ~kbdev->pm.backend.shaders_desired_mask;

	return !!(engines_to_disable & engines_active);
}

/**
 * hctl_neural_power_up_done - Trigger the power up of neural engines
 *
 * @kbdev:         Pointer to the device
 * @shaders_ready: Bitmask of shader cores (shading engines) that are ready.
 *
 * This function is called to initiate the power up of neural engines when Host
 * controls the power for shader cores. Power up is done for the neural engine
 * on all the shader cores that are in ready state.
 *
 * Return: true if power up is complete for required neural engines or there are
 *         no neural engines present, otherwise false.
 */
static bool hctl_neural_power_up_done(struct kbase_device *kbdev, u64 shaders_ready)
{
	u64 neural_trans, neural_ready;

	if (!kbase_csf_dev_has_ne(kbdev))
		return true;

	neural_trans = kbase_pm_get_trans_cores(kbdev, KBASE_PM_CORE_NEURAL);
	if (neural_trans)
		return false;

	WARN_ON_ONCE(!shaders_ready);
	neural_ready = kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_NEURAL);
	if (neural_ready != shaders_ready) {
		kbase_pm_invoke(kbdev, KBASE_PM_CORE_NEURAL, ~neural_ready & shaders_ready,
				ACTION_PWRON);
		return false;
	}

	return true;
}

/**
 * hctl_neural_power_down_done - Trigger the power down of neural engines
 *
 * @kbdev:         Pointer to the device
 * @shaders_ready: Bitmask of shader cores (shading engines) that are ready.
 *
 * This function is called to initiate the power down of neural engines when Host
 * controls the power for shader cores. Power down is done for the neural engine
 * on all the shader cores that do not have the shading engine in ready state.
 *
 * Return: true if power down is complete for required neural engines or there are
 *         no neural engines present, otherwise false.
 */
static bool hctl_neural_power_down_done(struct kbase_device *kbdev, u64 shaders_ready)
{
	u64 neural_trans, neural_ready;

	if (!kbase_csf_dev_has_ne(kbdev))
		return true;

	neural_trans = kbase_pm_get_trans_cores(kbdev, KBASE_PM_CORE_NEURAL);
	if (neural_trans)
		return false;

	neural_ready = kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_NEURAL);
	if (neural_ready != shaders_ready) {
		kbase_pm_invoke(kbdev, KBASE_PM_CORE_NEURAL, neural_ready & ~shaders_ready,
				ACTION_PWROFF);
		return false;
	}

	return true;
}

/**
 * hctl_base_power_down_done - Check the shader core base domains are powered down
 *
 * @kbdev:         Pointer to the device
 * @shaders_ready: Bitmask of shader cores (shading engines) that are ready.
 *
 * This function is called to check if the required shader core base domains are
 * powered down. A shader core base domain is expected to be powered down automatically
 * when both shading and neural engine have been powered down.
 *
 * Return: true if required shader core base domains are powered down, otherwise false.
 */
static bool hctl_base_power_down_done(struct kbase_device *kbdev, u64 shaders_ready)
{
	u64 base_trans, base_ready;

	if (!kbdev->pm.backend.has_host_pwr_iface)
		return true;

	base_trans = kbase_pm_get_trans_cores(kbdev, KBASE_PM_CORE_BASE);
	base_ready = kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_BASE);

	return (!base_trans && base_ready == shaders_ready);
}
#endif /* MALI_USE_CSF */

#if MALI_USE_CSF
/**
 * hctl_shader_cores_active - Check the shader cores are active
 *
 * @kbdev:         Pointer to the device.
 * @shaders_ready: Bitmask of shader cores that are ready.
 *
 * This function is called to check if the required shader cores are active.
 *
 * Return: true if required shader cores are active, otherwise false.
 */
static bool hctl_shader_cores_active(struct kbase_device *kbdev, u64 shaders_ready)
{
	const u64 shaders_active = kbase_pm_get_active_cores(kbdev, KBASE_PM_CORE_SHADER);
	const u64 shaders_to_disable = shaders_ready & ~kbdev->pm.backend.shaders_desired_mask;
	bool cores_are_active = !!(shaders_to_disable & shaders_active);

	/* Shading engine is inactive in all the required shader cores, but also need
	 * to check for the neural engine to be inactive in all those shader cores.
	 */
	cores_are_active = cores_are_active || hctl_neural_engines_active(kbdev);

	return cores_are_active;
}

/**
 * hctl_shader_cores_power_up_done - Check the shader cores are powered up.
 *
 * @kbdev:         Pointer to the device
 * @shaders_ready: Bitmask of shader cores that are ready.
 * @shaders_trans: Bitmask of shader cores that are transitioning.
 * @shaders_avail: Bitmask of shader cores that are allowed to be used.
 *
 * This function is called to check if the required number of shader cores have been
 * powered up.
 *
 * Return: true if power up is complete for required shader cores, otherwise false.
 */
static bool hctl_shader_cores_power_up_done(struct kbase_device *kbdev, u64 shaders_ready,
					    u64 shaders_trans, u64 shaders_avail)
{
	if (shaders_trans || shaders_ready != shaders_avail)
		return false;

	/* Shading engine is powered up in all the required shader cores, but also need
	 * to trigger the power up of neural engine present in all those shader cores.
	 */
	if (!hctl_neural_power_up_done(kbdev, shaders_ready))
		return false;

	return true;
}

/*
 * hctl_shader_cores_power_down_done - Check the shader cores are powered down
 *
 * @kbdev:         Pointer to the device
 * @shaders_ready: Bitmask of shader cores that are ready.
 * @shaders_trans: Bitmask of shader cores that are transitioning.
 * @shaders_avail: Bitmask of shader cores that are allowed to be used.
 *
 * This function is called to check if the required number of shader cores have been
 * powered down.
 *
 * Return: true if power down is complete for required shader cores, otherwise false.
 */
static bool hctl_shader_cores_power_down_done(struct kbase_device *kbdev, u64 shaders_ready,
					      u64 shaders_trans, u64 shaders_avail)
{
	if (shaders_trans || shaders_ready != shaders_avail)
		return false;

	/* Shading engine is powered down in all the required shader cores, but also need
	 * to trigger the power down of neural engine present in all those shader cores.
	 * Also need to wait for the (automatic) power down of shader core base domain.
	 */
	if (!hctl_neural_power_down_done(kbdev, shaders_ready) ||
	    !hctl_base_power_down_done(kbdev, shaders_ready))
		return false;

	return true;
}
#endif /* MALI_USE_CSF */

static void disable_gpu_idle_timer_no_db(struct kbase_device *kbdev)
{
	unsigned long flags;

	if (!atomic_read(&kbdev->csf.scheduler.gpu_idle_timer_enabled))
		return;

	kbase_csf_fw_io_open_force(&kbdev->csf.fw_io, &flags);
	kbase_csf_fw_io_global_write_mask(&kbdev->csf.fw_io, GLB_REQ, GLB_REQ_REQ_IDLE_DISABLE,
					  GLB_REQ_IDLE_DISABLE_MASK);
	kbase_csf_fw_io_close(&kbdev->csf.fw_io, flags);
	atomic_set(&kbdev->csf.scheduler.gpu_idle_timer_enabled, false);
}

static int kbase_pm_mcu_update_state(struct kbase_device *kbdev)
{
	struct kbase_pm_backend_data *backend = &kbdev->pm.backend;
	enum kbase_mcu_state prev_state;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	/*
	 * Initial load of firmware should have been done to
	 * exercise the MCU state machine.
	 */
	if (unlikely(!kbdev->csf.firmware_inited)) {
		WARN_ON(backend->mcu_state != KBASE_MCU_OFF);
		return 0;
	}

	do {
		u64 shaders_trans = kbase_pm_get_trans_cores(kbdev, KBASE_PM_CORE_SHADER);
		u64 shaders_ready = kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_SHADER);

		/* This check mirrors the check inside kbase_pm_l2_update_state(). */
		if (unlikely(!kbase_io_has_gpu(kbdev))) {
			backend->hwcnt_desired = false;
			if (!backend->hwcnt_disabled) {
				backend->mcu_state = KBASE_MCU_ON_HWCNT_DISABLE;
				KBASE_KTRACE_ADD(kbdev, PM_MCU_ON_HWCNT_DISABLE, NULL,
						 backend->mcu_state);
				kbase_pm_trigger_hwcnt_disable(kbdev);
			}

			if (backend->hwcnt_disabled && (backend->mcu_state != KBASE_MCU_OFF)) {
				backend->mcu_state = KBASE_MCU_OFF;
				KBASE_KTRACE_ADD(kbdev, PM_MCU_OFF, NULL, backend->mcu_state);
				dev_dbg(kbdev->dev, "GPU lost has occurred - MCU off");
			}
			break;
		}

		/* mask off ready from trans in case transitions finished
		 * between the register reads
		 */
		shaders_trans &= ~shaders_ready;

		prev_state = backend->mcu_state;

		switch (backend->mcu_state) {
		case KBASE_MCU_OFF:
			if (kbase_pm_is_mcu_desired(kbdev) &&
			    !backend->policy_change_clamp_state_to_off &&
			    backend->l2_state == KBASE_L2_ON) {
				kbdev->csf.mcu_halted = false;

				/* Ensure that FW would not go to sleep immediately after
				 * resumption.
				 */
				disable_gpu_idle_timer_no_db(kbdev);

				kbase_csf_firmware_trigger_reload(kbdev);
				backend->mcu_state = KBASE_MCU_PEND_ON_RELOAD;
			}
			break;

		case KBASE_MCU_PEND_ON_RELOAD:
			if (kbdev->csf.firmware_reloaded) {
				backend->shaders_desired_mask = kbase_pm_ca_get_core_mask(kbdev);
				kbase_csf_firmware_global_reinit(kbdev,
								 backend->shaders_desired_mask);
				if (!kbdev->csf.firmware_hctl_core_pwr)
					kbasep_pm_toggle_power_interrupt(kbdev, false);
				backend->mcu_state = KBASE_MCU_ON_GLB_REINIT_PEND;
			}
			break;

		case KBASE_MCU_ON_GLB_REINIT_PEND:
			if (kbase_csf_firmware_global_reinit_complete(kbdev)) {
				backend->shaders_avail = backend->shaders_desired_mask;
				backend->pm_shaders_core_mask = 0;
				if (kbdev->csf.firmware_hctl_core_pwr) {
					kbase_pm_invoke(kbdev, KBASE_PM_CORE_SHADER,
							backend->shaders_avail, ACTION_PWRON);
					backend->mcu_state = KBASE_MCU_HCTL_SHADERS_PEND_ON;
				} else
					backend->mcu_state = KBASE_MCU_ON_HWCNT_ENABLE;
#if IS_ENABLED(CONFIG_MALI_CORESIGHT)
				if (kbase_debug_coresight_csf_state_check(
					    kbdev, KBASE_DEBUG_CORESIGHT_CSF_DISABLED)) {
					kbase_debug_coresight_csf_state_request(
						kbdev, KBASE_DEBUG_CORESIGHT_CSF_ENABLED);
					backend->mcu_state = KBASE_MCU_CORESIGHT_ENABLE;
				} else if (kbase_debug_coresight_csf_state_check(
						   kbdev, KBASE_DEBUG_CORESIGHT_CSF_ENABLED)) {
					backend->mcu_state = KBASE_MCU_CORESIGHT_ENABLE;
				}
#endif /* IS_ENABLED(CONFIG_MALI_CORESIGHT) */
			}
			break;

		case KBASE_MCU_HCTL_SHADERS_PEND_ON:
			if (!hctl_shader_cores_power_up_done(kbdev, shaders_ready, shaders_trans,
							     backend->shaders_avail))
				break;

			/* Cores now stable, notify MCU the stable mask */
			kbase_csf_firmware_update_core_attr(kbdev, false, true, shaders_ready);

			backend->pm_shaders_core_mask = shaders_ready;
			backend->mcu_state = KBASE_MCU_HCTL_CORES_NOTIFY_PEND;
			break;

		case KBASE_MCU_HCTL_CORES_NOTIFY_PEND:
			/* Wait for the acknowledgement */
			if (kbase_csf_firmware_core_attr_updated(kbdev))
				backend->mcu_state = KBASE_MCU_ON_HWCNT_ENABLE;
			break;

		case KBASE_MCU_ON_HWCNT_ENABLE:
			backend->hwcnt_desired = true;
			if (backend->hwcnt_disabled) {
				unsigned long flags;

				kbase_csf_scheduler_spin_lock(kbdev, &flags);
				kbase_hwcnt_backend_csf_set_hw_availability(
					&kbdev->hwcnt_gpu_iface,
					kbdev->gpu_props.curr_config.l2_slices,
					kbdev->gpu_props.curr_config.shader_present,
					kbdev->pm.debug_core_mask);
				kbase_hwcnt_context_enable(kbdev->hwcnt_gpu_ctx);
				kbase_csf_scheduler_spin_unlock(kbdev, flags);
				backend->hwcnt_disabled = false;
			}
			backend->mcu_state = KBASE_MCU_ON;
			break;

		case KBASE_MCU_ON:
			backend->shaders_desired_mask = kbase_pm_ca_get_core_mask(kbdev);
			if (!kbase_pm_is_mcu_desired(kbdev))
				backend->mcu_state = KBASE_MCU_ON_HWCNT_DISABLE;
			else if (kbdev->csf.firmware_hctl_core_pwr) {
				/* Host control scale up/down cores as needed */
				if (backend->shaders_desired_mask != shaders_ready) {
					backend->hwcnt_desired = false;
					if (!backend->hwcnt_disabled)
						kbase_pm_trigger_hwcnt_disable(kbdev);
					backend->mcu_state = KBASE_MCU_HCTL_MCU_ON_RECHECK;
				}
			} else if (kbase_pm_handle_mcu_core_attr_update(kbdev))
				backend->mcu_state = KBASE_MCU_ON_CORE_ATTR_UPDATE_PEND;
#if IS_ENABLED(CONFIG_MALI_CORESIGHT)
			else if (kbdev->csf.coresight.disable_on_pmode_enter) {
				kbase_debug_coresight_csf_state_request(
					kbdev, KBASE_DEBUG_CORESIGHT_CSF_DISABLED);
				backend->mcu_state = KBASE_MCU_ON_PMODE_ENTER_CORESIGHT_DISABLE;
			} else if (kbdev->csf.coresight.enable_on_pmode_exit) {
				kbase_debug_coresight_csf_state_request(
					kbdev, KBASE_DEBUG_CORESIGHT_CSF_ENABLED);
				backend->mcu_state = KBASE_MCU_ON_PMODE_EXIT_CORESIGHT_ENABLE;
			}
#endif
			break;

		case KBASE_MCU_HCTL_MCU_ON_RECHECK:
			backend->shaders_desired_mask = kbase_pm_ca_get_core_mask(kbdev);

			if (!backend->hwcnt_disabled) {
				/* Wait for being disabled */
				;
			} else if (!kbase_pm_is_mcu_desired(kbdev)) {
				/* Converging to MCU powering down flow */
				backend->mcu_state = KBASE_MCU_ON_HWCNT_DISABLE;
			} else if (backend->shaders_desired_mask & ~shaders_ready) {
				/* set cores ready but not available to
				 * meet SHADERS_PEND_ON check pass
				 */
				backend->shaders_avail =
					(backend->shaders_desired_mask | shaders_ready);

				kbase_pm_invoke(kbdev, KBASE_PM_CORE_SHADER,
						backend->shaders_avail & ~shaders_ready,
						ACTION_PWRON);
				backend->mcu_state = KBASE_MCU_HCTL_SHADERS_PEND_ON;

			} else if (~backend->shaders_desired_mask & shaders_ready) {
				kbase_csf_firmware_update_core_attr(kbdev, false, true,
								    backend->shaders_desired_mask);
				backend->mcu_state = KBASE_MCU_HCTL_CORES_DOWN_SCALE_NOTIFY_PEND;
			} else {
				backend->mcu_state = KBASE_MCU_HCTL_SHADERS_PEND_ON;
			}
			break;

		case KBASE_MCU_HCTL_CORES_DOWN_SCALE_NOTIFY_PEND:
			if (kbase_csf_firmware_core_attr_updated(kbdev)) {
				/* wait in queue until cores idle */
				queue_work(backend->core_idle_wq, &backend->core_idle_work);
				backend->mcu_state = KBASE_MCU_HCTL_CORE_INACTIVE_PEND;
			}
			break;

		case KBASE_MCU_HCTL_CORE_INACTIVE_PEND: {
			if (hctl_shader_cores_active(kbdev, shaders_ready))
				break;

			kbase_pm_invoke(kbdev, KBASE_PM_CORE_SHADER,
					shaders_ready & ~backend->shaders_desired_mask,
					ACTION_PWROFF);
			backend->shaders_avail = backend->shaders_desired_mask;
			backend->mcu_state = KBASE_MCU_HCTL_SHADERS_CORE_OFF_PEND;
		} break;

		case KBASE_MCU_HCTL_SHADERS_CORE_OFF_PEND:
			if (!hctl_shader_cores_power_down_done(kbdev, shaders_ready, shaders_trans,
							       backend->shaders_avail))
				break;

			/* Cores now stable */
			backend->pm_shaders_core_mask = shaders_ready;
			backend->mcu_state = KBASE_MCU_ON_HWCNT_ENABLE;
			break;

		case KBASE_MCU_ON_CORE_ATTR_UPDATE_PEND:
			if (kbase_csf_firmware_core_attr_updated(kbdev)) {
				backend->shaders_avail = backend->shaders_desired_mask;
				backend->mcu_state = KBASE_MCU_ON;
			}
			break;

		case KBASE_MCU_ON_HWCNT_DISABLE:
			if (kbase_pm_is_mcu_desired(kbdev)) {
				backend->mcu_state = KBASE_MCU_ON_HWCNT_ENABLE;
				break;
			}

			backend->hwcnt_desired = false;
			if (!backend->hwcnt_disabled)
				kbase_pm_trigger_hwcnt_disable(kbdev);

			if (backend->hwcnt_disabled) {
#ifdef KBASE_PM_RUNTIME
				if (backend->gpu_sleep_mode_active)
					backend->mcu_state = KBASE_MCU_ON_SLEEP_INITIATE;
				else {
#endif
					backend->mcu_state = KBASE_MCU_ON_HALT;
#if IS_ENABLED(CONFIG_MALI_CORESIGHT)
					kbase_debug_coresight_csf_state_request(
						kbdev, KBASE_DEBUG_CORESIGHT_CSF_DISABLED);
					backend->mcu_state = KBASE_MCU_CORESIGHT_DISABLE;
#endif /* IS_ENABLED(CONFIG_MALI_CORESIGHT) */
				}
			}
			break;

#if IS_ENABLED(CONFIG_MALI_CORESIGHT)
		case KBASE_MCU_ON_PMODE_ENTER_CORESIGHT_DISABLE:
			if (kbase_debug_coresight_csf_state_check(
				    kbdev, KBASE_DEBUG_CORESIGHT_CSF_DISABLED)) {
				backend->mcu_state = KBASE_MCU_ON;
				kbdev->csf.coresight.disable_on_pmode_enter = false;
			}
			break;
		case KBASE_MCU_ON_PMODE_EXIT_CORESIGHT_ENABLE:
			if (kbase_debug_coresight_csf_state_check(
				    kbdev, KBASE_DEBUG_CORESIGHT_CSF_ENABLED)) {
				backend->mcu_state = KBASE_MCU_ON;
				kbdev->csf.coresight.enable_on_pmode_exit = false;
			}
			break;
		case KBASE_MCU_CORESIGHT_DISABLE:
			if (kbase_debug_coresight_csf_state_check(
				    kbdev, KBASE_DEBUG_CORESIGHT_CSF_DISABLED))
				backend->mcu_state = KBASE_MCU_ON_HALT;
			break;

		case KBASE_MCU_CORESIGHT_ENABLE:
			if (kbase_debug_coresight_csf_state_check(
				    kbdev, KBASE_DEBUG_CORESIGHT_CSF_ENABLED))
				backend->mcu_state = KBASE_MCU_ON_HWCNT_ENABLE;
			break;
#endif /* IS_ENABLED(CONFIG_MALI_CORESIGHT) */

		case KBASE_MCU_ON_HALT:
			if (!kbase_pm_is_mcu_desired(kbdev)) {
				/* Ensure that the MCU would be active before
				 * sending the request, in case it has not
				 * finished booting/waking up.
				 */
				wait_mcu_active(kbdev);
				kbase_csf_firmware_trigger_mcu_halt(kbdev);
				backend->mcu_state = KBASE_MCU_ON_PEND_HALT;
			} else
				backend->mcu_state = KBASE_MCU_ON_HWCNT_ENABLE;
			break;

		case KBASE_MCU_ON_PEND_HALT:
			if (kbase_csf_firmware_mcu_halt_req_complete(kbdev)) {
				KBASE_KTRACE_ADD(kbdev, CSF_FIRMWARE_MCU_HALTED, NULL,
						 kbase_csf_ktrace_gpu_cycle_cnt(kbdev));
				if (kbdev->csf.firmware_hctl_core_pwr)
					backend->mcu_state = KBASE_MCU_HCTL_SHADERS_READY_OFF;
				else
					backend->mcu_state = KBASE_MCU_POWER_DOWN;
			}
			break;

		case KBASE_MCU_HCTL_SHADERS_READY_OFF:
			kbase_pm_invoke(kbdev, KBASE_PM_CORE_SHADER, shaders_ready, ACTION_PWROFF);
			backend->mcu_state = KBASE_MCU_HCTL_SHADERS_PEND_OFF;
			break;

		case KBASE_MCU_HCTL_SHADERS_PEND_OFF:
			if (!hctl_shader_cores_power_down_done(kbdev, shaders_ready, shaders_trans,
							       0))
				break;

			backend->pm_shaders_core_mask = 0;
			backend->mcu_state = KBASE_MCU_POWER_DOWN;
			break;

		case KBASE_MCU_POWER_DOWN:
			if (kbase_hw_has_issue(kbdev, KBASE_HW_ISSUE_TITANHW_2922)) {
				if (!kbdev->csf.firmware_hctl_core_pwr)
					kbasep_pm_toggle_power_interrupt(kbdev, true);
				backend->mcu_state = KBASE_MCU_OFF;
				backend->l2_force_off_after_mcu_halt = true;
			} else {
				kbase_csf_firmware_disable_mcu(kbdev);
				backend->mcu_state = KBASE_MCU_PEND_OFF;
			}
			break;

		case KBASE_MCU_PEND_OFF:
			/* wait synchronously for the MCU to get disabled */
			kbase_csf_firmware_disable_mcu_wait(kbdev);
			if (!kbdev->csf.firmware_hctl_core_pwr)
				kbasep_pm_toggle_power_interrupt(kbdev, true);
			backend->mcu_state = KBASE_MCU_OFF;
			break;
#ifdef KBASE_PM_RUNTIME
		case KBASE_MCU_ON_SLEEP_INITIATE:
			if (!kbase_pm_is_mcu_desired(kbdev))
				handle_sleep_initiate_state(kbdev);
			else
				backend->mcu_state = KBASE_MCU_ON_HWCNT_ENABLE;
			break;

		case KBASE_MCU_ON_PEND_SOI_SLEEP:
			if (kbase_pm_is_mcu_desired(kbdev)) {
				/* Assume the transition is complete and prepare to goto ON state */
				WARN_ON_ONCE(backend->l2_state != KBASE_L2_ON);
				backend->mcu_state = KBASE_MCU_IN_SLEEP;
				break;
			}
			fallthrough;

		case KBASE_MCU_ON_PEND_SLEEP:
			if (kbase_csf_firmware_is_mcu_in_sleep(kbdev)) {
				KBASE_KTRACE_ADD(kbdev, CSF_FIRMWARE_MCU_SLEEP, NULL,
						 kbase_csf_ktrace_gpu_cycle_cnt(kbdev));
				backend->mcu_state = KBASE_MCU_IN_SLEEP;
				kbase_pm_enable_db_mirror_interrupt(kbdev);
				kbase_csf_scheduler_reval_idleness_post_sleep(kbdev);
				/* Enable PM interrupt, after MCU has been put
				 * to sleep, for the power down of L2.
				 */
				if (!kbdev->csf.firmware_hctl_core_pwr)
					kbasep_pm_toggle_power_interrupt(kbdev, true);
				/* If PM refcount is not zero then the early wakeup
				 * on reaching the sleep state can be skipped as the waiting thread
				 * (like Scheduler kthread) would be interested in MCU being
				 * turned ON.
				 * In the more regular flow, the refcount is very likely to be zero
				 * and there would be no waiters. The wake_up() call won't have an
				 * effect if there are no waiters.
				 */
				if (likely(!kbdev->pm.active_count))
					wake_up(&backend->gpu_in_desired_state_wait);
			}
			break;

		case KBASE_MCU_IN_SLEEP:
			if (kbase_pm_is_mcu_desired(kbdev) && backend->l2_state == KBASE_L2_ON) {
				wait_mcu_as_inactive(kbdev);

				/* Ensure that FW would not go to sleep immediately after
				 * resumption.
				 */
				disable_gpu_idle_timer_no_db(kbdev);

				KBASE_TLSTREAM_TL_KBASE_CSFFW_FW_REQUEST_WAKEUP(
					kbdev, kbase_backend_get_cycle_cnt(kbdev));
				/* Set the state to ACTIVE before waking up MCU */
				if (kbdev->pm.backend.has_host_pwr_iface)
					kbase_csf_firmware_set_glb_state_active(kbdev);
				kbase_pm_enable_mcu_db_notification(kbdev);
				kbase_pm_disable_db_mirror_interrupt(kbdev);
				/* Disable PM interrupt after L2 has been
				 * powered up for the wakeup of MCU.
				 */
				if (!kbdev->csf.firmware_hctl_core_pwr)
					kbasep_pm_toggle_power_interrupt(kbdev, false);
				backend->mcu_state = KBASE_MCU_ON_HWCNT_ENABLE;
				kbase_csf_ring_doorbell(kbdev, CSF_KERNEL_DOORBELL_NR);
			}
			break;
#endif /* KBASE_PM_RUNTIME */
		case KBASE_MCU_RESET_WAIT:
			/* Reset complete  */
			if (!backend->in_reset)
				backend->mcu_state = KBASE_MCU_OFF;

#if IS_ENABLED(CONFIG_MALI_CORESIGHT)
			kbdev->csf.coresight.disable_on_pmode_enter = false;
			kbdev->csf.coresight.enable_on_pmode_exit = false;
#endif /* IS_ENABLED(CONFIG_MALI_CORESIGHT) */
			break;

		default:
			WARN(1, "Invalid state in mcu_state: %d", backend->mcu_state);
		}

		if (backend->mcu_state != prev_state) {
			struct kbase_pm_event_log_event *event =
					kbase_pm_add_log_event(kbdev);
			event->type = KBASE_PM_LOG_EVENT_MCU_STATE;
			event->state.prev = prev_state;
			event->state.next = backend->mcu_state;

			dev_dbg(kbdev->dev, "MCU state transition: %s to %s\n",
				kbase_mcu_state_to_string(prev_state),
				kbase_mcu_state_to_string(backend->mcu_state));
			trace_mali_pm_mcu_state(prev_state, backend->mcu_state);
			kbase_ktrace_log_mcu_state(kbdev, backend->mcu_state);
		}

	} while (backend->mcu_state != prev_state);

	return 0;
}

static void core_idle_worker(struct work_struct *work)
{
	struct kbase_device *kbdev =
		container_of(work, struct kbase_device, pm.backend.core_idle_work);
	struct kbase_pm_backend_data *backend = &kbdev->pm.backend;
	unsigned long flags;

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	while (kbase_io_is_gpu_powered(kbdev) &&
	       (backend->mcu_state == KBASE_MCU_HCTL_CORE_INACTIVE_PEND)) {
		const unsigned int core_inactive_wait_ms = 1;
		u64 shaders_ready = kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_SHADER);

		if (!hctl_shader_cores_active(kbdev, shaders_ready)) {
			kbase_pm_update_state(kbdev);
			break;
		}

		spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
		msleep(core_inactive_wait_ms);
		spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	}

	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
}
#endif

static const char *kbase_l2_core_state_to_string(enum kbase_l2_core_state state)
{
	const char *const strings[] = {
#define KBASEP_L2_STATE(n) #n,
#include "mali_kbase_pm_l2_states.h"
#undef KBASEP_L2_STATE
	};
	if (WARN_ON((size_t)state >= ARRAY_SIZE(strings)))
		return "Bad level 2 cache state";
	else
		return strings[state];
}

static void kbase_ktrace_log_l2_core_state(struct kbase_device *kbdev,
					   enum kbase_l2_core_state state)
{
#if KBASE_KTRACE_ENABLE
	switch (state) {
#define KBASEP_L2_STATE(n)                                       \
	case KBASE_L2_##n:                                       \
		KBASE_KTRACE_ADD(kbdev, PM_L2_##n, NULL, state); \
		break;
#include "mali_kbase_pm_l2_states.h"
#undef KBASEP_L2_STATE
	}
#else
	CSTD_UNUSED(kbdev);
	CSTD_UNUSED(state);
#endif
}

#if !MALI_USE_CSF
/* On powering on the L2, the tracked kctx becomes stale and can be cleared.
 * This enables the backend to spare the START_FLUSH.INV_SHADER_OTHER
 * operation on the first submitted katom after the L2 powering on.
 */
static void kbase_pm_l2_clear_backend_slot_submit_kctx(struct kbase_device *kbdev)
{
	int js;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	/* Clear the slots' last katom submission kctx */
	for (js = 0; js < kbdev->gpu_props.num_job_slots; js++)
		kbdev->hwaccess.backend.slot_rb[js].last_kctx_tagged = SLOT_RB_NULL_TAG_VAL;
}
#endif

static bool can_power_down_l2(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);

	/* Defer the power-down if MMU is in process of page migration. */
	return !kbdev->mmu_page_migrate_in_progress;
}

static bool can_power_up_l2(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);

	/* Avoiding l2 transition if MMU is undergoing page migration */
	return !kbdev->mmu_page_migrate_in_progress;
}

static bool need_tiler_control(struct kbase_device *kbdev)
{
#if MALI_USE_CSF
	if (kbase_pm_no_mcu_core_pwroff(kbdev))
		return true;
	else
		return false;
#else
	CSTD_UNUSED(kbdev);
	return true;
#endif
}

#if MALI_USE_CSF
/**
 * hctl_l2_power_down - Initiate power down of L2 cache
 *
 * @kbdev: The kbase device structure for the device.
 *
 * This function initiates the power down of L2 cache when Host controls the power
 * for Tiler block. The function expects that power down of Tiler to already have
 * been initiated and it triggers the L2 power down only after the power down for
 * Tiler is complete.
 * The function shall be called only if L2 is in ready state.
 */
static void hctl_l2_power_down(struct kbase_device *kbdev)
{
	u64 tiler_trans, tiler_ready;

	if (!kbdev->pm.backend.has_host_pwr_iface)
		return;

	/* If using host power control, then tiler must already be powering down */
	tiler_ready = kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_TILER);
	WARN_ON_ONCE(tiler_ready);
	tiler_trans = kbase_pm_get_trans_cores(kbdev, KBASE_PM_CORE_TILER);
	if (tiler_trans)
		return;

	kbase_pm_invoke(kbdev, KBASE_PM_CORE_L2, kbdev->gpu_props.curr_config.l2_present,
			ACTION_PWROFF);
}
#endif

/**
 * hctl_tiler_power_up_done - Check and/or initiate power up of Tiler
 *
 * @kbdev: The kbase device structure for the device.
 *
 * This function initiates the power up of Tiler, when Host controls the power
 * for Tiler block, but only if the caller hasn't already triggered the power up
 * of Tiler.
 * The function shall be called only if L2 is in ready state.
 *
 * Return: true if power up is complete for Tiler, otherwise false.
 */
static bool hctl_tiler_power_up_done(struct kbase_device *kbdev)
{
	u64 tiler_trans = kbase_pm_get_trans_cores(kbdev, KBASE_PM_CORE_TILER);
	const u64 tiler_ready = kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_TILER);

	tiler_trans &= ~tiler_ready;
	if (tiler_trans)
		return false;

	if (!tiler_ready) {
#if MALI_USE_CSF
		if (kbdev->pm.backend.has_host_pwr_iface) {
			kbase_pm_invoke(kbdev, KBASE_PM_CORE_TILER, kbdev->gpu_props.tiler_present,
					ACTION_PWRON);
		}
#endif
		return false;
	}

	KBASE_KTRACE_ADD(kbdev, PM_CORES_CHANGE_AVAILABLE_TILER, NULL, tiler_ready);
	return true;
}

#if MALI_USE_CSF
static int delegate_pm_domain_control_to_fw(struct kbase_device *kbdev, u32 pm_domain)
{
	u64 val;
	int err;
	const u32 timeout_us =
		kbase_get_timeout_ms(kbdev, CSF_PWR_DELEGATE_TIMEOUT) * USEC_PER_MSEC;
	u64 pwr_cmd = PWR_COMMAND_COMMAND_SET(0, PWR_COMMAND_COMMAND_DELEGATE);

	const u32 allow_mask = 1 << (PWR_STATUS_ALLOW_L2_SHIFT + pm_domain);
	const u32 delegate_mask = 1 << (PWR_STATUS_DELEGATED_L2_SHIFT + pm_domain);

	pwr_cmd = PWR_COMMAND_DOMAIN_SET(pwr_cmd, pm_domain);
	kbase_reg_write32(kbdev, HOST_POWER_ENUM(PWR_COMMAND), pwr_cmd);

	err = kbase_reg_poll64_timeout(kbdev, HOST_POWER_ENUM(PWR_STATUS), val,
				       !(val & allow_mask) && (val & delegate_mask), 0, timeout_us,
				       false);
	if (!err) {
		dev_dbg(kbdev->dev, "Delegated pm domain %u", pm_domain);
		return 0;
	}

	dev_err(kbdev->dev, "Delegate command for pm domain %u failed", pm_domain);
	if (kbase_prepare_to_reset_gpu(kbdev, RESET_FLAGS_HWC_UNRECOVERABLE_ERROR))
		kbase_reset_gpu(kbdev);
	return -ETIMEDOUT;
}

static int retract_pm_domain_control_from_fw(struct kbase_device *kbdev, u32 pm_domain)
{
	u64 val;
	int err;
	const u32 timeout_us =
		kbase_get_timeout_ms(kbdev, CSF_PWR_DELEGATE_TIMEOUT) * USEC_PER_MSEC;
	u64 pwr_cmd = PWR_COMMAND_COMMAND_SET(0, PWR_COMMAND_COMMAND_RETRACT);

	pwr_cmd = PWR_COMMAND_DOMAIN_SET(pwr_cmd, pm_domain);
	kbase_reg_write32(kbdev, HOST_POWER_ENUM(PWR_COMMAND), pwr_cmd);

	err = kbase_reg_poll64_timeout(kbdev, HOST_POWER_ENUM(PWR_STATUS), val,
				       !PWR_STATUS_RETRACT_PENDING_GET(val), 0, timeout_us, false);

	if (!err) {
		dev_dbg(kbdev->dev, "Retracted pm domain %u", pm_domain);
		return 0;
	}

	dev_err(kbdev->dev, "Retract command for pm domain %u failed, error no: %d", pm_domain,
		err);
	if (kbase_prepare_to_reset_gpu(kbdev, RESET_FLAGS_HWC_UNRECOVERABLE_ERROR))
		kbase_reset_gpu(kbdev);
	return -ETIMEDOUT;
}

/**
 * delegate_pm_domains_control_to_fw - Delegate power control for Tiler, Shading engine and
 *                                     Neural engine to MCU firmware.
 *
 * @kbdev: The kbase device structure for the device.
 *
 * This function is called before the L2 cache is powered up, when power control for
 * Tiler, Shading engine and Neural engine is supposed to be controlled by the MCU firmware.
 * This function needs to be called after the GPU power cycle and GPU reset.
 *
 * GPU reset would be triggered to recover if the delegation of any of the 3 PM domains fails.
 * If a PM domain is found to be already delegated then the DELEGATE command won't be sent for
 * that domain.
 */
static void delegate_pm_domains_control_to_fw(struct kbase_device *kbdev)
{
	const u64 pwr_status = kbase_reg_read64(kbdev, HOST_POWER_ENUM(PWR_STATUS));
	bool ne_pwr_status_delegated =
		kbase_csf_dev_has_ne(kbdev) ? PWR_STATUS_DELEGATED_NEURAL_GET(pwr_status) : false;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	WARN_ONCE(PWR_STATUS_DELEGATED_TILER_GET(pwr_status) ||
			  PWR_STATUS_DELEGATED_SHADER_GET(pwr_status) || ne_pwr_status_delegated,
		  "Trying to delegate control for already delegated PM domains, pwr_status %llx",
		  pwr_status);

	if (delegate_pm_domain_control_to_fw(kbdev, PWR_COMMAND_DOMAIN_TILER))
		return;

	if (delegate_pm_domain_control_to_fw(kbdev, PWR_COMMAND_DOMAIN_SHADER))
		return;

	if (kbase_csf_dev_has_ne(kbdev))
		delegate_pm_domain_control_to_fw(kbdev, PWR_COMMAND_DOMAIN_NEURAL);
}

/**
 * retract_pm_domains_control_from_fw - Retract power control for Tiler, shading engine and
 *                                      neural engine from MCU firmware.
 *
 * @kbdev: The kbase device structure for the device.
 *
 * This function is called before the L2 cache is powered up, when power control for
 * Tiler, shading engine and neural engine is supposed to be controlled by the host.
 * This function must be called only if the power control was previously delegated to
 * the firmware.
 *
 * GPU reset would be triggered to recover if the retract fails for any of the 3 PM domains.
 * If a PM domain is already not delegated then the RETRACT command won't be sent for that domain.
 */
static void retract_pm_domains_control_from_fw(struct kbase_device *kbdev)
{
	const u64 pwr_status = kbase_reg_read64(kbdev, HOST_POWER_ENUM(PWR_STATUS));
	bool ne_pwr_status_not_delegated = kbase_csf_dev_has_ne(kbdev) ?
							 (!PWR_STATUS_DELEGATED_NEURAL_GET(pwr_status)) :
							 false;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	WARN_ONCE(
		!PWR_STATUS_DELEGATED_TILER_GET(pwr_status) ||
			!PWR_STATUS_DELEGATED_SHADER_GET(pwr_status) || ne_pwr_status_not_delegated,
		"Trying to retract control for PM domains that were not delegated, pwr_status %llx",
		pwr_status);

	if (retract_pm_domain_control_from_fw(kbdev, PWR_COMMAND_DOMAIN_TILER))
		return;

	if (retract_pm_domain_control_from_fw(kbdev, PWR_COMMAND_DOMAIN_SHADER))
		return;

	if (kbase_csf_dev_has_ne(kbdev))
		retract_pm_domain_control_from_fw(kbdev, PWR_COMMAND_DOMAIN_NEURAL);
}

u32 kbase_pm_get_domain_status(struct kbase_device *kbdev, u32 pm_domain, u32 endpoint)
{
	u64 val;
	int err;
	u64 pwr_cmd = 0;
	const u32 timeout_us = kbase_get_timeout_ms(kbdev, CSF_PWR_INSPECT_TIMEOUT) * USEC_PER_MSEC;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	/* Send INSPECT command for the power domain */
	kbase_reg_write64(kbdev, HOST_POWER_ENUM(PWR_CMDARG), 1UL << endpoint);
	pwr_cmd = PWR_COMMAND_COMMAND_SET(pwr_cmd, PWR_COMMAND_COMMAND_INSPECT);
	pwr_cmd = PWR_COMMAND_DOMAIN_SET(pwr_cmd, pm_domain);
	kbase_reg_write32(kbdev, HOST_POWER_ENUM(PWR_COMMAND), pwr_cmd);

	err = kbase_reg_poll64_timeout(kbdev, HOST_POWER_ENUM(PWR_STATUS), val,
				       !PWR_STATUS_INSPECT_PENDING_GET(val), 0, timeout_us, false);

	if (!err) {
		u32 reg_status = (u32)kbase_reg_read64(kbdev, HOST_POWER_ENUM(PWR_CMDARG));

		dev_dbg(kbdev->dev, "Inpect pm domain status = %05x", reg_status);
		return reg_status;
	}
	dev_err(kbdev->dev, "Inspect command for pm domain %u failed", pm_domain);
	return 0;
}
#endif /* MALI_USE_CSF */

/* Forward declaration for kbase_pm_request_gpu_cycle_counter_do_request */
static void kbase_pm_request_gpu_cycle_counter_do_request(struct kbase_device *kbdev);

/* Forward declaration for kbase_pm_release_gpu_cycle_counter_nolock */
void kbase_pm_release_gpu_cycle_counter_nolock(struct kbase_device *kbdev);

static int kbase_pm_l2_update_state(struct kbase_device *kbdev)
{
	struct kbase_pm_backend_data *backend = &kbdev->pm.backend;
	u64 l2_present = kbdev->gpu_props.curr_config.l2_present;
	u64 tiler_present = kbdev->gpu_props.tiler_present;
#if MALI_USE_CSF
	const bool has_host_pwr_iface = backend->has_host_pwr_iface;
#endif
	bool l2_power_up_done;
	enum kbase_l2_core_state prev_state;
#if IS_ENABLED(CONFIG_MALI_TRACE_POWER_GPU_WORK_PERIOD) && !MALI_USE_CSF
	u64 cycle_count, system_time;
#endif /* IS_ENABLED(CONFIG_MALI_TRACE_POWER_GPU_WORK_PERIOD) && !MALI_USE_CSF */

	lockdep_assert_held(&kbdev->hwaccess_lock);

	do {
		/* Get current state */
		u64 l2_trans = kbase_pm_get_trans_cores(kbdev, KBASE_PM_CORE_L2);
		u64 l2_ready = kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_L2);

		/*
		 * kbase_pm_get_ready_cores and kbase_pm_get_trans_cores
		 * are vulnerable to corruption if gpu is lost
		 */
		if (!kbase_io_has_gpu(kbdev)) {
			backend->shaders_state = KBASE_SHADERS_OFF_CORESTACK_OFF;
			backend->hwcnt_desired = false;
			if (!backend->hwcnt_disabled) {
				/* Don't progress until hw counters are disabled
				 * This may involve waiting for a worker to complete.
				 * The HW counters backend disable code checks for the
				 * GPU removed case and will error out without touching
				 * the hardware. This step is needed to keep the HW
				 * counters in a consistent state after a GPU lost.
				 */
				backend->l2_state = KBASE_L2_ON_HWCNT_DISABLE;
				KBASE_KTRACE_ADD(kbdev, PM_L2_ON_HWCNT_DISABLE, NULL,
						 backend->l2_state);
				kbase_pm_trigger_hwcnt_disable(kbdev);
			}

			if (backend->hwcnt_disabled && (backend->l2_state != KBASE_L2_OFF)) {
				backend->l2_state = KBASE_L2_OFF;
				KBASE_KTRACE_ADD(kbdev, PM_L2_OFF, NULL, backend->l2_state);
				dev_dbg(kbdev->dev, "GPU lost has occurred - L2 off\n");
			}
			break;
		}

		/* mask off ready from trans in case transitions finished
		 * between the register reads
		 */
		l2_trans &= ~l2_ready;

		prev_state = backend->l2_state;

		switch (backend->l2_state) {
		case KBASE_L2_OFF:
			if (kbase_pm_is_l2_desired(kbdev) && can_power_up_l2(kbdev)) {
#if MALI_USE_CSF && defined(KBASE_PM_RUNTIME)
#if IS_ENABLED(CONFIG_SOC_GS201)
				// Workaround: give a short pause here before starting L2 transition.
				udelay(200);
#endif
				/* Enable HW timer of IPA control before
				 * L2 cache is powered-up.
				 */
				{
					kbase_ipa_control_handle_gpu_sleep_exit(kbdev);
				}
#endif
				/*
				 * Set the desired config for L2 before
				 * powering it on
				 */
				kbase_pm_l2_config_override(kbdev);
				kbase_pbha_write_settings(kbdev);

				/* If Host is controlling the power for shader
				 * cores, then it also needs to control the
				 * power for Tiler.
				 * Powering on the tiler will also power the
				 * L2 cache.
				 */
				if (need_tiler_control(kbdev)) {
#if MALI_USE_CSF
					if (backend->pwr_cntl_delegated) {
						retract_pm_domains_control_from_fw(kbdev);
						backend->pwr_cntl_delegated = false;
					}
					if (has_host_pwr_iface) {
						/* When using the host power interface,
						 * L2 needs to be explicitly powered up
						 * before the tiler.
						 */
						kbase_pm_invoke(kbdev, KBASE_PM_CORE_L2, l2_present,
								ACTION_PWRON);
					} else
#endif
						kbase_pm_invoke(kbdev, KBASE_PM_CORE_TILER,
								tiler_present, ACTION_PWRON);
				} else {
#if MALI_USE_CSF
					if (has_host_pwr_iface && !backend->pwr_cntl_delegated) {
						delegate_pm_domains_control_to_fw(kbdev);
						backend->pwr_cntl_delegated = true;
					}
#endif

					kbase_pm_invoke(kbdev, KBASE_PM_CORE_L2, l2_present,
							ACTION_PWRON);
				}
#if !MALI_USE_CSF
				/* If we have more than one L2 cache then we
				 * must power them on explicitly.
				 */
				if (l2_present != 1)
					kbase_pm_invoke(kbdev, KBASE_PM_CORE_L2, l2_present & ~1ULL,
							ACTION_PWRON);
				/* Clear backend slot submission kctx */
				kbase_pm_l2_clear_backend_slot_submit_kctx(kbdev);
#endif
				backend->l2_state = KBASE_L2_PEND_ON;
			}
			break;

		case KBASE_L2_PEND_ON:
			l2_power_up_done = false;
			if (!l2_trans && l2_ready == l2_present) {
				if (need_tiler_control(kbdev)) {
					l2_power_up_done = hctl_tiler_power_up_done(kbdev);
				} else {
					KBASE_KTRACE_ADD(kbdev, PM_CORES_CHANGE_AVAILABLE_L2, NULL,
							 l2_ready);
					l2_power_up_done = true;
				}
			}
			if (l2_power_up_done) {
				/*
				 * Ensure snoops are enabled after L2 is powered
				 * up. Note that kbase keeps track of the snoop
				 * state, so safe to repeatedly call.
				 */
				kbase_pm_cache_snoop_enable(kbdev);

				/* With the L2 enabled, we can now enable
				 * hardware counters.
				 */
				if (kbdev->pm.backend.gpu_clock_slow_down_wa)
					backend->l2_state = KBASE_L2_RESTORE_CLOCKS;
				else
					backend->l2_state = KBASE_L2_ON_HWCNT_ENABLE;

				/* Now that the L2 is on, the shaders can start
				 * powering on if they're required. The obvious
				 * way to do this would be to call
				 * kbase_pm_shaders_update_state() here.
				 * However, that would make the two state
				 * machines mutually recursive, as the opposite
				 * would be needed for powering down. Instead,
				 * callers of this function should use the
				 * kbase_pm_update_state() wrapper, which will
				 * call the shader state machine immediately
				 * after the L2 (for power up), or
				 * automatically re-invoke the L2 state machine
				 * when the shaders power down.
				 */
			}
			break;

		case KBASE_L2_RESTORE_CLOCKS:
			/* We always assume only GPUs being affected by
			 * KBASE_HW_ISSUE_GPU2017_1336 fall into this state
			 */
			WARN_ON_ONCE(!kbdev->pm.backend.gpu_clock_slow_down_wa);

			/* If L2 not needed, we need to make sure cancellation
			 * of any previously issued work to restore GPU clock.
			 * For it, move to KBASE_L2_SLOW_DOWN_CLOCKS state.
			 */
			if (!kbase_pm_is_l2_desired(kbdev)) {
				backend->l2_state = KBASE_L2_SLOW_DOWN_CLOCKS;
				break;
			}

			backend->gpu_clock_slow_down_desired = false;
			if (backend->gpu_clock_slowed_down)
				kbase_pm_control_gpu_clock(kbdev);
			else
				backend->l2_state = KBASE_L2_ON_HWCNT_ENABLE;
			break;

		case KBASE_L2_ON_HWCNT_ENABLE:
#if !MALI_USE_CSF
			backend->hwcnt_desired = true;
			if (backend->hwcnt_disabled) {
				kbase_hwcnt_context_enable(kbdev->hwcnt_gpu_ctx);
				backend->hwcnt_disabled = false;
			}
#if IS_ENABLED(CONFIG_MALI_TRACE_POWER_GPU_WORK_PERIOD)
			/* START the GPU cycle counter*/
			kbase_pm_request_gpu_cycle_counter_do_request(kbdev);
#endif /* IS_ENABLED(CONFIG_MALI_TRACE_POWER_GPU_WORK_PERIOD) */
#endif
			backend->l2_state = KBASE_L2_ON;
			break;

		case KBASE_L2_ON:
			if (!kbase_pm_is_l2_desired(kbdev)) {
#if !MALI_USE_CSF
				/* Do not power off L2 until the shaders and
				 * core stacks are off.
				 */
				if (backend->shaders_state != KBASE_SHADERS_OFF_CORESTACK_OFF)
					break;
#else
				/* Do not power off L2 until the MCU has been stopped */
				if ((backend->mcu_state != KBASE_MCU_OFF) &&
				    (backend->mcu_state != KBASE_MCU_IN_SLEEP))
					break;
#endif

				/* We need to make sure hardware counters are
				 * disabled before powering down the L2, to
				 * prevent loss of data.
				 *
				 * We waited until after the cores were powered
				 * down to prevent ping-ponging between hwcnt
				 * enabled and disabled, which would have
				 * happened if userspace submitted more work
				 * while we were trying to power down.
				 */
				backend->l2_state = KBASE_L2_ON_HWCNT_DISABLE;
			}
			break;

		case KBASE_L2_ON_HWCNT_DISABLE:
#if !MALI_USE_CSF
			/* If the L2 became desired while we were waiting on the
			 * worker to do the actual hwcnt disable (which might
			 * happen if some work was submitted immediately after
			 * the shaders powered off), then we need to early-out
			 * of this state and re-enable hwcnt.
			 *
			 * If we get lucky, the hwcnt disable might not have
			 * actually started yet, and the logic in the hwcnt
			 * enable state will prevent the worker from
			 * performing the disable entirely, preventing loss of
			 * any hardware counter data.
			 *
			 * If the hwcnt disable has started, then we'll lose
			 * a tiny amount of hardware counter data between the
			 * disable and the re-enable occurring.
			 *
			 * This loss of data is preferable to the alternative,
			 * which is to block the shader cores from doing any
			 * work until we're sure hwcnt has been re-enabled.
			 */
			if (kbase_pm_is_l2_desired(kbdev)) {
				backend->l2_state = KBASE_L2_ON_HWCNT_ENABLE;
				break;
			}
#if IS_ENABLED(CONFIG_MALI_TRACE_POWER_GPU_WORK_PERIOD) && !MALI_USE_CSF
			/* Retrieve the cycle count */
			kbase_backend_get_gpu_time_norequest(
					kbdev, &cycle_count, &system_time, NULL);
			/* Store the last seen cycle count */
			kbdev->last_cycle_count = cycle_count;
			/* STOP cycle count */
			kbase_pm_release_gpu_cycle_counter_nolock(kbdev);
#endif /* IS_ENABLED(CONFIG_MALI_TRACE_POWER_GPU_WORK_PERIOD) & MALI_USE_CSF */

			backend->hwcnt_desired = false;
			if (!backend->hwcnt_disabled)
				kbase_pm_trigger_hwcnt_disable(kbdev);
#endif

			if (backend->hwcnt_disabled) {
				if (kbdev->pm.backend.gpu_clock_slow_down_wa)
					backend->l2_state = KBASE_L2_SLOW_DOWN_CLOCKS;
				else
					backend->l2_state = KBASE_L2_POWER_DOWN;
			}
			break;

		case KBASE_L2_SLOW_DOWN_CLOCKS:
			/* We always assume only GPUs being affected by
			 * KBASE_HW_ISSUE_GPU2017_1336 fall into this state
			 */
			WARN_ON_ONCE(!kbdev->pm.backend.gpu_clock_slow_down_wa);

			/* L2 needs to be powered up. And we need to make sure
			 * cancellation of any previously issued work to slow
			 * down GPU clock. For it, we move to the state,
			 * KBASE_L2_RESTORE_CLOCKS.
			 */
			if (kbase_pm_is_l2_desired(kbdev)) {
				backend->l2_state = KBASE_L2_RESTORE_CLOCKS;
				break;
			}

			backend->gpu_clock_slow_down_desired = true;
			if (!backend->gpu_clock_slowed_down)
				kbase_pm_control_gpu_clock(kbdev);
			else
				backend->l2_state = KBASE_L2_POWER_DOWN;

			break;

		case KBASE_L2_POWER_DOWN:
			if (kbase_pm_is_l2_desired(kbdev))
				backend->l2_state = KBASE_L2_PEND_ON;
			else if (can_power_down_l2(kbdev)) {
				if (!backend->l2_always_on) {
#if MALI_USE_CSF
					if (need_tiler_control(kbdev) && has_host_pwr_iface) {
						/* If using host power control,
						 * the tiler must be explicitly turned off
						 * before the L2
						 */
						kbase_pm_invoke(kbdev, KBASE_PM_CORE_TILER,
								tiler_present, ACTION_PWROFF);
					} else
#endif
						/* Powering off the L2 will also power off the tiler. */
						kbase_pm_invoke(kbdev, KBASE_PM_CORE_L2, l2_present,
								ACTION_PWROFF);
				} else
					/* If L2 cache is powered then we must flush it
					 * before we power off the GPU. Normally this
					 * would have been handled when the L2 was
					 * powered off.
					 */
					kbase_gpu_start_cache_clean_nolock(
						kbdev, GPU_COMMAND_CACHE_CLN_INV_L2);
#if !MALI_USE_CSF
				KBASE_KTRACE_ADD(kbdev, PM_CORES_CHANGE_AVAILABLE_TILER, NULL, 0u);
#else
				KBASE_KTRACE_ADD(kbdev, PM_CORES_CHANGE_AVAILABLE_L2, NULL, 0u);
#endif
				backend->l2_state = KBASE_L2_PEND_OFF;
			}
			break;

		case KBASE_L2_PEND_OFF:
			if (likely(!backend->l2_always_on)) {
#if MALI_USE_CSF
				if (need_tiler_control(kbdev) && l2_ready) {
					hctl_l2_power_down(kbdev);
					break;
				}
#endif
				if (l2_trans || l2_ready)
					break;
			} else if (kbdev->cache_clean_in_progress)
				break;
#if MALI_USE_CSF
#if defined(KBASE_PM_RUNTIME)
				/* Allow clock gating within the GPU and prevent it
			 * from being seen as active during sleep.
			 */
			{
				kbase_ipa_control_handle_gpu_sleep_enter(kbdev);
			}
#endif
			/* Disabling MCU after L2 cache power down is to address
			 * KBASE_HW_ISSUE_TITANHW_2922 hardware issue.
			 */
			if (backend->l2_force_off_after_mcu_halt) {
				kbase_csf_stop_firmware_and_wait(kbdev);
				WARN_ON_ONCE(backend->mcu_state != KBASE_MCU_OFF);
				backend->l2_force_off_after_mcu_halt = false;
			}
#endif
			/* L2 is now powered off */
			backend->l2_state = KBASE_L2_OFF;

			break;

		case KBASE_L2_RESET_WAIT:
			/* Reset complete  */
			if (!backend->in_reset) {
#if MALI_USE_CSF
				backend->l2_force_off_after_mcu_halt = false;
#endif
				backend->l2_state = KBASE_L2_OFF;
			}

			break;

		default:
			WARN(1, "Invalid state in l2_state: %d", backend->l2_state);
		}

		if (backend->l2_state != prev_state) {
			struct kbase_pm_event_log_event *event =
					kbase_pm_add_log_event(kbdev);
			event->type = KBASE_PM_LOG_EVENT_L2_STATE;
			event->state.prev = prev_state;
			event->state.next = backend->l2_state;

			dev_dbg(kbdev->dev, "L2 state transition: %s to %s\n",
				kbase_l2_core_state_to_string(prev_state),
				kbase_l2_core_state_to_string(backend->l2_state));
			trace_mali_pm_l2_state(prev_state, backend->l2_state);
#if IS_ENABLED(CONFIG_SOC_GS201)
			if (!kbdev->pm.backend.invoke_poweroff_wait_wq_when_l2_off &&
				backend->l2_state == KBASE_L2_OFF) {
				dev_warn(kbdev->dev, "transition to l2 off without waking waiter");
			}
#endif
			kbase_ktrace_log_l2_core_state(kbdev, backend->l2_state);
		}

	} while (backend->l2_state != prev_state);

	if (kbdev->pm.backend.invoke_poweroff_wait_wq_when_l2_off &&
	    backend->l2_state == KBASE_L2_OFF) {
		kbdev->pm.backend.invoke_poweroff_wait_wq_when_l2_off = false;
		queue_work(kbdev->pm.backend.gpu_poweroff_wait_wq,
			   &kbdev->pm.backend.gpu_poweroff_wait_work);
	}

	return 0;
}

static void shader_poweroff_timer_stop_callback(struct work_struct *data)
{
	unsigned long flags;
	struct kbasep_pm_tick_timer_state *stt =
		container_of(data, struct kbasep_pm_tick_timer_state, work);
	struct kbase_device *kbdev =
		container_of(stt, struct kbase_device, pm.backend.shader_tick_timer);

	hrtimer_cancel(&stt->timer);

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);

	stt->cancel_queued = false;
	if (kbase_io_is_gpu_powered(kbdev))
		kbase_pm_update_state(kbdev);

	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
}

/**
 * shader_poweroff_timer_queue_cancel - cancel the shader poweroff tick timer
 * @kbdev:      pointer to kbase device
 *
 * Synchronization between the shader state machine and the timer thread is
 * difficult. This is because situations may arise where the state machine
 * wants to start the timer, but the callback is already running, and has
 * already passed the point at which it checks whether it is required, and so
 * cancels itself, even though the state machine may have just tried to call
 * hrtimer_start.
 *
 * This cannot be stopped by holding hwaccess_lock in the timer thread,
 * because there are still infinitesimally small sections at the start and end
 * of the callback where the lock is not held.
 *
 * Instead, a new state is added to the shader state machine,
 * KBASE_SHADERS_OFF_CORESTACK_OFF_TIMER_PEND_OFF. This is used to guarantee
 * that when the shaders are switched off, the timer has definitely been
 * cancelled. As a result, when KBASE_SHADERS_ON_CORESTACK_ON is left and the
 * timer is started, it is guaranteed that either the timer is already running
 * (from an availability change or cancelled timer), or hrtimer_start will
 * succeed. It is critical to avoid ending up in
 * KBASE_SHADERS_WAIT_OFF_CORESTACK_ON without the timer running, or it could
 * hang there forever.
 */
static void shader_poweroff_timer_queue_cancel(struct kbase_device *kbdev)
{
	struct kbasep_pm_tick_timer_state *stt = &kbdev->pm.backend.shader_tick_timer;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	stt->needed = false;

	if (hrtimer_active(&stt->timer) && !stt->cancel_queued) {
		stt->cancel_queued = true;
		queue_work(stt->wq, &stt->work);
	}
}

#if !MALI_USE_CSF
static const char *kbase_shader_core_state_to_string(enum kbase_shader_core_state state)
{
	const char *const strings[] = {
#define KBASEP_SHADER_STATE(n) #n,
#include "mali_kbase_pm_shader_states.h"
#undef KBASEP_SHADER_STATE
	};
	if (WARN_ON((size_t)state >= ARRAY_SIZE(strings)))
		return "Bad shader core state";
	else
		return strings[state];
}

static int kbase_pm_shaders_update_state(struct kbase_device *kbdev)
{
	struct kbase_pm_backend_data *backend = &kbdev->pm.backend;
	struct kbasep_pm_tick_timer_state *stt = &kbdev->pm.backend.shader_tick_timer;
	enum kbase_shader_core_state prev_state;
	u64 stacks_avail = 0;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	if (corestack_driver_control)
		/* Always power on all the corestacks. Disabling certain
		 * corestacks when their respective shaders are not in the
		 * available bitmap is not currently supported.
		 */
		stacks_avail = kbase_pm_get_present_cores(kbdev, KBASE_PM_CORE_STACK);

	do {
		u64 shaders_trans = kbase_pm_get_trans_cores(kbdev, KBASE_PM_CORE_SHADER);
		u64 shaders_ready = kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_SHADER);
		u64 stacks_trans = 0;
		u64 stacks_ready = 0;

		if (corestack_driver_control) {
			stacks_trans = kbase_pm_get_trans_cores(kbdev, KBASE_PM_CORE_STACK);
			stacks_ready = kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_STACK);
		}

		/*
		 * kbase_pm_get_ready_cores and kbase_pm_get_trans_cores
		 * are vulnerable to corruption if gpu is lost
		 */
		if (!kbase_io_has_gpu(kbdev)) {
			backend->shaders_state = KBASE_SHADERS_OFF_CORESTACK_OFF;
			dev_dbg(kbdev->dev, "GPU lost has occurred - shaders off\n");
			break;
		}

		/* mask off ready from trans in case transitions finished
		 * between the register reads
		 */
		shaders_trans &= ~shaders_ready;
		stacks_trans &= ~stacks_ready;

		prev_state = backend->shaders_state;

		switch (backend->shaders_state) {
		case KBASE_SHADERS_OFF_CORESTACK_OFF:
			/* Ignore changes to the shader core availability
			 * except at certain points where we can handle it,
			 * i.e. off and SHADERS_ON_CORESTACK_ON.
			 */
			backend->shaders_desired_mask = kbase_pm_ca_get_core_mask(kbdev);
			backend->pm_shaders_core_mask = 0;

			if (backend->shaders_desired && backend->l2_state == KBASE_L2_ON) {
				if (backend->hwcnt_desired && !backend->hwcnt_disabled) {
					/* Trigger a hwcounter dump */
					backend->hwcnt_desired = false;
					kbase_pm_trigger_hwcnt_disable(kbdev);
				}

				if (backend->hwcnt_disabled) {
					if (corestack_driver_control) {
						kbase_pm_invoke(kbdev, KBASE_PM_CORE_STACK,
								stacks_avail, ACTION_PWRON);
					}
					backend->shaders_state =
						KBASE_SHADERS_OFF_CORESTACK_PEND_ON;
				}
			}
			break;

		case KBASE_SHADERS_OFF_CORESTACK_PEND_ON:
			if (!stacks_trans && stacks_ready == stacks_avail) {
				backend->shaders_avail = backend->shaders_desired_mask;
				kbase_pm_invoke(kbdev, KBASE_PM_CORE_SHADER, backend->shaders_avail,
						ACTION_PWRON);

				if (backend->pm_current_policy &&
				    backend->pm_current_policy->handle_event)
					backend->pm_current_policy->handle_event(
						kbdev, KBASE_PM_POLICY_EVENT_POWER_ON);

				backend->shaders_state = KBASE_SHADERS_PEND_ON_CORESTACK_ON;
			}
			break;

		case KBASE_SHADERS_PEND_ON_CORESTACK_ON:
			if (!shaders_trans && shaders_ready == backend->shaders_avail) {
				KBASE_KTRACE_ADD(kbdev, PM_CORES_CHANGE_AVAILABLE, NULL,
						 shaders_ready);
				backend->pm_shaders_core_mask = shaders_ready;
				backend->hwcnt_desired = true;
				if (backend->hwcnt_disabled) {
#if MALI_USE_CSF
					unsigned long flags;

					kbase_csf_scheduler_spin_lock(kbdev, &flags);
#endif
					kbase_hwcnt_context_enable(kbdev->hwcnt_gpu_ctx);
#if MALI_USE_CSF
					kbase_csf_scheduler_spin_unlock(kbdev, flags);
#endif
					backend->hwcnt_disabled = false;
				}

				backend->shaders_state = KBASE_SHADERS_ON_CORESTACK_ON;
			}
			break;

		case KBASE_SHADERS_ON_CORESTACK_ON:
			backend->shaders_desired_mask = kbase_pm_ca_get_core_mask(kbdev);

			/* If shaders to change state, trigger a counter dump */
			if (!backend->shaders_desired ||
			    (backend->shaders_desired_mask != shaders_ready)) {
				backend->hwcnt_desired = false;
				if (!backend->hwcnt_disabled)
					kbase_pm_trigger_hwcnt_disable(kbdev);
				backend->shaders_state = KBASE_SHADERS_ON_CORESTACK_ON_RECHECK;
			}
			break;

		case KBASE_SHADERS_ON_CORESTACK_ON_RECHECK:
			backend->shaders_desired_mask = kbase_pm_ca_get_core_mask(kbdev);

			if (!backend->hwcnt_disabled) {
				/* Wait for being disabled */
				;
			} else if (!backend->shaders_desired) {
				if (backend->pm_current_policy &&
				    backend->pm_current_policy->handle_event)
					backend->pm_current_policy->handle_event(
						kbdev, KBASE_PM_POLICY_EVENT_IDLE);

				if (kbdev->pm.backend.protected_transition_override ||
				    (kbase_has_arbiter(kbdev) && (kbase_pm_is_suspending(kbdev) ||
								  kbase_io_is_gpu_lost(kbdev))) ||
				    !stt->configured_ticks || WARN_ON(stt->cancel_queued)) {
					backend->shaders_state =
						KBASE_SHADERS_WAIT_FINISHED_CORESTACK_ON;
				} else {
					stt->remaining_ticks = stt->configured_ticks;
					stt->needed = true;

					/* The shader hysteresis timer is not
					 * done the obvious way, which would be
					 * to start an hrtimer when the shader
					 * power off is requested. Instead,
					 * use a 'tick' timer, and set the
					 * remaining number of ticks on a power
					 * off request.  This avoids the
					 * latency of starting, then
					 * immediately cancelling an hrtimer
					 * when the shaders are re-requested
					 * before the timeout expires.
					 */
					if (!hrtimer_active(&stt->timer))
						hrtimer_start(&stt->timer, stt->configured_interval,
							      HRTIMER_MODE_REL);

					backend->shaders_state =
						KBASE_SHADERS_WAIT_OFF_CORESTACK_ON;
				}
			} else if (backend->shaders_desired_mask & ~shaders_ready) {
				/* set cores ready but not available to
				 * meet KBASE_SHADERS_PEND_ON_CORESTACK_ON
				 * check pass
				 */
				backend->shaders_avail =
					(backend->shaders_desired_mask | shaders_ready);

				kbase_pm_invoke(kbdev, KBASE_PM_CORE_SHADER,
						backend->shaders_avail & ~shaders_ready,
						ACTION_PWRON);
				backend->shaders_state = KBASE_SHADERS_PEND_ON_CORESTACK_ON;
			} else if (shaders_ready & ~backend->shaders_desired_mask) {
				backend->shaders_state = KBASE_SHADERS_WAIT_GPU_IDLE;
			} else {
				backend->shaders_state = KBASE_SHADERS_PEND_ON_CORESTACK_ON;
			}
			break;

		case KBASE_SHADERS_WAIT_OFF_CORESTACK_ON:
			if (WARN_ON(!hrtimer_active(&stt->timer))) {
				stt->remaining_ticks = 0;
				backend->shaders_state = KBASE_SHADERS_WAIT_FINISHED_CORESTACK_ON;
			}

			if (backend->shaders_desired) {
				if (backend->pm_current_policy &&
				    backend->pm_current_policy->handle_event)
					backend->pm_current_policy->handle_event(
						kbdev, KBASE_PM_POLICY_EVENT_TIMER_HIT);

				stt->remaining_ticks = 0;
				backend->shaders_state = KBASE_SHADERS_ON_CORESTACK_ON_RECHECK;
			} else if (stt->remaining_ticks == 0) {
				if (backend->pm_current_policy &&
				    backend->pm_current_policy->handle_event)
					backend->pm_current_policy->handle_event(
						kbdev, KBASE_PM_POLICY_EVENT_TIMER_MISS);

				backend->shaders_state = KBASE_SHADERS_WAIT_FINISHED_CORESTACK_ON;
			} else if (kbase_has_arbiter(kbdev) &&
				   (kbase_pm_is_suspending(kbdev) || kbase_io_is_gpu_lost(kbdev))) {
				backend->shaders_state = KBASE_SHADERS_WAIT_FINISHED_CORESTACK_ON;
			}
			break;

		case KBASE_SHADERS_WAIT_GPU_IDLE:
			/* If partial shader core off need to wait the job in
			 * running and next register finished then flush L2
			 * or it might hit GPU2017-861
			 */
			if (!kbase_gpu_atoms_submitted_any(kbdev)) {
				backend->partial_shaderoff = true;
				backend->shaders_state = KBASE_SHADERS_WAIT_FINISHED_CORESTACK_ON;
			}
			break;

		case KBASE_SHADERS_WAIT_FINISHED_CORESTACK_ON:
			if (!backend->partial_shaderoff)
				shader_poweroff_timer_queue_cancel(kbdev);

			if (kbase_hw_has_issue(kbdev, KBASE_HW_ISSUE_TTRX_921)) {
				kbase_gpu_start_cache_clean_nolock(kbdev,
								   GPU_COMMAND_CACHE_CLN_INV_L2);
				backend->shaders_state = KBASE_SHADERS_L2_FLUSHING_CORESTACK_ON;
			} else {
				backend->shaders_state = KBASE_SHADERS_READY_OFF_CORESTACK_ON;
			}
			break;

		case KBASE_SHADERS_L2_FLUSHING_CORESTACK_ON:
			if (!kbdev->cache_clean_in_progress)
				backend->shaders_state = KBASE_SHADERS_READY_OFF_CORESTACK_ON;

			break;

		case KBASE_SHADERS_READY_OFF_CORESTACK_ON:
			if (backend->partial_shaderoff) {
				backend->partial_shaderoff = false;
				/* remove cores available but not ready to
				 * meet KBASE_SHADERS_PEND_ON_CORESTACK_ON
				 * check pass
				 */

				/* shaders_desired_mask shall be a subset of
				 * shaders_ready
				 */
				WARN_ON(backend->shaders_desired_mask & ~shaders_ready);
				WARN_ON(!(backend->shaders_desired_mask & shaders_ready));

				backend->shaders_avail = backend->shaders_desired_mask;
				kbase_pm_invoke(kbdev, KBASE_PM_CORE_SHADER,
						shaders_ready & ~backend->shaders_avail,
						ACTION_PWROFF);
				backend->shaders_state = KBASE_SHADERS_PEND_ON_CORESTACK_ON;
				KBASE_KTRACE_ADD(kbdev, PM_CORES_CHANGE_AVAILABLE, NULL,
						 (shaders_ready & ~backend->shaders_avail));
			} else {
				kbase_pm_invoke(kbdev, KBASE_PM_CORE_SHADER, shaders_ready,
						ACTION_PWROFF);

				KBASE_KTRACE_ADD(kbdev, PM_CORES_CHANGE_AVAILABLE, NULL, 0u);

				backend->shaders_state = KBASE_SHADERS_PEND_OFF_CORESTACK_ON;
			}
			break;

		case KBASE_SHADERS_PEND_OFF_CORESTACK_ON:
			if (!shaders_trans && !shaders_ready) {
				if (corestack_driver_control)
					kbase_pm_invoke(kbdev, KBASE_PM_CORE_STACK, stacks_avail,
							ACTION_PWROFF);

				backend->shaders_state = KBASE_SHADERS_OFF_CORESTACK_PEND_OFF;
			}
			break;

		case KBASE_SHADERS_OFF_CORESTACK_PEND_OFF:
			if (!stacks_trans && !stacks_ready) {
				/* On powered off, re-enable the hwcnt */
				backend->pm_shaders_core_mask = 0;
				backend->hwcnt_desired = true;
				if (backend->hwcnt_disabled) {
#if MALI_USE_CSF
					unsigned long flags;

					kbase_csf_scheduler_spin_lock(kbdev, &flags);
#endif
					kbase_hwcnt_context_enable(kbdev->hwcnt_gpu_ctx);
#if MALI_USE_CSF
					kbase_csf_scheduler_spin_unlock(kbdev, flags);
#endif
					backend->hwcnt_disabled = false;
				}
				backend->shaders_state =
					KBASE_SHADERS_OFF_CORESTACK_OFF_TIMER_PEND_OFF;
			}
			break;

		case KBASE_SHADERS_OFF_CORESTACK_OFF_TIMER_PEND_OFF:
			if (!hrtimer_active(&stt->timer) && !stt->cancel_queued)
				backend->shaders_state = KBASE_SHADERS_OFF_CORESTACK_OFF;
			break;

		case KBASE_SHADERS_RESET_WAIT:
			/* Reset complete */
			if (!backend->in_reset)
				backend->shaders_state =
					KBASE_SHADERS_OFF_CORESTACK_OFF_TIMER_PEND_OFF;
			break;
		}

		if (backend->shaders_state != prev_state) {
			struct kbase_pm_event_log_event *event =
					kbase_pm_add_log_event(kbdev);
			event->type = KBASE_PM_LOG_EVENT_SHADERS_STATE;
			event->state.prev = prev_state;
			event->state.next = backend->shaders_state;

			dev_dbg(kbdev->dev, "Shader state transition: %s to %s\n",
				kbase_shader_core_state_to_string(prev_state),
				kbase_shader_core_state_to_string(backend->shaders_state));
		}

	} while (backend->shaders_state != prev_state);

	return 0;
}
#endif /* !MALI_USE_CSF */

static bool kbase_pm_is_in_desired_state_nolock(struct kbase_device *kbdev)
{
	bool in_desired_state = true;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	in_desired_state = kbase_pm_l2_is_in_desired_state(kbdev);

#if !MALI_USE_CSF
	if (kbdev->pm.backend.shaders_desired &&
	    kbdev->pm.backend.shaders_state != KBASE_SHADERS_ON_CORESTACK_ON)
		in_desired_state = false;
	else if (!kbdev->pm.backend.shaders_desired &&
		 kbdev->pm.backend.shaders_state != KBASE_SHADERS_OFF_CORESTACK_OFF)
		in_desired_state = false;
#else
	in_desired_state &= kbase_pm_mcu_is_in_desired_state(kbdev);
#endif

	return in_desired_state;
}

static bool kbase_pm_is_in_desired_state(struct kbase_device *kbdev)
{
	bool in_desired_state;
	unsigned long flags;

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	in_desired_state = kbase_pm_is_in_desired_state_nolock(kbdev);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	return in_desired_state;
}

static bool kbase_pm_is_in_desired_state_with_l2_powered(struct kbase_device *kbdev)
{
	bool in_desired_state = false;
	unsigned long flags;

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	if (kbase_pm_is_in_desired_state_nolock(kbdev) &&
	    (kbdev->pm.backend.l2_state == KBASE_L2_ON))
		in_desired_state = true;
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	return in_desired_state;
}

#if !MALI_USE_CSF
static void kbase_pm_trace_power_state(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->hwaccess_lock);

	{
		struct kbase_pm_event_log_event *event =
				kbase_pm_add_log_event(kbdev);
		event->type = KBASE_PM_LOG_EVENT_CORES;
		event->cores.l2 = kbase_pm_get_state(
				kbdev, KBASE_PM_CORE_L2, ACTION_READY);
		event->cores.shader = kbase_pm_get_state(
				kbdev, KBASE_PM_CORE_SHADER, ACTION_READY);
		event->cores.tiler = kbase_pm_get_state(
				kbdev, KBASE_PM_CORE_TILER, ACTION_READY);
		if (corestack_driver_control) {
			event->cores.stack = kbase_pm_get_state(
					kbdev, KBASE_PM_CORE_STACK, ACTION_READY);
		}
	}

	KBASE_TLSTREAM_AUX_PM_STATE(kbdev, TL_PM_STATE_L2,
				    kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_L2));
	KBASE_TLSTREAM_AUX_PM_STATE(kbdev, TL_PM_STATE_SHADER,
				    kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_SHADER));
	KBASE_TLSTREAM_AUX_PM_STATE(kbdev, TL_PM_STATE_TILER,
				    kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_TILER));

	if (corestack_driver_control)
		KBASE_TLSTREAM_AUX_PM_STATE(kbdev, TL_PM_STATE_STACK,
					    kbase_pm_get_ready_cores(kbdev, KBASE_PM_CORE_STACK));
}
#endif

void kbase_pm_update_state(struct kbase_device *kbdev)
{
#if !MALI_USE_CSF
	enum kbase_shader_core_state prev_shaders_state = kbdev->pm.backend.shaders_state;
#else
	enum kbase_mcu_state prev_mcu_state = kbdev->pm.backend.mcu_state;
#endif

	ATRACE_BEGIN(__func__);

	lockdep_assert_held(&kbdev->hwaccess_lock);

	if (!kbdev->pm.backend.gpu_ready) {
		ATRACE_END();
		return; /* Do nothing if the GPU is not ready */
	}

	if (kbase_pm_l2_update_state(kbdev)) {
		ATRACE_END();
		return;
	}

#if !MALI_USE_CSF
	if (kbase_pm_shaders_update_state(kbdev)) {
		ATRACE_END();
		return;
	}

	/* If the shaders just turned off, re-invoke the L2 state machine, in
	 * case it was waiting for the shaders to turn off before powering down
	 * the L2.
	 */
	if (prev_shaders_state != KBASE_SHADERS_OFF_CORESTACK_OFF &&
	    kbdev->pm.backend.shaders_state == KBASE_SHADERS_OFF_CORESTACK_OFF) {
		if (kbase_pm_l2_update_state(kbdev)) {
			ATRACE_END();
			return;
		}
	}
#else
	if (kbase_pm_mcu_update_state(kbdev)) {
		ATRACE_END();
		return;
	}

	if (!kbase_pm_is_mcu_inactive(kbdev, prev_mcu_state) &&
	    kbase_pm_is_mcu_inactive(kbdev, kbdev->pm.backend.mcu_state)) {
		if (kbase_pm_l2_update_state(kbdev)) {
			ATRACE_END();
			return;
		}
	}
#endif

	if (kbase_pm_is_in_desired_state_nolock(kbdev)) {
		KBASE_KTRACE_ADD(kbdev, PM_DESIRED_REACHED, NULL, kbdev->pm.backend.shaders_avail);

#if !MALI_USE_CSF
		kbase_pm_trace_power_state(kbdev);
#endif

		KBASE_KTRACE_ADD(kbdev, PM_WAKE_WAITERS, NULL, 0);
		wake_up(&kbdev->pm.backend.gpu_in_desired_state_wait);
	}

	ATRACE_END();
}
KBASE_EXPORT_TEST_API(kbase_pm_update_state);

static enum hrtimer_restart shader_tick_timer_callback(struct hrtimer *timer)
{
	struct kbasep_pm_tick_timer_state *stt =
		container_of(timer, struct kbasep_pm_tick_timer_state, timer);
	struct kbase_device *kbdev =
		container_of(stt, struct kbase_device, pm.backend.shader_tick_timer);
	struct kbase_pm_backend_data *backend = &kbdev->pm.backend;
	unsigned long flags;
	enum hrtimer_restart restart = HRTIMER_NORESTART;

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);

	if (stt->remaining_ticks && backend->shaders_state == KBASE_SHADERS_WAIT_OFF_CORESTACK_ON) {
		stt->remaining_ticks--;

		/* If the remaining ticks just changed from 1 to 0, invoke the
		 * PM state machine to power off the shader cores.
		 */
		if (!stt->remaining_ticks && !backend->shaders_desired)
			kbase_pm_update_state(kbdev);
	}

	if (stt->needed) {
		hrtimer_forward_now(timer, stt->configured_interval);
		restart = HRTIMER_RESTART;
	}

	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	return restart;
}

int kbase_pm_state_machine_init(struct kbase_device *kbdev)
{
	struct kbasep_pm_tick_timer_state *stt = &kbdev->pm.backend.shader_tick_timer;

	stt->wq = alloc_workqueue("kbase_pm_shader_poweroff", WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!stt->wq)
		return -ENOMEM;

	INIT_WORK(&stt->work, shader_poweroff_timer_stop_callback);

	hrtimer_init(&stt->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	stt->timer.function = shader_tick_timer_callback;
	stt->configured_interval = HR_TIMER_DELAY_NSEC(DEFAULT_PM_GPU_POWEROFF_TICK_NS);
	stt->default_ticks = DEFAULT_PM_POWEROFF_TICK_SHADER;
	stt->configured_ticks = stt->default_ticks;

#if MALI_USE_CSF
	if (kbdev->pm.backend.has_host_pwr_iface) {
		const u64 neural_present = kbase_pm_get_present_cores(kbdev, KBASE_PM_CORE_NEURAL);
		const u64 shader_present = kbase_pm_get_present_cores(kbdev, KBASE_PM_CORE_SHADER);
		int err = -EINVAL;

		if (kbase_csf_dev_has_ne(kbdev)) {
			if (!neural_present)
				dev_err(kbdev->dev,
					"NEURAL_PRESENT 0 when device reports NE cores");
			else if ((neural_present & shader_present) != neural_present)
				dev_err(kbdev->dev,
					"Detected NE core without a corresponding shader core: NEURAL_PRESENT %llx SHADER_PRESENT %llx",
					neural_present, shader_present);
			else
				err = 0;
		} else {
			if (neural_present)
				dev_err(kbdev->dev, "Unexpectedly, NEURAL_PRESENT %llx is not zero",
					neural_present);
			else
				err = 0;
		}

		if (err) {
			destroy_workqueue(stt->wq);
			return -EINVAL;
		}

		VALIDATE_ALL_POWER_REG_ENUMS;
	}

	kbdev->pm.backend.core_idle_wq = alloc_workqueue("coreoff_wq", WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!kbdev->pm.backend.core_idle_wq) {
		destroy_workqueue(stt->wq);
		return -ENOMEM;
	}

	INIT_WORK(&kbdev->pm.backend.core_idle_work, core_idle_worker);
#endif

	return 0;
}

void kbase_pm_state_machine_term(struct kbase_device *kbdev)
{
#if MALI_USE_CSF
	destroy_workqueue(kbdev->pm.backend.core_idle_wq);
#endif
	hrtimer_cancel(&kbdev->pm.backend.shader_tick_timer.timer);
	destroy_workqueue(kbdev->pm.backend.shader_tick_timer.wq);
}

void kbase_pm_reset_start_locked(struct kbase_device *kbdev)
{
	struct kbase_pm_backend_data *backend = &kbdev->pm.backend;

	lockdep_assert_held(&kbdev->hwaccess_lock);

	backend->in_reset = true;
	backend->l2_state = KBASE_L2_RESET_WAIT;
	KBASE_KTRACE_ADD(kbdev, PM_L2_RESET_WAIT, NULL, backend->l2_state);
#if !MALI_USE_CSF
	backend->shaders_state = KBASE_SHADERS_RESET_WAIT;
#else
	/* MCU state machine is exercised only after the initial load/boot
	 * of the firmware.
	 */
	if (likely(kbdev->csf.firmware_inited)) {
		backend->mcu_state = KBASE_MCU_RESET_WAIT;
		KBASE_KTRACE_ADD(kbdev, PM_MCU_RESET_WAIT, NULL, backend->mcu_state);
#ifdef KBASE_PM_RUNTIME
		backend->exit_gpu_sleep_mode = true;
#endif
		kbdev->csf.firmware_reload_needed = true;
	} else {
		WARN_ON(backend->mcu_state != KBASE_MCU_OFF);
	}
#endif

	/* We're in a reset, so hwcnt will have been synchronously disabled by
	 * this function's caller as part of the reset process. We therefore
	 * know that any call to kbase_hwcnt_context_disable_atomic, if
	 * required to sync the hwcnt refcount with our internal state, is
	 * guaranteed to succeed.
	 */
	backend->hwcnt_desired = false;
	if (!backend->hwcnt_disabled) {
		WARN_ON(!kbase_hwcnt_context_disable_atomic(kbdev->hwcnt_gpu_ctx));
		backend->hwcnt_disabled = true;
	}

	shader_poweroff_timer_queue_cancel(kbdev);
}

void kbase_pm_reset_complete(struct kbase_device *kbdev)
{
	struct kbase_pm_backend_data *backend = &kbdev->pm.backend;
	unsigned long flags;

	if (!kbase_io_is_gpu_lost(kbdev))
		WARN_ON(!kbase_reset_gpu_is_active(kbdev));
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);

	/* As GPU has just been reset, that results in implicit flush of L2
	 * cache, can safely mark the pending cache flush operation (if there
	 * was any) as complete and unblock the waiter.
	 * No work can be submitted whilst GPU reset is ongoing.
	 */
	kbase_gpu_cache_clean_wait_complete(kbdev);
	backend->in_reset = false;
#if MALI_USE_CSF && defined(KBASE_PM_RUNTIME)
	backend->gpu_wakeup_override = false;
	backend->db_mirror_interrupt_enabled = false;
	backend->gpu_sleep_mode_active = false;
	backend->exit_gpu_sleep_mode = false;
#endif
	kbase_pm_update_state(kbdev);

	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
}

#if !MALI_USE_CSF
/* Timeout in milliseconds for GPU Power Management to reach the desired
 * Shader and L2 state. If the time spent waiting has exceeded this threshold
 * then there is most likely a hardware issue.
 */
#define PM_TIMEOUT_MS (5000 * KBASE_TIMEOUT_MULTIPLIER) /* 5s */
#endif

void kbase_gpu_timeout_debug_message(struct kbase_device *kbdev, const char *timeout_msg)
{
	unsigned long flags;

	dev_err(kbdev->dev, "[%lluns] %s", ktime_get_raw_ns(), timeout_msg);
#if !MALI_USE_CSF
	CSTD_UNUSED(flags);
	dev_err(kbdev->dev, "Desired state :\n");
	dev_err(kbdev->dev, "\tShader=%016llx\n",
		kbdev->pm.backend.shaders_desired ? kbdev->pm.backend.shaders_avail : 0);
#else
	dev_err(kbdev->dev, "GPU pm state :\n");
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	dev_err(kbdev->dev, "\tpm.active_count = %d", kbdev->pm.active_count);
	dev_err(kbdev->dev, "\tpoweron_required %d pm.active_count %d invoke_poweroff_wait_wq_when_l2_off %d",
		kbdev->pm.backend.poweron_required,
		kbdev->pm.active_count,
		kbdev->pm.backend.invoke_poweroff_wait_wq_when_l2_off);
	dev_err(kbdev->dev, "\tgpu_poweroff_wait_work pending %d",
		work_pending(&kbdev->pm.backend.gpu_poweroff_wait_work));
	dev_err(kbdev->dev, "\tMCU desired = %d\n", kbase_pm_is_mcu_desired(kbdev));
	dev_err(kbdev->dev, "\tMCU sw state = %d\n", kbdev->pm.backend.mcu_state);
	dev_err(kbdev->dev, "\tL2 desired = %d (locked_off: %d)\n",
		kbase_pm_is_l2_desired(kbdev), kbdev->pm.backend.policy_change_clamp_state_to_off);
	dev_err(kbdev->dev, "\tL2 sw state = %d\n",
		kbdev->pm.backend.l2_state);
	gpu_debug_dump_pdc_status(kbdev);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
#endif
	dev_err(kbdev->dev, "Current state :\n");
	dev_err(kbdev->dev, "\tShader=%016llx\n",
		kbase_reg_read64(kbdev, GPU_CONTROL_ENUM(SHADER_READY)));
	dev_err(kbdev->dev, "\tTiler =%016llx\n",
		kbase_reg_read64(kbdev, GPU_CONTROL_ENUM(TILER_READY)));
	dev_err(kbdev->dev, "\tL2    =%016llx\n",
		kbase_reg_read64(kbdev, GPU_CONTROL_ENUM(L2_READY)));
#if MALI_USE_CSF
	kbase_csf_debug_dump_registers(kbdev);
#endif
	dev_err(kbdev->dev, "Cores transitioning :\n");
	dev_err(kbdev->dev, "\tShader=%016llx\n",
		kbase_reg_read64(kbdev, GPU_CONTROL_ENUM(SHADER_PWRTRANS)));
	dev_err(kbdev->dev, "\tTiler =%016llx\n",
		kbase_reg_read64(kbdev, GPU_CONTROL_ENUM(TILER_PWRTRANS)));
	dev_err(kbdev->dev, "\tL2    =%016llx\n",
		kbase_reg_read64(kbdev, GPU_CONTROL_ENUM(L2_PWRTRANS)));

	dump_stack();
}

static void kbase_pm_timed_out(struct kbase_device *kbdev, const char *timeout_msg)
{
	kbase_gpu_timeout_debug_message(kbdev, timeout_msg);
#if IS_ENABLED(CONFIG_SOC_GS201)
	struct device_node *dpm = of_find_node_by_name(NULL, "dpm");
	const char *variant = NULL;

	if (dpm && !of_property_read_string(dpm, "variant", &variant) &&
	    strcmp(variant, "user")) {
		/* pixel : b/286061575: panic on gs201 non-user builds. */
		panic("b/286061575: mali: kbase_pm_timed_out\n");
	}
#endif
	/* pixel: If either:
	 *   1. L2/MCU power transition timed out, or,
	 *   2. kbase state machine fell out of sync with the hw state,
	 * a soft/hard reset (ie writing to SOFT/HARD_RESET regs) is insufficient to resume
	 * operation.
	 *
	 * Besides, Odin TRM advises against touching SOFT/HARD_RESET
	 * regs if L2_PWRTRANS is 1 to avoid undefined state.
	 *
	 * We have already lost work if we end up here, so send a powercycle to reset the hw,
	 * which is more reliable.
	 */
	dev_err(kbdev->dev, "Sending reset to GPU - all running jobs will be lost\n");
	if (kbase_prepare_to_reset_gpu(kbdev,
				       RESET_FLAGS_HWC_UNRECOVERABLE_ERROR |
				       RESET_FLAGS_FORCE_PM_HW_RESET))
		kbase_reset_gpu(kbdev);
}

int kbase_pm_wait_for_l2_powered(struct kbase_device *kbdev)
{
	unsigned long flags;
	unsigned long timeout;
	long remaining;
	int err = 0;

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	kbase_pm_update_state(kbdev);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

#if MALI_USE_CSF
	timeout = (unsigned long)kbase_csf_timeout_in_jiffies(
		kbase_get_timeout_ms(kbdev, CSF_PM_TIMEOUT));
#else
	timeout = msecs_to_jiffies(PM_TIMEOUT_MS);
#endif

	/* Wait for cores */
#if KERNEL_VERSION(4, 13, 1) <= LINUX_VERSION_CODE
	remaining = wait_event_killable_timeout(kbdev->pm.backend.gpu_in_desired_state_wait,
						kbase_pm_is_in_desired_state_with_l2_powered(kbdev),
						(long)timeout);
#else
	remaining = wait_event_timeout(kbdev->pm.backend.gpu_in_desired_state_wait,
				       kbase_pm_is_in_desired_state_with_l2_powered(kbdev),
				       (long)timeout);
#endif

	if (!remaining) {
		pixel_gpu_uevent_kmd_error_send(kbdev, GPU_UEVENT_INFO_L2_PM_TIMEOUT);
		kbase_pm_timed_out(kbdev, "Wait for desired PM state with L2 powered timed out");
		err = -ETIMEDOUT;
	} else if (remaining < 0) {
		dev_info(kbdev->dev, "Wait for desired PM state with L2 powered got interrupted");
		err = (int)remaining;
	}

	return err;
}

static int pm_wait_for_desired_state(struct kbase_device *kbdev, bool killable_wait)
{
	unsigned long flags;
	long remaining;
#if MALI_USE_CSF
	long timeout = kbase_csf_timeout_in_jiffies(kbase_get_timeout_ms(kbdev, CSF_PM_TIMEOUT));
#else
	long timeout = (long)msecs_to_jiffies(PM_TIMEOUT_MS);
#endif
	int err = 0;

	/* Let the state machine latch the most recent desired state. */
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	kbase_pm_update_state(kbdev);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	/* Wait for cores */
#if KERNEL_VERSION(4, 13, 1) <= LINUX_VERSION_CODE
	if (killable_wait)
		remaining = wait_event_killable_timeout(kbdev->pm.backend.gpu_in_desired_state_wait,
							kbase_pm_is_in_desired_state(kbdev),
							timeout);
#else
	killable_wait = false;
#endif
	if (!killable_wait)
		remaining = wait_event_timeout(kbdev->pm.backend.gpu_in_desired_state_wait,
					       kbase_pm_is_in_desired_state(kbdev), timeout);
	if (!remaining) {
		pixel_gpu_uevent_kmd_error_send(kbdev, GPU_UEVENT_INFO_PM_TIMEOUT);
		kbase_pm_timed_out(kbdev, "Wait for power transition timed out");
		err = -ETIMEDOUT;
	} else if (remaining < 0) {
		WARN_ON_ONCE(!killable_wait);
		dev_info(kbdev->dev, "Wait for power transition got interrupted");
		err = (int)remaining;
	}

	return err;
}

int kbase_pm_killable_wait_for_desired_state(struct kbase_device *kbdev)
{
	return pm_wait_for_desired_state(kbdev, true);
}

int kbase_pm_wait_for_desired_state(struct kbase_device *kbdev)
{
	return pm_wait_for_desired_state(kbdev, false);
}
KBASE_EXPORT_TEST_API(kbase_pm_wait_for_desired_state);

#if MALI_USE_CSF
/**
 * core_mask_update_done - Check if downscaling of shader cores is done
 *
 * @kbdev: The kbase device structure for the device.
 *
 * This function checks if the downscaling of cores is effectively complete.
 *
 * Return: true if the downscale is done.
 */
static bool core_mask_update_done(struct kbase_device *kbdev)
{
	bool update_done = false;
	unsigned long flags;

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	/* If MCU is in stable ON state then it implies that the downscale
	 * request had completed.
	 * If MCU is not active then it implies all cores are off, so can
	 * consider the downscale request as complete.
	 */
	if ((kbdev->pm.backend.mcu_state == KBASE_MCU_ON) ||
	    kbase_pm_is_mcu_inactive(kbdev, kbdev->pm.backend.mcu_state))
		update_done = true;
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	return update_done;
}

int kbase_pm_wait_for_cores_down_scale(struct kbase_device *kbdev)
{
	long timeout = kbase_csf_timeout_in_jiffies(kbase_get_timeout_ms(kbdev, CSF_PM_TIMEOUT));
	long remaining;
	int err = 0;

	/* Wait for core mask update to complete  */
#if KERNEL_VERSION(4, 13, 1) <= LINUX_VERSION_CODE
	remaining = wait_event_killable_timeout(kbdev->pm.backend.gpu_in_desired_state_wait,
						core_mask_update_done(kbdev), timeout);
#else
	remaining = wait_event_timeout(kbdev->pm.backend.gpu_in_desired_state_wait,
				       core_mask_update_done(kbdev), timeout);
#endif

	if (!remaining) {
		kbase_pm_timed_out(kbdev, "Wait for cores down scaling timed out");
		err = -ETIMEDOUT;
	} else if (remaining < 0) {
		dev_info(kbdev->dev, "Wait for cores down scaling got interrupted");
		err = (int)remaining;
	}

	return err;
}
#endif

static bool is_poweroff_wait_in_progress(struct kbase_device *kbdev)
{
	bool ret;
	unsigned long flags;

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	ret = kbdev->pm.backend.poweroff_wait_in_progress;
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	return ret;
}

static int pm_wait_for_poweroff_work_complete(struct kbase_device *kbdev, bool killable_wait)
{
	long remaining;
	/* Indefinite wait needed when arbitration is enabled */
#if MALI_USE_CSF
	/* gpu_poweroff_wait_work would be subjected to the kernel scheduling
	 * and so the wait time can't only be the function of GPU frequency.
	 */
	const unsigned int extra_wait_time_ms = 2000;
	const long timeout =
		kbase_has_arbiter(kbdev) ?
			      MAX_SCHEDULE_TIMEOUT :
			      kbase_csf_timeout_in_jiffies(
						     kbase_get_timeout_ms(kbdev, CSF_PM_TIMEOUT) +
						     extra_wait_time_ms);
#else
	const long timeout = kbase_has_arbiter(kbdev) ? MAX_SCHEDULE_TIMEOUT :
							      (long)msecs_to_jiffies(PM_TIMEOUT_MS);
#endif
	int err = 0;

#if KERNEL_VERSION(4, 13, 1) <= LINUX_VERSION_CODE
	if (killable_wait)
		remaining = wait_event_killable_timeout(kbdev->pm.backend.poweroff_wait,
							!is_poweroff_wait_in_progress(kbdev),
							timeout);
#else
	killable_wait = false;
#endif

	if (!killable_wait)
		remaining = wait_event_timeout(kbdev->pm.backend.poweroff_wait,
					       !is_poweroff_wait_in_progress(kbdev), timeout);
	if (!remaining) {
		/* If work is now pending, kbase_pm_gpu_poweroff_wait_wq() will
		 * definitely be called, so it's safe to continue waiting for it.
		 */
		if (work_pending(&kbdev->pm.backend.gpu_poweroff_wait_work)) {
			wait_event_killable(kbdev->pm.backend.poweroff_wait,
			                    !is_poweroff_wait_in_progress(kbdev));
		} else {
			unsigned long flags;
			kbasep_platform_event_core_dump(kbdev, "poweroff work timeout");
			kbase_gpu_timeout_debug_message(kbdev, "failed to wait for poweroff worker");
#if MALI_USE_CSF
			//csf.scheduler.state should be accessed with scheduler lock!
			//callchains go through this function though holding that lock
			//so just print without locking.
			dev_err(kbdev->dev, "scheduler.state %d", kbdev->csf.scheduler.state);
			dev_err(kbdev->dev, "Firmware ping %d", kbase_csf_firmware_ping_wait(kbdev, 0));
#endif
			//Attempt another state machine transition prompt.
			dev_err(kbdev->dev, "Attempt to prompt state machine");
			spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
			kbase_pm_update_state(kbdev);
			spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

			kbase_gpu_timeout_debug_message(kbdev, "GPU state after re-prompt of state machine");
			err = -ETIMEDOUT;
		}
	} else if (remaining < 0) {
		WARN_ON_ONCE(!killable_wait);
		dev_info(kbdev->dev, "Wait for poweroff work got interrupted");
		err = (int)remaining;
	}
	return err;
}

int kbase_pm_killable_wait_for_poweroff_work_complete(struct kbase_device *kbdev)
{
	return pm_wait_for_poweroff_work_complete(kbdev, true);
}

int kbase_pm_wait_for_poweroff_work_complete(struct kbase_device *kbdev)
{
	return pm_wait_for_poweroff_work_complete(kbdev, false);
}
KBASE_EXPORT_TEST_API(kbase_pm_wait_for_poweroff_work_complete);

void kbase_pm_enable_interrupts(struct kbase_device *kbdev)
{
	unsigned long flags;
	bool is_legacy_gpu_irq_mask;

	ATRACE_BEGIN(__func__);
	KBASE_DEBUG_ASSERT(kbdev != NULL);
#if MALI_USE_CSF
	is_legacy_gpu_irq_mask = !kbdev->pm.backend.has_host_pwr_iface;
#else
	is_legacy_gpu_irq_mask = true;
#endif

	/*
	 * Clear all interrupts,
	 * and unmask them all.
	 */
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_IRQ_CLEAR),
			  kbase_reg_gpu_irq_all(is_legacy_gpu_irq_mask));
	kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_IRQ_MASK),
			  kbase_reg_gpu_irq_all(is_legacy_gpu_irq_mask));

	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	kbase_reg_write32(kbdev, JOB_CONTROL_ENUM(JOB_IRQ_CLEAR), 0xFFFFFFFF);
	kbase_reg_write32(kbdev, JOB_CONTROL_ENUM(JOB_IRQ_MASK), 0xFFFFFFFF);

	kbase_reg_write32(kbdev, MMU_CONTROL_ENUM(IRQ_CLEAR), 0xFFFFFFFF);
#if MALI_USE_CSF
	/* Enable only the Page fault bits part */
	kbase_reg_write32(kbdev, MMU_CONTROL_ENUM(IRQ_MASK), 0xFFFF);
#else
	kbase_reg_write32(kbdev, MMU_CONTROL_ENUM(IRQ_MASK), 0xFFFFFFFF);
#endif
	ATRACE_END();

#if MALI_USE_CSF
	if (kbdev->pm.backend.has_host_pwr_iface) {
		kbase_reg_write32(kbdev, HOST_POWER_ENUM(PWR_IRQ_CLEAR), PWR_IRQ_REG_ALL);
		kbase_reg_write32(kbdev, HOST_POWER_ENUM(PWR_IRQ_MASK), PWR_IRQ_REG_ALL);
	}
#endif
}

KBASE_EXPORT_TEST_API(kbase_pm_enable_interrupts);

void kbase_pm_disable_interrupts_nolock(struct kbase_device *kbdev)
{
	bool is_legacy_gpu_irq_mask;

	KBASE_DEBUG_ASSERT(kbdev != NULL);
#if MALI_USE_CSF
	is_legacy_gpu_irq_mask = !kbdev->pm.backend.has_host_pwr_iface;
#else
	is_legacy_gpu_irq_mask = true;
#endif
	/*
	 * Mask all interrupts,
	 * and clear them all.
	 */
	lockdep_assert_held(&kbdev->hwaccess_lock);

	kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_IRQ_MASK), 0);
	kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_IRQ_CLEAR),
			  kbase_reg_gpu_irq_all(is_legacy_gpu_irq_mask));

	kbase_reg_write32(kbdev, JOB_CONTROL_ENUM(JOB_IRQ_MASK), 0);
	kbase_reg_write32(kbdev, JOB_CONTROL_ENUM(JOB_IRQ_CLEAR), 0xFFFFFFFF);

	kbase_reg_write32(kbdev, MMU_CONTROL_ENUM(IRQ_MASK), 0);
	kbase_reg_write32(kbdev, MMU_CONTROL_ENUM(IRQ_CLEAR), 0xFFFFFFFF);

#if MALI_USE_CSF
	if (kbdev->pm.backend.has_host_pwr_iface) {
		kbase_reg_write32(kbdev, HOST_POWER_ENUM(PWR_IRQ_MASK), 0);
		kbase_reg_write32(kbdev, HOST_POWER_ENUM(PWR_IRQ_CLEAR), PWR_IRQ_REG_ALL);
	}
#endif
}

void kbase_pm_disable_interrupts(struct kbase_device *kbdev)
{
	unsigned long flags;

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	kbase_pm_disable_interrupts_nolock(kbdev);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
}

KBASE_EXPORT_TEST_API(kbase_pm_disable_interrupts);

#if MALI_USE_CSF
/**
 * update_user_reg_page_mapping - Update the mapping for USER Register page
 *
 * @kbdev: The kbase device structure for the device.
 *
 * This function must be called to unmap the dummy or real page from USER Register page
 * mapping whenever GPU is powered up or down. The dummy or real page would get
 * appropriately mapped in when Userspace reads the LATEST_FLUSH value.
 */
static void update_user_reg_page_mapping(struct kbase_device *kbdev)
{
	struct kbase_context *kctx, *n;

	ATRACE_BEGIN(__func__);
	lockdep_assert_held(&kbdev->pm.lock);

	mutex_lock(&kbdev->csf.reg_lock);
	list_for_each_entry_safe(kctx, n, &kbdev->csf.user_reg.list, csf.user_reg.link) {
		/* This would zap the PTE corresponding to the mapping of User
		 * Register page of the kbase context. The mapping will be reestablished
		 * when the context (user process) needs to access to the page.
		 */
		unmap_mapping_range(kbdev->csf.user_reg.filp->f_inode->i_mapping,
				    (loff_t)kctx->csf.user_reg.file_offset << PAGE_SHIFT, PAGE_SIZE,
				    1);
		list_del_init(&kctx->csf.user_reg.link);
		dev_dbg(kbdev->dev, "Updated USER Reg page mapping of ctx %d_%d", kctx->tgid,
			kctx->id);
	}
	mutex_unlock(&kbdev->csf.reg_lock);
	ATRACE_END();
}
#endif

/*
 * pmu layout:
 * 0x0000: PMU TAG (RO) (0xCAFECAFE)
 * 0x0004: PMU VERSION ID (RO) (0x00000000)
 * 0x0008: CLOCK ENABLE (RW) (31:1 SBZ, 0 CLOCK STATE)
 */
void kbase_pm_clock_on(struct kbase_device *kbdev, bool is_resume)
{
	struct kbase_pm_backend_data *backend = &kbdev->pm.backend;
	int ret = is_resume;
	unsigned long flags;

	ATRACE_BEGIN(__func__);
	KBASE_DEBUG_ASSERT(kbdev != NULL);
#if !MALI_USE_CSF
	lockdep_assert_held(&kbdev->js_data.runpool_mutex);
#endif /* !MALI_USE_CSF */
	lockdep_assert_held(&kbdev->pm.lock);

	if (WARN_ON(kbase_io_is_gpu_lost(kbdev))) {
		dev_err(kbdev->dev, "%s: Cannot power up while GPU lost", __func__);
		ATRACE_END();
		return;
	}

	if (kbase_io_is_gpu_powered(kbdev)) {
#if MALI_USE_CSF && defined(KBASE_PM_RUNTIME)
		if (backend->gpu_idled) {
			backend->callback_power_runtime_gpu_active(kbdev);
			backend->gpu_idled = false;
		}
#endif
		/* Already turned on */
		if (kbdev->poweroff_pending)
			kbase_pm_enable_interrupts(kbdev);
		kbdev->poweroff_pending = false;
		KBASE_DEBUG_ASSERT(!is_resume);
		ATRACE_END();
		return;
	}

	kbdev->poweroff_pending = false;

	KBASE_KTRACE_ADD(kbdev, PM_GPU_ON, NULL, 0u);

	if (is_resume && backend->callback_power_resume) {
		backend->callback_power_resume(kbdev);
		ATRACE_END();
		return;
	} else if (backend->callback_power_on) {
		ret = backend->callback_power_on(kbdev);
	}

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	kbase_io_clear_status(kbdev->io, KBASE_IO_STATUS_GPU_OFF);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

#if MALI_USE_CSF
	/* GPU has been turned on, can switch to actual register page */
	update_user_reg_page_mapping(kbdev);
#endif


	if (ret == GPU_STATE_IN_RESET) {
		/* GPU is already in reset state after power on and no
		 * soft-reset needed. Just reconfiguration is needed.
		 */
		kbase_pm_init_hw(kbdev, PM_ENABLE_IRQS | PM_NO_RESET);
	} else if (ret == GPU_STATE_LOST) {
		/* GPU state was lost, reset GPU to ensure it is in a
		 * consistent state
		 */
		kbase_pm_init_hw(kbdev, PM_ENABLE_IRQS);
	} else {
		if (kbase_has_arbiter(kbdev)) {
			struct kbase_arbiter_vm_state *arb_vm_state = kbdev->pm.arb_vm_state;

			/* In the case that the GPU has just been granted by
			 * the Arbiter, a reset will have already been done.
			 * However, it is still necessary to initialize the GPU.
			 */
			if (arb_vm_state->vm_arb_starting)
				kbase_pm_init_hw(kbdev, PM_ENABLE_IRQS | PM_NO_RESET);
		}
	}
	/*
	 * This point means that the GPU trasitioned to ON. So there is a chance
	 * that a repartitioning occurred. In this case the current config
	 * should be read again.
	 */
	if (kbase_has_arbiter(kbdev))
		kbase_gpuprops_get_curr_config_props(kbdev, &kbdev->gpu_props.curr_config);

	mutex_lock(&kbdev->mmu_hw_mutex);
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	kbase_ctx_sched_restore_all_as(kbdev);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
	mutex_unlock(&kbdev->mmu_hw_mutex);

	if (kbdev->dummy_job_wa.flags & KBASE_DUMMY_JOB_WA_FLAG_LOGICAL_SHADER_POWER) {
		spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
		kbase_dummy_job_wa_execute(kbdev,
					   kbase_pm_get_present_cores(kbdev, KBASE_PM_CORE_SHADER));
		spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
	}

	/* Enable the interrupts */
	kbase_pm_enable_interrupts(kbdev);

	/* Turn on the L2 caches */
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	backend->gpu_ready = true;
	backend->l2_desired = true;
#if MALI_USE_CSF
	{
		if (ret != GPU_STATE_INTACT) {
			/* GPU reset was done after the power on, so send the post
			 * reset event instead. This is okay as GPU power off event
			 * is same as pre GPU reset event.
			 */
			kbase_ipa_control_handle_gpu_reset_post(kbdev);
		} else {
			kbase_ipa_control_handle_gpu_power_on(kbdev);
		}
	}
#endif
	kbase_pm_update_state(kbdev);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

#if MALI_USE_CSF && defined(KBASE_PM_RUNTIME)
	/* GPU is now powered up. Invoke the GPU active callback as GPU idle
	 * callback would have been invoked before the power down.
	 */
	if (backend->gpu_idled) {
		backend->callback_power_runtime_gpu_active(kbdev);
		backend->gpu_idled = false;
	}
#endif

	ATRACE_END();
}

KBASE_EXPORT_TEST_API(kbase_pm_clock_on);

bool kbase_pm_clock_off(struct kbase_device *kbdev)
{
	unsigned long flags;

	KBASE_DEBUG_ASSERT(kbdev != NULL);
	lockdep_assert_held(&kbdev->pm.lock);

	/* ASSERT that the cores should now be unavailable. No lock needed. */
	WARN_ON(kbdev->pm.backend.shaders_state != KBASE_SHADERS_OFF_CORESTACK_OFF);

	kbdev->poweroff_pending = true;

	if (!kbase_io_is_gpu_powered(kbdev)) {
		/* Already turned off */
		return true;
	}

	KBASE_KTRACE_ADD(kbdev, PM_GPU_OFF, NULL, 0u);

	/* Disable interrupts. This also clears any outstanding interrupts */
	kbase_pm_disable_interrupts(kbdev);
	/* Ensure that any IRQ handlers have finished */
	kbase_synchronize_irqs(kbdev);

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);

	if (atomic_read(&kbdev->faults_pending)) {
		/* Page/bus faults are still being processed. The GPU can not
		 * be powered off until they have completed
		 */
		spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
		return false;
	}

	kbase_pm_cache_snoop_disable(kbdev);
#if MALI_USE_CSF
	{
		kbase_ipa_control_handle_gpu_power_off(kbdev);
	}
#endif

	if (kbase_io_is_gpu_lost(kbdev)) {
		/* Ensure we unblock any threads that are stuck waiting
		 * for the GPU
		 */
		kbase_gpu_cache_clean_wait_complete(kbdev);
	}

	kbdev->pm.backend.gpu_ready = false;

	/* The GPU power may be turned off from this point */
	kbase_io_set_status(kbdev->io, KBASE_IO_STATUS_GPU_OFF);

	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

#if MALI_USE_CSF
	/* GPU is about to be turned off, switch to dummy page */
	update_user_reg_page_mapping(kbdev);
#endif
	kbase_arbiter_pm_vm_event(kbdev, KBASE_VM_GPU_IDLE_EVENT);

	if (kbdev->pm.backend.callback_power_off)
		kbdev->pm.backend.callback_power_off(kbdev);
	return true;
}

KBASE_EXPORT_TEST_API(kbase_pm_clock_off);

struct kbasep_reset_timeout_data {
	struct hrtimer timer;
	bool timed_out;
	struct kbase_device *kbdev;
};

void kbase_pm_reset_done(struct kbase_device *kbdev)
{
	KBASE_DEBUG_ASSERT(kbdev != NULL);
	kbdev->pm.backend.reset_done = true;
	wake_up(&kbdev->pm.backend.reset_done_wait);
}

/**
 * kbase_pm_wait_for_reset - Wait for a reset to happen
 *
 * @kbdev: Kbase device
 *
 * Wait for the %RESET_COMPLETED IRQ to occur, then reset the waiting state.
 */
static void kbase_pm_wait_for_reset(struct kbase_device *kbdev)
{
	lockdep_assert_held(&kbdev->pm.lock);

	wait_event(kbdev->pm.backend.reset_done_wait, (kbdev->pm.backend.reset_done));
	kbdev->pm.backend.reset_done = false;
	atomic_set(&kbdev->pm.backend.reset_in_progress, 0);
}

KBASE_EXPORT_TEST_API(kbase_pm_reset_done);

static enum hrtimer_restart kbasep_reset_timeout(struct hrtimer *timer)
{
	struct kbasep_reset_timeout_data *rtdata =
		container_of(timer, struct kbasep_reset_timeout_data, timer);

	rtdata->timed_out = true;

	/* Set the wait queue to wake up kbase_pm_init_hw even though the reset
	 * hasn't completed
	 */
	kbase_pm_reset_done(rtdata->kbdev);

	return HRTIMER_NORESTART;
}

#if MALI_USE_CSF
/**
 * kbase_ne_control_apply - Apply DT values to NEURAL_CONTROL register
 *
 * @kbdev: Kbase device
 *
 * Set mac_step_cycles register value from DT if present.
 *
 * Return: 0 on success, or error code
 */
static int kbase_ne_control_apply(struct kbase_device *kbdev)
{
	const u32 default_mac_step_cycles = 0;
	u32 mac_step_cycles = 0;
	u32 reg_val;

	if (of_property_read_u32(kbdev->dev->of_node, "mac-step-cycles", &mac_step_cycles))
		return 0;

	if (mac_step_cycles > NEURAL_CONTROL_MAC_STEP_CYCLES_MAX_VALUE) {
		dev_warn(kbdev->dev,
			 "mac-step-cycles value (%d) is too large - using default value of %d",
			 mac_step_cycles, default_mac_step_cycles);
		mac_step_cycles = default_mac_step_cycles;
	}

	if (kbase_reg_is_valid(kbdev, GPU_CONTROL_ENUM(NEURAL_CONTROL))) {
		reg_val = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(NEURAL_CONTROL));
		reg_val = (reg_val & ~NEURAL_CONTROL_MAC_STEP_CYCLES_MASK) |
			  (mac_step_cycles << NEURAL_CONTROL_MAC_STEP_CYCLES_SHIFT);

		if (!kbase_io_has_gpu(kbdev))
			return -EIO;

		kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(NEURAL_CONTROL), reg_val);
	}

	return 0;
}
#endif

static int kbase_set_gpu_quirks(struct kbase_device *kbdev)
{
#if MALI_USE_CSF
	if (kbase_reg_is_valid(kbdev, GPU_CONTROL_ENUM(CSF_CONFIG)))
		kbdev->hw_quirks_gpu = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(CSF_CONFIG));
#else
	u32 hw_quirks_gpu = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(JM_CONFIG));

	if (kbdev->gpu_props.gpu_id.product_model == GPU_ID_PRODUCT_TMIX) {
		/* Only for tMIx */
		u32 coherency_features;

		coherency_features = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(COHERENCY_FEATURES));

		/* (COHERENCY_ACE_LITE | COHERENCY_ACE) was incorrectly
		 * documented for tMIx so force correct value here.
		 */
		if (coherency_features == COHERENCY_FEATURE_BIT(COHERENCY_ACE)) {
			hw_quirks_gpu |= (COHERENCY_ACE_LITE | COHERENCY_ACE)
					 << JM_FORCE_COHERENCY_FEATURES_SHIFT;
		}
	}

	if (!kbase_io_has_gpu(kbdev))
		return -EIO;

	kbdev->hw_quirks_gpu = hw_quirks_gpu;

#endif /* !MALI_USE_CSF */
	if (kbase_hw_has_feature(kbdev, KBASE_HW_FEATURE_IDVS_GROUP_SIZE)) {
		u32 default_idvs_group_size = 0xF;
		u32 group_size = 0;

		if (of_property_read_u32(kbdev->dev->of_node, "idvs-group-size", &group_size))
			group_size = default_idvs_group_size;

		if (group_size > IDVS_GROUP_MAX_SIZE) {
			dev_err(kbdev->dev,
				"idvs-group-size of %d is too large. Maximum value is %d",
				group_size, IDVS_GROUP_MAX_SIZE);
			group_size = default_idvs_group_size;
		}

		kbdev->hw_quirks_gpu |= group_size << IDVS_GROUP_SIZE_SHIFT;
	}

#define MANUAL_POWER_CONTROL ((u32)(1 << 8))
	if (corestack_driver_control)
		kbdev->hw_quirks_gpu |= MANUAL_POWER_CONTROL;

	return 0;
}

static int kbase_set_sc_quirks(struct kbase_device *kbdev)
{
	u32 hw_quirks_sc = 0;

	if (kbase_reg_is_valid(kbdev, GPU_CONTROL_ENUM(SHADER_CONFIG)))
		hw_quirks_sc = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(SHADER_CONFIG));

	if (!kbase_io_has_gpu(kbdev))
		return -EIO;

	if (kbase_hw_has_issue(kbdev, KBASE_HW_ISSUE_TTRX_2968_TTRX_3162))
		hw_quirks_sc |= SC_VAR_ALGORITHM;

	if (kbase_hw_has_feature(kbdev, KBASE_HW_FEATURE_TLS_HASHING))
		hw_quirks_sc |= SC_TLS_HASH_ENABLE;

	kbdev->hw_quirks_sc = hw_quirks_sc;

	return 0;
}

static int kbase_set_tiler_quirks(struct kbase_device *kbdev)
{
	u32 hw_quirks_tiler = 0;

	if (kbase_reg_is_valid(kbdev, GPU_CONTROL_ENUM(TILER_CONFIG)))
		hw_quirks_tiler = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(TILER_CONFIG));

	if (!kbase_io_has_gpu(kbdev))
		return -EIO;

	/* Set tiler clock gate override if required */
	if (kbase_hw_has_issue(kbdev, KBASE_HW_ISSUE_T76X_3953))
		hw_quirks_tiler |= TC_CLOCK_GATE_OVERRIDE;

	kbdev->hw_quirks_tiler = hw_quirks_tiler;

	return 0;
}

#if MALI_USE_CSF
static int kbase_set_ne_quirks(struct kbase_device *kbdev)
{
	u32 hw_quirks_ne = 0;

	if (kbase_reg_is_valid(kbdev, GPU_CONTROL_ENUM(NEURAL_CONFIG)))
		hw_quirks_ne = kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(NEURAL_CONFIG));

	if (!kbase_io_has_gpu(kbdev))
		return -EIO;

	kbdev->hw_quirks_ne = hw_quirks_ne;

	return 0;
}
#endif

static int kbase_pm_hw_issues_detect(struct kbase_device *kbdev)
{
	struct device_node *np = kbdev->dev->of_node;
	int error = 0;

	kbdev->hw_quirks_gpu = 0;
	kbdev->hw_quirks_sc = 0;
	kbdev->hw_quirks_tiler = 0;
	kbdev->hw_quirks_mmu = 0;

	/* Read the "-" versions of the properties and fall back to
	 * the "_" versions if these are not found
	 */

	if (!of_property_read_u32(np, "quirks-gpu", &kbdev->hw_quirks_gpu) ||
	    !of_property_read_u32(np, "quirks_gpu", &kbdev->hw_quirks_gpu)) {
		dev_info(kbdev->dev, "Found quirks_gpu = [0x%x] in Devicetree\n",
			 kbdev->hw_quirks_gpu);
	} else {
		error = kbase_set_gpu_quirks(kbdev);
		if (error)
			return error;
	}

	if (!of_property_read_u32(np, "quirks-sc", &kbdev->hw_quirks_sc) ||
	    !of_property_read_u32(np, "quirks_sc", &kbdev->hw_quirks_sc)) {
		dev_info(kbdev->dev, "Found quirks_sc = [0x%x] in Devicetree\n",
			 kbdev->hw_quirks_sc);
	} else {
		error = kbase_set_sc_quirks(kbdev);
		if (error)
			return error;
	}

	if (!of_property_read_u32(np, "quirks-tiler", &kbdev->hw_quirks_tiler) ||
	    !of_property_read_u32(np, "quirks_tiler", &kbdev->hw_quirks_tiler)) {
		dev_info(kbdev->dev, "Found quirks_tiler = [0x%x] in Devicetree\n",
			 kbdev->hw_quirks_tiler);
	} else {
		error = kbase_set_tiler_quirks(kbdev);
		if (error)
			return error;
	}

	if (!of_property_read_u32(np, "quirks-mmu", &kbdev->hw_quirks_mmu) ||
	    !of_property_read_u32(np, "quirks_mmu", &kbdev->hw_quirks_mmu)) {
		dev_info(kbdev->dev, "Found MMU quirks = [0x%x] in Devicetree\n",
			 kbdev->hw_quirks_mmu);
	} else {
		error = kbase_set_mmu_quirks(kbdev);
	}

#if MALI_USE_CSF
	if (!of_property_read_u32(np, "quirks-ne", &kbdev->hw_quirks_ne)) {
		dev_info(kbdev->dev, "Found NE quirks = [0x%x] in Devicetree\n",
			 kbdev->hw_quirks_ne);
	} else {
		error = kbase_set_ne_quirks(kbdev);
		if (error)
			return error;
	}
#endif

	return error;
}

static void kbase_pm_hw_issues_apply(struct kbase_device *kbdev)
{
	if (kbase_reg_is_valid(kbdev, GPU_CONTROL_ENUM(SHADER_CONFIG)))
		kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(SHADER_CONFIG), kbdev->hw_quirks_sc);

	if (kbase_reg_is_valid(kbdev, GPU_CONTROL_ENUM(TILER_CONFIG)))
		kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(TILER_CONFIG), kbdev->hw_quirks_tiler);

	if (kbase_reg_is_valid(kbdev, GPU_CONTROL_ENUM(L2_MMU_CONFIG)))
		kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(L2_MMU_CONFIG), kbdev->hw_quirks_mmu);
#if MALI_USE_CSF
	if (kbase_reg_is_valid(kbdev, GPU_CONTROL_ENUM(CSF_CONFIG)))
		kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(CSF_CONFIG), kbdev->hw_quirks_gpu);
#else
	kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(JM_CONFIG), kbdev->hw_quirks_gpu);
#endif

#if MALI_USE_CSF
	if (kbase_reg_is_valid(kbdev, GPU_CONTROL_ENUM(NEURAL_CONFIG)))
		kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(NEURAL_CONFIG), kbdev->hw_quirks_ne);
#endif
}

void kbase_pm_cache_snoop_enable(struct kbase_device *kbdev)
{
#if !MALI_USE_CSF
	if ((kbdev->current_gpu_coherency_mode == COHERENCY_ACE) && !kbdev->cci_snoop_enabled) {
#if IS_ENABLED(CONFIG_ARM64)
		if (kbdev->snoop_enable_smc != 0)
			kbase_invoke_smc_fid(kbdev->snoop_enable_smc, 0, 0, 0);
#endif /* CONFIG_ARM64 */
		dev_dbg(kbdev->dev, "MALI - CCI Snoops - Enabled\n");
		kbdev->cci_snoop_enabled = true;
	}
#endif /* !MALI_USE_CSF */
}

void kbase_pm_cache_snoop_disable(struct kbase_device *kbdev)
{
#if !MALI_USE_CSF
	if (kbdev->cci_snoop_enabled) {
#if IS_ENABLED(CONFIG_ARM64)
		if (kbdev->snoop_disable_smc != 0) {
			mali_cci_flush_l2(kbdev);
			kbase_invoke_smc_fid(kbdev->snoop_disable_smc, 0, 0, 0);
		}
#endif /* CONFIG_ARM64 */
		dev_dbg(kbdev->dev, "MALI - CCI Snoops Disabled\n");
		kbdev->cci_snoop_enabled = false;
	}
#endif /* !MALI_USE_CSF */
}

#if !MALI_USE_CSF
static void reenable_protected_mode_hwcnt(struct kbase_device *kbdev)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&kbdev->hwaccess_lock, irq_flags);
	kbdev->protected_mode_hwcnt_desired = true;
	if (kbdev->protected_mode_hwcnt_disabled) {
		kbase_hwcnt_context_enable(kbdev->hwcnt_gpu_ctx);
		kbdev->protected_mode_hwcnt_disabled = false;
	}
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, irq_flags);
}
#endif

static int kbase_pm_hw_reset(struct kbase_device *kbdev)
{
	unsigned long flags;
	bool gpu_ready;

	lockdep_assert_held(&kbdev->pm.lock);

	if (!kbdev->pm.backend.callback_hardware_reset) {
		dev_warn(kbdev->dev, "No hardware reset provided");
		return -EINVAL;
	}

	/* Save GPU power state */
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	WARN_ON(!kbase_io_is_gpu_powered(kbdev));
	gpu_ready = kbdev->pm.backend.gpu_ready;
	kbdev->pm.backend.gpu_ready = false;
	kbase_io_set_status(kbdev->io, KBASE_IO_STATUS_GPU_OFF);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

#if MALI_USE_CSF
	/* Swap for dummy page */
	update_user_reg_page_mapping(kbdev);
#endif

	/* Delegate hardware reset to platform */
	kbdev->pm.backend.callback_hardware_reset(kbdev);

#if MALI_USE_CSF
	/* Swap for real page */
	update_user_reg_page_mapping(kbdev);
#endif

	/* GPU is powered again, restore state */
	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);
	kbase_io_clear_status(kbdev->io, KBASE_IO_STATUS_GPU_OFF);
	kbdev->pm.backend.gpu_ready = gpu_ready;
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);

	/* Check register access for success */
	if (!kbase_io_has_gpu(kbdev)) {
		dev_err(kbdev->dev, "Registers in-accessible after platform reset");
		return -EINVAL;
	}
	return 0;
}

static int kbase_pm_do_reset_soft(struct kbase_device *kbdev)
{
	int ret;

	if (kbdev->pm.backend.callback_soft_reset) {
		ret = kbdev->pm.backend.callback_soft_reset(kbdev);
		if (ret < 0)
			return ret;
		else if (ret > 0)
			return 0;
	} else {
#if MALI_USE_CSF
		/* Check if soft reset needs to be issued via PWR_COMMAND */
		if (kbdev->pm.backend.has_host_pwr_iface)
			kbase_reg_write32(kbdev, HOST_POWER_ENUM(PWR_COMMAND),
					  PWR_COMMAND_COMMAND_RESET_SOFT);
		else
#endif
		{
			kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_COMMAND),
					  GPU_COMMAND_SOFT_RESET);
		}
	}
	return 0;
}

static int kbase_pm_do_reset(struct kbase_device *kbdev)
{
	struct kbasep_reset_timeout_data rtdata;
	u32 reg_offset, reg_val;
	int ret;

#if MALI_USE_CSF
	if (kbdev->csf.reset.force_pm_hw_reset && kbdev->pm.backend.callback_hardware_reset) {
		dev_err(kbdev->dev, "Power Cycle reset mali");
		kbdev->csf.reset.force_pm_hw_reset = false;
		return kbase_pm_hw_reset(kbdev);
	}
#endif

	KBASE_KTRACE_ADD(kbdev, CORE_GPU_SOFT_RESET, NULL, 0);

	KBASE_TLSTREAM_JD_GPU_SOFT_RESET(kbdev, kbdev);

	atomic_set(&kbdev->pm.backend.reset_in_progress, 1);

	{
		/* b/290409680 - unmask irq before the soft reset */
		reg_offset = GPU_CONTROL_ENUM(GPU_IRQ_MASK);
		reg_val = RESET_COMPLETED;

		/* Unmask the reset complete interrupt only */
#if MALI_USE_CSF
		if (kbdev->pm.backend.has_host_pwr_iface) {
			reg_offset = HOST_POWER_ENUM(PWR_IRQ_MASK);
			reg_val = PWR_IRQ_RESET_COMPLETED;
		}
#endif
		kbase_reg_write32(kbdev, reg_offset, reg_val);

		ret = kbase_pm_do_reset_soft(kbdev);
		if (ret)
			return ret;
	}

	/* Initialize a structure for tracking the status of the reset */
	rtdata.kbdev = kbdev;
	rtdata.timed_out = false;

	/* Create a timer to use as a timeout on the reset */
	hrtimer_init_on_stack(&rtdata.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	rtdata.timer.function = kbasep_reset_timeout;

	hrtimer_start(&rtdata.timer, HR_TIMER_DELAY_MSEC(RESET_TIMEOUT), HRTIMER_MODE_REL);

	/* Wait for the RESET_COMPLETED interrupt to be raised */
	kbase_pm_wait_for_reset(kbdev);


	if (!rtdata.timed_out) {
		/* GPU has been reset */
		hrtimer_cancel(&rtdata.timer);
		destroy_hrtimer_on_stack(&rtdata.timer);
		return 0;
	}

	reg_offset = GPU_CONTROL_ENUM(GPU_IRQ_RAWSTAT);
	reg_val = RESET_COMPLETED;

#if MALI_USE_CSF
	if (kbdev->pm.backend.has_host_pwr_iface) {
		reg_offset = HOST_POWER_ENUM(PWR_IRQ_RAWSTAT);
		reg_val = PWR_IRQ_RESET_COMPLETED;
	}
#endif

	/* No interrupt has been received - check if the RAWSTAT register says
	 * the reset has completed.
	 */
	if (kbase_reg_read32(kbdev, reg_offset) & reg_val) {
		/* The interrupt is set in the RAWSTAT; this suggests that the
		 * interrupts are not getting to the CPU
		 */
		dev_err(kbdev->dev,
			"Reset interrupt didn't reach CPU. Check interrupt assignments.\n");
		/* If interrupts aren't working we can't continue. */
		destroy_hrtimer_on_stack(&rtdata.timer);
		return -EINVAL;
	}

	if (!kbase_io_has_gpu(kbdev)) {
		dev_dbg(kbdev->dev, "GPU has been removed, reset no longer needed.\n");
		destroy_hrtimer_on_stack(&rtdata.timer);
		return -EINVAL;
	}

	/* The GPU doesn't seem to be responding to the reset so try a hard
	 * reset, but only when NOT in arbitration mode.
	 */

	if (!kbase_has_arbiter(kbdev)) {
		dev_err(kbdev->dev,
			"Failed to soft-reset GPU (timed out after %d ms), now attempting a hard reset\n",
			RESET_TIMEOUT);
		KBASE_KTRACE_ADD(kbdev, CORE_GPU_HARD_RESET, NULL, 0);

		atomic_set(&kbdev->pm.backend.reset_in_progress, 1);

#if MALI_USE_CSF
		if (kbdev->pm.backend.has_host_pwr_iface)
			kbase_reg_write32(kbdev, HOST_POWER_ENUM(PWR_COMMAND),
					  PWR_COMMAND_COMMAND_RESET_HARD);
		else
#endif
		{
			kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_COMMAND),
					  GPU_COMMAND_HARD_RESET);
		}

		/* Restart the timer to wait for the hard reset to complete */
		rtdata.timed_out = false;

		hrtimer_start(&rtdata.timer, HR_TIMER_DELAY_MSEC(RESET_TIMEOUT), HRTIMER_MODE_REL);

		/* Wait for the RESET_COMPLETED interrupt to be raised */
		kbase_pm_wait_for_reset(kbdev);

		if (!rtdata.timed_out) {
			/* GPU has been reset */
			hrtimer_cancel(&rtdata.timer);
			destroy_hrtimer_on_stack(&rtdata.timer);
			return 0;
		}

		destroy_hrtimer_on_stack(&rtdata.timer);

		dev_err(kbdev->dev,
			"Failed to hard-reset the GPU (timed out after %d ms) GPU_IRQ_RAWSTAT: %d\n",
			RESET_TIMEOUT, kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(GPU_IRQ_RAWSTAT)));

		/* Last resort, trigger a hardware reset of the GPU */
		return kbase_pm_hw_reset(kbdev);
	}

	return -EINVAL;
}

int kbase_pm_protected_mode_enable(struct kbase_device *const kbdev)
{
	kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_COMMAND), GPU_COMMAND_SET_PROTECTED_MODE);
	return 0;
}

int kbase_pm_protected_mode_disable(struct kbase_device *const kbdev)
{
	lockdep_assert_held(&kbdev->pm.lock);

	return kbase_pm_do_reset(kbdev);
}

int kbase_pm_init_hw(struct kbase_device *kbdev, unsigned int flags)
{
	unsigned long irq_flags;
	int err = 0;

	ATRACE_BEGIN(__func__);
	KBASE_DEBUG_ASSERT(kbdev != NULL);
	lockdep_assert_held(&kbdev->pm.lock);

	/* Ensure the clock is on before attempting to access the hardware */
	if (!kbase_io_is_gpu_powered(kbdev)) {
		if (kbdev->pm.backend.callback_power_on)
			kbdev->pm.backend.callback_power_on(kbdev);

		kbase_io_clear_status(kbdev->io, KBASE_IO_STATUS_GPU_OFF);
	}

#if MALI_USE_CSF
	if (kbdev->pm.backend.has_host_pwr_iface && !pwr_status_gpu_reset_allowed(kbdev))
		return -EINVAL;
#endif
	/* Ensure interrupts are off to begin with, this also clears any
	 * outstanding interrupts
	 */
	kbase_pm_disable_interrupts(kbdev);
	/* Ensure cache snoops are disabled before reset. */
	kbase_pm_cache_snoop_disable(kbdev);
	/* Prepare for the soft-reset */
	kbdev->pm.backend.reset_done = false;
	atomic_set(&kbdev->pm.backend.reset_in_progress, 0);

	/* The cores should be made unavailable due to the reset */
	spin_lock_irqsave(&kbdev->hwaccess_lock, irq_flags);
	if (kbdev->pm.backend.shaders_state != KBASE_SHADERS_OFF_CORESTACK_OFF)
		KBASE_KTRACE_ADD(kbdev, PM_CORES_CHANGE_AVAILABLE, NULL, 0u);
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, irq_flags);

	/* Soft reset the GPU */
	if (!(flags & PM_NO_RESET))
		err = kbdev->protected_ops->protected_mode_disable(kbdev->protected_dev);

	spin_lock_irqsave(&kbdev->hwaccess_lock, irq_flags);
#if MALI_USE_CSF
	if (kbdev->protected_mode) {
		unsigned long flags;

		kbase_ipa_control_protm_exited(kbdev);

		kbase_csf_scheduler_spin_lock(kbdev, &flags);
		kbase_hwcnt_backend_csf_protm_exited(&kbdev->hwcnt_gpu_iface);
		kbase_csf_scheduler_spin_unlock(kbdev, flags);
	}
#endif
	kbdev->protected_mode = false;
	spin_unlock_irqrestore(&kbdev->hwaccess_lock, irq_flags);

	if (err)
		goto exit;

#if MALI_USE_CSF
	/* Check power status register value after GPU reset */
	if (kbdev->pm.backend.has_host_pwr_iface && !pwr_status_value_valid_post_reset(kbdev)) {
		err = -EINVAL;
		goto exit;
	}

	/* As GPU has been reset, so the power control delegation done previously
	 * would have been undone.
	 */
	kbdev->pm.backend.pwr_cntl_delegated = false;

	if (kbdev->gpu_props.gpu_id.arch_id >= GPU_ID_ARCH_MAKE(14, 8, 0) &&
	    kbase_csf_dev_has_ne(kbdev)) {
		err = kbase_ne_control_apply(kbdev);
		if (err)
			goto exit;
	}
#endif
	if (flags & PM_HW_ISSUES_DETECT) {
		err = kbase_pm_hw_issues_detect(kbdev);
		if (err)
			goto exit;
	}

	kbase_pm_hw_issues_apply(kbdev);
	kbase_cache_set_coherency_mode(kbdev, kbdev->system_coherency);
	kbase_amba_set_shareable_cache_support(kbdev);
#if MALI_USE_CSF
	kbase_backend_update_gpu_timestamp_offset(kbdev);
	kbdev->csf.compute_progress_timeout_cc = 0;
#endif

	/* Sanity check protected mode was left after reset */
	WARN_ON(kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(GPU_STATUS)) &
		GPU_STATUS_PROTECTED_MODE_ACTIVE);

	if (flags & PM_ENABLE_IRQS)
		kbase_pm_enable_interrupts(kbdev);

exit:
#if !MALI_USE_CSF
	if (!kbdev->pm.backend.protected_entry_transition_override) {
		/* Re-enable GPU hardware counters if we're resetting from
		 * protected mode.
		 */
		reenable_protected_mode_hwcnt(kbdev);
	}
#endif

	ATRACE_END();

	return err;
}

/**
 * kbase_pm_request_gpu_cycle_counter_do_request - Request cycle counters
 * @kbdev:     The kbase device structure of the device
 *
 * Increase the count of cycle counter users and turn the cycle counters on if
 * they were previously off
 *
 * This function is designed to be called by
 * kbase_pm_request_gpu_cycle_counter() or
 * kbase_pm_request_gpu_cycle_counter_l2_is_on() only
 *
 * When this function is called the l2 cache must be on - i.e., the GPU must be
 * on.
 */
static void kbase_pm_request_gpu_cycle_counter_do_request(struct kbase_device *kbdev)
{
	unsigned long flags;

	spin_lock_irqsave(&kbdev->pm.backend.gpu_cycle_counter_requests_lock, flags);
	++kbdev->pm.backend.gpu_cycle_counter_requests;

	if (kbdev->pm.backend.gpu_cycle_counter_requests == 1)
		kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_COMMAND),
				  GPU_COMMAND_CYCLE_COUNT_START);
	else {
		/* This might happen after GPU reset.
		 * Then counter needs to be kicked.
		 */
#if !IS_ENABLED(CONFIG_MALI_NO_MALI) && !MALI_USE_CSF
		if (!(kbase_reg_read32(kbdev, GPU_CONTROL_ENUM(GPU_STATUS)) &
		      GPU_STATUS_CYCLE_COUNT_ACTIVE)) {
			kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_COMMAND),
					  GPU_COMMAND_CYCLE_COUNT_START);
		}
#endif
	}

	spin_unlock_irqrestore(&kbdev->pm.backend.gpu_cycle_counter_requests_lock, flags);
}

void kbase_pm_request_gpu_cycle_counter(struct kbase_device *kbdev)
{
	KBASE_DEBUG_ASSERT(kbdev != NULL);

	KBASE_DEBUG_ASSERT(kbase_io_is_gpu_powered(kbdev));

	KBASE_DEBUG_ASSERT(kbdev->pm.backend.gpu_cycle_counter_requests < INT_MAX);

	kbase_pm_wait_for_l2_powered(kbdev);

	kbase_pm_request_gpu_cycle_counter_do_request(kbdev);
}

KBASE_EXPORT_TEST_API(kbase_pm_request_gpu_cycle_counter);

void kbase_pm_request_gpu_cycle_counter_l2_is_on(struct kbase_device *kbdev)
{
	KBASE_DEBUG_ASSERT(kbdev != NULL);

	KBASE_DEBUG_ASSERT(kbase_io_is_gpu_powered(kbdev));

	KBASE_DEBUG_ASSERT(kbdev->pm.backend.gpu_cycle_counter_requests < INT_MAX);

	kbase_pm_request_gpu_cycle_counter_do_request(kbdev);
}

KBASE_EXPORT_TEST_API(kbase_pm_request_gpu_cycle_counter_l2_is_on);

void kbase_pm_release_gpu_cycle_counter_nolock(struct kbase_device *kbdev)
{
	unsigned long flags;

	KBASE_DEBUG_ASSERT(kbdev != NULL);

	lockdep_assert_held(&kbdev->hwaccess_lock);

	spin_lock_irqsave(&kbdev->pm.backend.gpu_cycle_counter_requests_lock, flags);

	KBASE_DEBUG_ASSERT(kbdev->pm.backend.gpu_cycle_counter_requests > 0);

	--kbdev->pm.backend.gpu_cycle_counter_requests;

	if (kbdev->pm.backend.gpu_cycle_counter_requests == 0)
		kbase_reg_write32(kbdev, GPU_CONTROL_ENUM(GPU_COMMAND),
				  GPU_COMMAND_CYCLE_COUNT_STOP);

	spin_unlock_irqrestore(&kbdev->pm.backend.gpu_cycle_counter_requests_lock, flags);
}

void kbase_pm_release_gpu_cycle_counter(struct kbase_device *kbdev)
{
	unsigned long flags;

	spin_lock_irqsave(&kbdev->hwaccess_lock, flags);

	kbase_pm_release_gpu_cycle_counter_nolock(kbdev);

	spin_unlock_irqrestore(&kbdev->hwaccess_lock, flags);
}

KBASE_EXPORT_TEST_API(kbase_pm_release_gpu_cycle_counter);

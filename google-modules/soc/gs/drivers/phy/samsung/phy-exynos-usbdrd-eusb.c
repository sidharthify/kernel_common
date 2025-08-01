// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung EXYNOS SoC series USB DRD PHY driver
 *
 * Copyright (C) 2022 Samsung Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/samsung_usb.h>
#include <linux/usb/otg.h>
#if IS_ENABLED(CONFIG_EXYNOS_OTP)
#include <linux/exynos_otp.h>
#endif
#ifdef CONFIG_OF
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#endif

#include "phy-exynos-usbdrd.h"
#include "phy-exynos-eusb.h"
#include "exynos-usb-blkcon.h"
#include "phy-exynos-snps-usbdp.h"
#include <soc/google/exynos-el3_mon.h>

static void __iomem *usbdp_combo_phy_reg;
void __iomem *phycon_base_addr;
EXPORT_SYMBOL_GPL(phycon_base_addr);

static int (*s2mpu_notify)(struct device *dev, bool on);

int exynos_usbdrd_set_s2mpu_pm_ops(int (*cb)(struct device *dev, bool on))
{
	/*
	 * Paired with smp_load_acquire(&s2mpu_notify),
	 * Ensure memory stores hapenning during module init
	 * are observed before executing the callback.
	 */
	return cmpxchg_release(&s2mpu_notify, NULL, cb) ? -EBUSY : 0;
}
EXPORT_SYMBOL_GPL(exynos_usbdrd_set_s2mpu_pm_ops);

/*u32 get_speed_and_disu1u2(void);*/

static ssize_t
exynos_usbdrd_eom_show(struct device *dev,
	 struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t
exynos_usbdrd_eom_store(struct device *dev,
	  struct device_attribute *attr, const char *buf, size_t n)
{
	return 0;
}

static DEVICE_ATTR_RW(exynos_usbdrd_eom);


static ssize_t
exynos_usbdrd_loopback_show(struct device *dev,
	 struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t
exynos_usbdrd_loopback_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	return 0;
}

static DEVICE_ATTR_RW(exynos_usbdrd_loopback);


static ssize_t
exynos_usbdrd_hs_phy_tune_show(struct device *dev,
		 struct device_attribute *attr, char *buf)
{
	struct exynos_usbdrd_phy *phy_drd = dev_get_drvdata(dev);
	int len = 0, i, ret;
	u32 tune_num = 0;
	struct device_node *tune_node;

	tune_node = of_parse_phandle(dev->of_node, "hs_tune_param", 0);

	ret = of_property_read_u32_array(tune_node, "hs_tune_cnt", &tune_num, 1);
	if (ret) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "Can't get tune value!!!\n");
		goto exit;
	}

	len += scnprintf(buf + len, PAGE_SIZE - len, "\t==== Print USB Tune Value ====\n");
	len += scnprintf(buf + len, PAGE_SIZE - len, "Tune value count : %d\n", tune_num);

	for (i = 0; i < tune_num; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s\t\t\t: %#x, %#x\n",
				phy_drd->usbphy_info.tune_param[i].name,
				phy_drd->hs_tune_param_value[i][0],
				phy_drd->hs_tune_param_value[i][1]);
	}

exit:
	return len;
}

static ssize_t
exynos_usbdrd_hs_phy_tune_store(struct device *dev,
		  struct device_attribute *attr, const char *buf, size_t n)
{
	char tune_name[20];
	u32 tune_val;
	struct device_node *tune_node;
	struct exynos_usbdrd_phy *phy_drd = dev_get_drvdata(dev);
	int ret, i;
	u32 tune_num = 0;

	if (sscanf(buf, "%19s %x", tune_name, &tune_val) != 2)
		return -EINVAL;

	tune_node = of_parse_phandle(dev->of_node, "hs_tune_param", 0);
	ret = of_property_read_u32_array(tune_node, "hs_tune_cnt", &tune_num, 1);
	if (ret) {
		pr_err("Can't get hs_tune_cnt!!!\n");
		goto exit;
	}

	for (i = 0; i < tune_num; i++) {
		if (!strncmp(phy_drd->usbphy_info.tune_param[i].name, tune_name,
			     strlen(phy_drd->usbphy_info.tune_param[i].name))) {
			phy_drd->hs_tune_param_value[i][0] = tune_val;
			phy_drd->hs_tune_param_value[i][1] = tune_val;
		}
	}

exit:
	return n;
}

static DEVICE_ATTR_RW(exynos_usbdrd_hs_phy_tune);

static ssize_t
exynos_usbdrd_phy_tune_show(struct device *dev,
	      struct device_attribute *attr, char *buf)
{
	struct exynos_usbdrd_phy *phy_drd = dev_get_drvdata(dev);
	int len = 0, i, ret;
	u32 tune_num = 0;
	struct device_node *tune_node;

	tune_node = of_parse_phandle(dev->of_node, "ss_tune_param", 0);

	ret = of_property_read_u32_array(tune_node, "ss_tune_cnt", &tune_num, 1);
	if (ret) {
		len += scnprintf(buf + len, PAGE_SIZE - len,
				"Can't get tune value!!!\n");
		goto exit;
	}

	len += scnprintf(buf + len, PAGE_SIZE - len, "\t==== Print USB Tune Value ====\n");
	len += scnprintf(buf + len, PAGE_SIZE - len, "Tune value count : %d\n", tune_num);

	for (i = 0; i < tune_num; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s\t\t\t: %#x, %#x\n",
				phy_drd->usbphy_sub_info.tune_param[i].name,
				phy_drd->ss_tune_param_value[i][0],
				phy_drd->ss_tune_param_value[i][1]);
	}

exit:
	return len;
}

static ssize_t
exynos_usbdrd_phy_tune_store(struct device *dev,
	       struct device_attribute *attr, const char *buf, size_t n)
{
	char tune_name[30];
	u32 tune_val;
	struct device_node *tune_node;
	struct exynos_usbdrd_phy *phy_drd = dev_get_drvdata(dev);
	int ret, i;
	u32 tune_num = 0;

	if (sscanf(buf, "%29s %x", tune_name, &tune_val) != 2)
		return -EINVAL;

	tune_node = of_parse_phandle(dev->of_node, "ss_tune_param", 0);
	ret = of_property_read_u32_array(tune_node, "ss_tune_cnt", &tune_num, 1);
	if (ret) {
		pr_err("Can't get ss_tune_cnt!!!\n");
		goto exit;
	}

	for (i = 0; i < tune_num; i++) {
		if (!strncmp(phy_drd->usbphy_sub_info.tune_param[i].name, tune_name,
			     strlen(phy_drd->usbphy_sub_info.tune_param[i].name))) {
			phy_drd->ss_tune_param_value[i][0] = tune_val;
			phy_drd->ss_tune_param_value[i][1] = tune_val;
		}
	}

exit:
	return n;
}
static DEVICE_ATTR_RW(exynos_usbdrd_phy_tune);

static struct attribute *exynos_usbdrd_attrs[] = {
	&dev_attr_exynos_usbdrd_eom.attr,
	&dev_attr_exynos_usbdrd_loopback.attr,
	&dev_attr_exynos_usbdrd_hs_phy_tune.attr,
	&dev_attr_exynos_usbdrd_phy_tune.attr,
	NULL
};
ATTRIBUTE_GROUPS(exynos_usbdrd);

static int exynos_usbdrd_clk_prepare(struct exynos_usbdrd_phy *phy_drd)
{
	int i;
	int ret;

	for (i = 0; phy_drd->clocks[i]; i++) {
		ret = clk_prepare(phy_drd->clocks[i]);
		if (ret) {
			for (i = i - 1; i >= 0; i--)
				clk_unprepare(phy_drd->clocks[i]);
			return ret;
		}
	}

	if (phy_drd->use_phy_umux) {
		for (i = 0; phy_drd->phy_clocks[i]; i++) {
			ret = clk_prepare(phy_drd->phy_clocks[i]);
			if (ret) {
				for (i = i - 1; i >= 0; i--)
					clk_unprepare(phy_drd->phy_clocks[i]);
				return ret;
			}
		}
	}
	return 0;
}

static int exynos_usbdrd_clk_enable(struct exynos_usbdrd_phy *phy_drd,
				    bool umux)
{
	int i;
	int ret;

	if (!umux) {
		for (i = 0; phy_drd->clocks[i]; i++) {
			ret = clk_enable(phy_drd->clocks[i]);
			if (ret) {
				for (i = i - 1; i >= 0; i--)
					clk_disable(phy_drd->clocks[i]);
				return ret;
			}
		}
	} else {
		for (i = 0; phy_drd->phy_clocks[i]; i++) {
			ret = clk_enable(phy_drd->phy_clocks[i]);
			if (ret) {
				for (i = i - 1; i >= 0; i--)
					clk_disable(phy_drd->phy_clocks[i]);
				return ret;
			}
		}
	}
	return 0;
}

static void exynos_usbdrd_clk_unprepare(struct exynos_usbdrd_phy *phy_drd)
{
	int i;

	for (i = 0; phy_drd->clocks[i]; i++)
		clk_unprepare(phy_drd->clocks[i]);
	for (i = 0; phy_drd->phy_clocks[i]; i++)
		clk_unprepare(phy_drd->phy_clocks[i]);
}

static void exynos_usbdrd_clk_disable(struct exynos_usbdrd_phy *phy_drd, bool umux)
{
	int i;

	if (!umux) {
		for (i = 0; phy_drd->clocks[i]; i++)
			clk_disable(phy_drd->clocks[i]);
	} else {
		for (i = 0; phy_drd->phy_clocks[i]; i++)
			clk_disable(phy_drd->phy_clocks[i]);
	}
}

static int exynos_usbdrd_phyclk_get(struct exynos_usbdrd_phy *phy_drd)
{
	struct device *dev = phy_drd->dev;
	const char **phyclk_ids;
	const char **clk_ids;
	const char *refclk_name;
	struct clk *clk;
	int phyclk_count;
	int clk_count;
	bool is_phyclk = false;
	int clk_index = 0;
	int i, j, ret;

	phyclk_count = of_property_count_strings(dev->of_node, "phyclk_mux");
	if (IS_ERR_VALUE((unsigned long)phyclk_count)) {
		dev_err(dev, "invalid phyclk list in %s node\n",
			dev->of_node->name);
		return -EINVAL;
	}

	phyclk_ids = devm_kcalloc(dev, (phyclk_count + 1),
				  sizeof(const char *), GFP_KERNEL);
	for (i = 0; i < phyclk_count; i++) {
		ret = of_property_read_string_index(dev->of_node,
						    "phyclk_mux", i, &phyclk_ids[i]);
		if (ret) {
			dev_err(dev, "failed to read phyclk_mux name %d from %s node\n",
				i, dev->of_node->name);
			return ret;
		}
	}
	phyclk_ids[phyclk_count] = NULL;

	if (!strcmp("none", phyclk_ids[0])) {
		dev_info(dev, "don't need user Mux for phyclk\n");
		phy_drd->use_phy_umux = false;
		phyclk_count = 0;

	} else {
		phy_drd->use_phy_umux = true;

		phy_drd->phy_clocks = devm_kcalloc(dev, (phyclk_count + 1),
						   sizeof(struct clk *), GFP_KERNEL);
		if (!phy_drd->phy_clocks) {
			dev_err(dev, "failed to alloc : phy clocks\n");
			return -ENOMEM;
		}

		for (i = 0; phyclk_ids[i]; i++) {
			clk = devm_clk_get(dev, phyclk_ids[i]);
			if (IS_ERR_OR_NULL(clk)) {
				dev_err(dev, "couldn't get %s clock\n", phyclk_ids[i]);
				return -EINVAL;
			}
			phy_drd->phy_clocks[i] = clk;
		}

		phy_drd->phy_clocks[i] = NULL;
	}

	clk_count = of_property_count_strings(dev->of_node, "clock-names");
	if (IS_ERR_VALUE((unsigned long)clk_count)) {
		dev_err(dev, "invalid clk list in %s node", dev->of_node->name);
		return -EINVAL;
	}
	clk_ids = devm_kcalloc(dev, (clk_count + 1), sizeof(const char *),
			       GFP_KERNEL);
	for (i = 0; i < clk_count; i++) {
		ret = of_property_read_string_index(dev->of_node, "clock-names",
						    i, &clk_ids[i]);
		if (ret) {
			dev_err(dev, "failed to read clocks name %d from %s node\n",
				i, dev->of_node->name);
			return ret;
		}
	}
	clk_ids[clk_count] = NULL;

	phy_drd->clocks = devm_kcalloc(dev, (clk_count + 1),
				       sizeof(struct clk *), GFP_KERNEL);
	if (!phy_drd->clocks)
		return -ENOMEM;

	for (i = 0; clk_ids[i]; i++) {
		if (phyclk_count) {
			for (j = 0; phyclk_ids[j]; j++) {
				if (!strcmp(phyclk_ids[j], clk_ids[i])) {
					is_phyclk = true;
					phyclk_count--;
				}
			}
		}
		if (!is_phyclk) {
			clk = devm_clk_get(dev, clk_ids[i]);
			if (IS_ERR_OR_NULL(clk)) {
				dev_err(dev, "couldn't get %s clock\n", clk_ids[i]);
				return -EINVAL;
			}
			phy_drd->clocks[clk_index] = clk;
			clk_index++;
		}
		is_phyclk = false;
	}
	phy_drd->clocks[clk_index] = NULL;

	ret = of_property_read_string_index(dev->of_node,
					    "phy_refclk", 0, &refclk_name);
	if (ret) {
		dev_err(dev, "failed to read ref_clocks name from %s node\n",
			dev->of_node->name);
		return ret;
	}

	if (!strcmp("none", refclk_name)) {
		dev_err(dev, "phy reference clock shouldn't be omitted");
		return -EINVAL;
	}

	for (i = 0; clk_ids[i]; i++) {
		if (!strcmp(clk_ids[i], refclk_name)) {
			phy_drd->ref_clk = phy_drd->clocks[0];
			break;
		}
	}

	if (IS_ERR_OR_NULL(phy_drd->ref_clk)) {
		dev_err(dev, "%s couldn't get ref_clk", __func__);
		return -EINVAL;
	}

	devm_kfree(dev, phyclk_ids);
	devm_kfree(dev, clk_ids);

	return 0;
}

static int exynos_usbdrd_clk_get(struct exynos_usbdrd_phy *phy_drd)
{
	struct device *dev = phy_drd->dev;
	int ret;

	ret = exynos_usbdrd_phyclk_get(phy_drd);
	if (ret < 0) {
		dev_err(dev, "failed to get clock for DRD USBPHY");
		return ret;
	}

	return 0;
}

static inline
struct exynos_usbdrd_phy *to_usbdrd_phy(struct phy_usb_instance *inst)
{
	return container_of((inst), struct exynos_usbdrd_phy,
			    phys[(inst)->index]);
}

#if IS_ENABLED(CONFIG_EXYNOS_OTP)
void exynos_usbdrd_phy_get_otp_info(struct exynos_usbdrd_phy *phy_drd)
{
	struct tune_bits *data;
	u16 magic;
	u8 type;
	u8 index_count;
	u8 i, j;

	phy_drd->otp_index[0] = 0;
	phy_drd->otp_index[1] = 0;

	for (i = 0; i < OTP_SUPPORT_USBPHY_NUMBER; i++) {
		magic = i ? OTP_MAGIC_USB2 : OTP_MAGIC_USB3;

		if (otp_tune_bits_parsed(magic, &type, &index_count, &data)) {
			dev_err(phy_drd->dev, "%s failed to get usb%d otp\n",
				__func__, i ? 2 : 3);
			continue;
		}
		dev_info(phy_drd->dev, "usb[%d] otp index_count: %d\n",
			 i, index_count);

		if (!index_count) {
			phy_drd->otp_data[i] = NULL;
			continue;
		}

		phy_drd->otp_data[i] = devm_kzalloc(phy_drd->dev,
						    sizeof(*data) * index_count, GFP_KERNEL);
		if (!phy_drd->otp_data[i])
			continue;

		phy_drd->otp_index[i] = index_count;
		phy_drd->otp_type[i] = type ? 4 : 1;
		dev_info(phy_drd->dev, "usb[%d] otp type: %d\n", i, type);

		for (j = 0; j < index_count; j++) {
			phy_drd->otp_data[i][j].index = data[j].index;
			phy_drd->otp_data[i][j].value = data[j].value;
			dev_dbg(phy_drd->dev,
				"usb[%d][%d] otp_data index:%d, value:0x%08x\n",
				i, j, phy_drd->otp_data[i][j].index,
				phy_drd->otp_data[i][j].value);
		}
	}
}
#endif

/*
 * exynos_rate_to_clk() converts the supplied clock rate to the value that
 * can be written to the phy register.
 */
static unsigned int exynos_rate_to_clk(struct exynos_usbdrd_phy *phy_drd)
{
	int ret;
	int clk;

	ret = clk_prepare_enable(phy_drd->ref_clk);
	if (ret) {
		dev_err(phy_drd->dev, "%s failed to enable ref_clk", __func__);
		return 0;
	}

	clk = clk_get_rate(phy_drd->ref_clk);
	pr_info("%s, ref_clk = %d\n", __func__, clk);

	/* EXYNOS_FSEL_MASK */
	switch (clk) {
	case 12 * MHZ:
		phy_drd->extrefclk = USBPHY_REFCLK_EXT_12MHZ;
		break;
	case 19200 * KHZ:
		phy_drd->extrefclk = USBPHY_REFCLK_EXT_19P2MHZ;
		break;
	case 20 * MHZ:
	case 20312500:
		phy_drd->extrefclk = USBPHY_REFCLK_EXT_20MHZ;
		break;
	case 24 * MHZ:
		phy_drd->extrefclk = USBPHY_REFCLK_EXT_24MHZ;
		break;
	case 26 * MHZ:
		phy_drd->extrefclk = USBPHY_REFCLK_EXT_26MHZ;
		break;
	case 24576000:
		phy_drd->extrefclk = USBPHY_REFCLK_EXT_26MHZ;
		break;
	case 50 * MHZ:
		phy_drd->extrefclk = USBPHY_REFCLK_EXT_50MHZ;
		break;
	default:
		phy_drd->extrefclk = 0;
		clk_disable_unprepare(phy_drd->ref_clk);
		return -EINVAL;
	}

	clk_disable_unprepare(phy_drd->ref_clk);

	return 0;
}

static void exynos_usbdrd_pipe3_phy_isol(struct phy_usb_instance *inst,
					 unsigned int on, unsigned int mask)
{
	unsigned int val;

	if (!inst->reg_pmu)
		return;

	val = on ? 0 : mask;
	rmw_priv_reg(inst->pmu_alive_pa + inst->pmu_offset_dp, mask, val);
}

static void exynos_usbdrd_utmi_phy_isol(struct phy_usb_instance *inst,
					unsigned int on, unsigned int mask)
{
	unsigned int val;

	if (!inst->reg_pmu)
		return;

	val = on ? 0 : mask;
	rmw_priv_reg(inst->pmu_alive_pa + inst->pmu_offset, mask, val);

	/* Control TCXO_BUF */
	if (inst->pmu_mask_tcxobuf != 0) {
		val = on ? 0 : inst->pmu_mask_tcxobuf;
		rmw_priv_reg(inst->pmu_alive_pa + inst->pmu_offset_tcxobuf,
			     inst->pmu_mask_tcxobuf, val);

	}
}

/*
 * Sets the pipe3 phy's clk as EXTREFCLK (XXTI) which is internal clock
 * from clock core. Further sets multiplier values and spread spectrum
 * clock settings for SuperSpeed operations.
 */
static unsigned int
exynos_usbdrd_pipe3_set_refclk(struct phy_usb_instance *inst)
{
	static u32 reg;
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	/* PHYCLKRST setting isn't required in Combo PHY */
	if (phy_drd->usbphy_info.version >= EXYNOS_USBPHY_VER_02_0_0)
		return -EINVAL;

	/* restore any previous reference clock settings */
	reg = readl(phy_drd->reg_phy + EXYNOS_DRD_PHYCLKRST);

	/* Use EXTREFCLK as ref clock */
	reg &= ~PHYCLKRST_REFCLKSEL_MASK;
	reg |=	PHYCLKRST_REFCLKSEL_EXT_REFCLK;

	/* FSEL settings corresponding to reference clock */
	reg &= ~PHYCLKRST_FSEL_PIPE_MASK |
		PHYCLKRST_MPLL_MULTIPLIER_MASK |
		PHYCLKRST_SSC_REFCLKSEL_MASK;
	switch (phy_drd->extrefclk) {
	case EXYNOS_FSEL_50MHZ:
		reg |= (PHYCLKRST_MPLL_MULTIPLIER_50M_REF |
			PHYCLKRST_SSC_REFCLKSEL(0x00));
		break;
	case EXYNOS_FSEL_24MHZ:
		reg |= (PHYCLKRST_MPLL_MULTIPLIER_24MHZ_REF |
			PHYCLKRST_SSC_REFCLKSEL(0x88));
		break;
	case EXYNOS_FSEL_20MHZ:
		reg |= (PHYCLKRST_MPLL_MULTIPLIER_20MHZ_REF |
			PHYCLKRST_SSC_REFCLKSEL(0x00));
		break;
	case EXYNOS_FSEL_19MHZ2:
		reg |= (PHYCLKRST_MPLL_MULTIPLIER_19200KHZ_REF |
			PHYCLKRST_SSC_REFCLKSEL(0x88));
		break;
	default:
		dev_dbg(phy_drd->dev, "unsupported ref clk\n");
		break;
	}

	return reg;
}

/*
 * Sets the utmi phy's clk as EXTREFCLK (XXTI) which is internal clock
 * from clock core. Further sets the FSEL values for HighSpeed operations.
 */
static unsigned int
exynos_usbdrd_utmi_set_refclk(struct phy_usb_instance *inst)
{
	static u32 reg;
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	/* PHYCLKRST setting isn't required in Combo PHY */
	if (phy_drd->usbphy_info.version >= EXYNOS_USBPHY_VER_02_0_0)
		return -EINVAL;

	/* restore any previous reference clock settings */
	reg = readl(phy_drd->reg_phy + EXYNOS_DRD_PHYCLKRST);

	reg &= ~PHYCLKRST_REFCLKSEL_MASK;
	reg |=	PHYCLKRST_REFCLKSEL_EXT_REFCLK;

	reg &= ~PHYCLKRST_FSEL_UTMI_MASK |
		PHYCLKRST_MPLL_MULTIPLIER_MASK |
		PHYCLKRST_SSC_REFCLKSEL_MASK;
	reg |= PHYCLKRST_FSEL(phy_drd->extrefclk);

	return reg;
}

/*
 * Sets the default PHY tuning values for high-speed connection.
 */
static int exynos_usbdrd_fill_hstune(struct exynos_usbdrd_phy *phy_drd,
				     struct device_node *node)
{
	struct device *dev = phy_drd->dev;
	struct exynos_usbphy_hs_tune *hs_tune = phy_drd->hs_value;
	int ret;
	u32 res[2];
	u32 value;

	ret = of_property_read_u32_array(node, "tx_vref", res, 2);
	if (ret == 0) {
		hs_tune[0].tx_vref = res[0];
		hs_tune[1].tx_vref = res[1];
	} else {
		dev_err(dev, "can't get tx_vref value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_pre_emp", res, 2);
	if (ret == 0) {
		hs_tune[0].tx_pre_emp = res[0];
		hs_tune[1].tx_pre_emp = res[1];
	} else {
		dev_err(dev, "can't get tx_pre_emp value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_pre_emp_puls", res, 2);
	if (ret == 0) {
		hs_tune[0].tx_pre_emp_puls = res[0];
		hs_tune[1].tx_pre_emp_puls = res[1];
	} else {
		dev_err(dev, "can't get tx_pre_emp_puls value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_res", res, 2);
	if (ret == 0) {
		hs_tune[0].tx_res = res[0];
		hs_tune[1].tx_res = res[1];
	} else {
		dev_err(dev, "can't get tx_res value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_rise", res, 2);
	if (ret == 0) {
		hs_tune[0].tx_rise = res[0];
		hs_tune[1].tx_rise = res[1];
	} else {
		dev_err(dev, "can't get tx_rise value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_hsxv", res, 2);
	if (ret == 0) {
		hs_tune[0].tx_hsxv = res[0];
		hs_tune[1].tx_hsxv = res[1];
	} else {
		dev_err(dev, "can't get tx_hsxv value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_fsls", res, 2);
	if (ret == 0) {
		hs_tune[0].tx_fsls = res[0];
		hs_tune[1].tx_fsls = res[1];
	} else {
		dev_err(dev, "can't get tx_fsls value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "rx_sqrx", res, 2);
	if (ret == 0) {
		hs_tune[0].rx_sqrx = res[0];
		hs_tune[1].rx_sqrx = res[1];
	} else {
		dev_err(dev, "can't get tx_sqrx value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "compdis", res, 2);
	if (ret == 0) {
		hs_tune[0].compdis = res[0];
		hs_tune[1].compdis = res[1];
	} else {
		dev_err(dev, "can't get compdis value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "otg", res, 2);
	if (ret == 0) {
		hs_tune[0].otg = res[0];
		hs_tune[1].otg = res[1];
	} else {
		dev_err(dev, "can't get otg_tune value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "enable_user_imp", res, 2);
	if (ret == 0) {
		if (res[0]) {
			hs_tune[0].enable_user_imp = true;
			hs_tune[1].enable_user_imp = true;
			hs_tune[0].user_imp_value = res[1];
			hs_tune[1].user_imp_value = res[1];
		} else {
			hs_tune[0].enable_user_imp = false;
			hs_tune[1].enable_user_imp = false;
			}
	} else {
		dev_err(dev, "can't get enable_user_imp value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32(node, "is_phyclock", &value);
	if (ret == 0) {
		if (value == 1) {
			hs_tune[0].utmi_clk = USBPHY_UTMI_PHYCLOCK;
			hs_tune[1].utmi_clk = USBPHY_UTMI_PHYCLOCK;
		} else {
			hs_tune[0].utmi_clk = USBPHY_UTMI_FREECLOCK;
			hs_tune[1].utmi_clk = USBPHY_UTMI_FREECLOCK;
	}
	} else {
		dev_err(dev, "can't get is_phyclock value, error = %d\n", ret);
		return -EINVAL;
	}

	return 0;
}

/*
 * Sets the default PHY tuning values for super-speed connection.
 */
static int exynos_usbdrd_fill_sstune(struct exynos_usbdrd_phy *phy_drd,
				     struct device_node *node)
{
	struct device *dev = phy_drd->dev;
	struct exynos_usbphy_ss_tune *ss_tune = phy_drd->ss_value;
	u32 res[2];
	int ret;

	ret = of_property_read_u32_array(node, "tx_boost_level", res, 2);
	if (ret == 0) {
		ss_tune[0].tx_boost_level = res[0];
		ss_tune[1].tx_boost_level = res[1];
	} else {
		dev_err(dev, "can't get tx_boost_level value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_swing_level", res, 2);
	if (ret == 0) {
		ss_tune[0].tx_swing_level = res[0];
		ss_tune[1].tx_swing_level = res[1];
	} else {
		dev_err(dev, "can't get tx_swing_level value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_swing_full", res, 2);
	if (ret == 0) {
		ss_tune[0].tx_swing_full = res[0];
		ss_tune[1].tx_swing_full = res[1];
	} else {
		dev_err(dev, "can't get tx_swing_full value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_swing_low", res, 2);
	if (ret == 0) {
		ss_tune[0].tx_swing_low = res[0];
		ss_tune[1].tx_swing_low = res[1];
	} else {
		dev_err(dev, "can't get tx_swing_low value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_deemphasis_mode", res, 2);
	if (ret == 0) {
		ss_tune[0].tx_deemphasis_mode = res[0];
		ss_tune[1].tx_deemphasis_mode = res[1];
	} else {
		dev_err(dev, "can't get tx_deemphasis_mode value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_deemphasis_3p5db", res, 2);
	if (ret == 0) {
		ss_tune[0].tx_deemphasis_3p5db = res[0];
		ss_tune[1].tx_deemphasis_3p5db = res[1];
	} else {
		dev_err(dev, "can't get tx_deemphasis_3p5db value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "tx_deemphasis_6db", res, 2);
	if (ret == 0) {
		ss_tune[0].tx_deemphasis_6db = res[0];
		ss_tune[1].tx_deemphasis_6db = res[1];
	} else {
		dev_err(dev, "can't get tx_deemphasis_6db value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "enable_ssc", res, 2);
	if (ret == 0) {
		ss_tune[0].enable_ssc = res[0];
		ss_tune[1].enable_ssc = res[1];
	} else {
		dev_err(dev, "can't get enable_ssc value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "ssc_range", res, 2);
	if (ret == 0) {
		ss_tune[0].ssc_range = res[0];
		ss_tune[1].ssc_range = res[1];
	} else {
		dev_err(dev, "can't get ssc_range value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "los_bias", res, 2);
	if (ret == 0) {
		ss_tune[0].los_bias = res[0];
		ss_tune[1].los_bias = res[1];
	} else {
		dev_err(dev, "can't get los_bias value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "los_mask_val", res, 2);
	if (ret == 0) {
		ss_tune[0].los_mask_val = res[0];
		ss_tune[1].los_mask_val = res[1];
	} else {
		dev_err(dev, "can't get los_mask_val value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "enable_fixed_rxeq_mode", res, 2);
	if (ret == 0) {
		ss_tune[0].enable_fixed_rxeq_mode = res[0];
		ss_tune[1].enable_fixed_rxeq_mode = res[1];
	} else {
		dev_err(dev, "can't get enable_fixed_rxeq_mode value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "fix_rxeq_value", res, 2);
	if (ret == 0) {
		ss_tune[0].fix_rxeq_value = res[0];
		ss_tune[1].fix_rxeq_value = res[1];
	} else {
		dev_err(dev, "can't get fix_rxeq_value value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "set_crport_level_en", res, 2);
	if (ret == 0) {
		ss_tune[0].set_crport_level_en = res[0];
		ss_tune[1].set_crport_level_en = res[1];
	} else {
		dev_err(dev, "can't get set_crport_level_en value, error = %d\n", ret);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "set_crport_mpll_charge_pump", res, 2);
	if (ret == 0) {
		ss_tune[0].set_crport_mpll_charge_pump = res[0];
		ss_tune[1].set_crport_mpll_charge_pump = res[1];
	} else {
		dev_err(dev, "can't get set_crport_mpll_charge_pump value, error = %d\n", ret);
		return -EINVAL;
	}

	return 0;
}

static int exynos_usbdrd_fill_hstune_param(struct exynos_usbdrd_phy *phy_drd,
					   struct device_node *node)
{
	struct device *dev = phy_drd->dev;
	struct device_node *child = NULL;
	struct exynos_usb_tune_param *hs_tune_param;
	size_t size = sizeof(struct exynos_usb_tune_param);
	int ret;
	u32 res[2];
	u32 param_index = 0;
	const char *name;

	ret = of_property_read_u32_array(node, "hs_tune_cnt", &res[0], 1);

	if (res[0] > 100 || res[0] < 0)
		return -EINVAL;

	dev_info(dev, "%s hs tune cnt = %d\n", __func__, res[0]);

	hs_tune_param = devm_kzalloc(dev, size * (res[0] + 1), GFP_KERNEL);
	if (!hs_tune_param)
		return -ENOMEM;
	phy_drd->usbphy_info.tune_param = hs_tune_param;

	for_each_child_of_node(node, child) {
		ret = of_property_read_string(child, "tune_name", &name);
		if (ret) {
			dev_err(dev, "failed to read hs tune name from %s node\n", child->name);
			return ret;
		}

		memcpy(hs_tune_param[param_index].name, name, strlen(name));

		ret = of_property_read_u32_array(child, "tune_value", res, 2);
		if (ret) {
			dev_err(dev, "failed to read hs tune value from %s node\n", child->name);
			return -EINVAL;
		}

		phy_drd->hs_tune_param_value[param_index][0] = res[0];
		phy_drd->hs_tune_param_value[param_index][1] = res[1];
		param_index++;
	}

	hs_tune_param[param_index].value = EXYNOS_USB_TUNE_LAST;

	return 0;
}

/*
 * Sets the default PHY tuning values for super-speed connection.
 */
static int exynos_usbdrd_fill_sstune_param(struct exynos_usbdrd_phy *phy_drd,
					   struct device_node *node)
{
	struct device *dev = phy_drd->dev;
	struct device_node *child = NULL;
	struct exynos_usb_tune_param *ss_tune_param;
	size_t size = sizeof(struct exynos_usb_tune_param);
	int ret;
	u32 res[2];
	u32 idx = 0;
	const char *name;

	ret = of_property_read_u32_array(node, "ss_tune_cnt", &res[0], 1);

	dev_info(dev, "%s ss tune cnt = %d\n", __func__, res[0]);

	ss_tune_param = devm_kzalloc(dev, size * (res[0] + 1), GFP_KERNEL);
	if (!ss_tune_param)
		return -ENOMEM;
	phy_drd->usbphy_sub_info.tune_param = ss_tune_param;

	for_each_child_of_node(node, child) {
		ret = of_property_read_string(child, "tune_name", &name);
		if (ret) {
			dev_err(dev, "failed to read ss tune name from %s node\n", child->name);
			return ret;
		}

		memcpy(ss_tune_param[idx].name, name, strlen(name));

		ret = of_property_read_u32_array(child, "tune_value", res, 2);
		if (ret) {
			dev_err(dev, "failed to read ss tune value from %s node\n", child->name);
			return -EINVAL;
		}

		phy_drd->ss_tune_param_value[idx][0] = res[0];
		phy_drd->ss_tune_param_value[idx][1] = res[1];

		if (phy_drd->use_default_tune_val) {
			phy_drd->ss_tune_param_value[idx][0] = -1;
			phy_drd->ss_tune_param_value[idx][1] = -1;
		}

		idx++;
	}

	ss_tune_param[idx].value = EXYNOS_USB_TUNE_LAST;

	return 0;
}

static int exynos_usbdrd_get_phy_refsel(struct exynos_usbdrd_phy *phy_drd)
{
	struct device *dev = phy_drd->dev;
	struct device_node *node = dev->of_node;
	int value, ret;
	int check_flag = 0;

	ret = of_property_read_u32(node, "phy_refsel_clockcore", &value);
	if (ret == 0 && value == 1) {
		phy_drd->usbphy_info.refsel = USBPHY_REFSEL_CLKCORE;
		phy_drd->usbphy_sub_info.refsel = USBPHY_REFSEL_CLKCORE;
		check_flag |= 0x1;
	} else if (ret < 0) {
		dev_err(dev, "can't get phy_refsel_clockcore, error = %d\n", ret);
		return ret;
	}

	ret = of_property_read_u32(node, "phy_refsel_ext_osc", &value);
	if (ret == 0 && value == 1) {
		phy_drd->usbphy_info.refsel = USBPHY_REFSEL_EXT_OSC;
		phy_drd->usbphy_sub_info.refsel = USBPHY_REFSEL_EXT_OSC;
		check_flag |= 0x2;
	} else if (ret < 0) {
		dev_err(dev, "can't get phy_refsel_ext_osc, error = %d\n", ret);
		return ret;
	}

	ret = of_property_read_u32(node, "phy_refsel_xtal", &value);
	if (ret == 0 && value == 1) {
		phy_drd->usbphy_info.refsel = USBPHY_REFSEL_EXT_XTAL;
		phy_drd->usbphy_sub_info.refsel = USBPHY_REFSEL_EXT_XTAL;
		check_flag |= 0x4;
	} else if (ret < 0) {
		dev_err(dev, "can't get phy_refsel_xtal, error = %d\n", ret);
		return ret;
	}

	ret = of_property_read_u32(node, "phy_refsel_diff_pad", &value);
	if (ret == 0 && value == 1) {
		phy_drd->usbphy_info.refsel = USBPHY_REFSEL_DIFF_PAD;
		phy_drd->usbphy_sub_info.refsel = USBPHY_REFSEL_DIFF_PAD;
		check_flag |= 0x8;
	} else if (ret < 0) {
		dev_err(dev, "can't get phy_refsel_diff_pad, error = %d\n", ret);
		return ret;
	}

	ret = of_property_read_u32(node, "phy_refsel_diff_internal", &value);
	if (ret == 0 && value == 1) {
		phy_drd->usbphy_info.refsel = USBPHY_REFSEL_DIFF_INTERNAL;
		phy_drd->usbphy_sub_info.refsel = USBPHY_REFSEL_DIFF_INTERNAL;
		check_flag |= 0x10;
	} else if (ret < 0) {
		dev_err(dev, "can't get phy_refsel_diff_internal, error = %d\n", ret);
		return ret;
	}

	ret = of_property_read_u32(node, "phy_refsel_diff_single", &value);
	if (ret == 0 && value == 1) {
		phy_drd->usbphy_info.refsel = USBPHY_REFSEL_DIFF_SINGLE;
		phy_drd->usbphy_sub_info.refsel = USBPHY_REFSEL_DIFF_SINGLE;
		check_flag |= 0x20;
	} else if (ret < 0) {
		dev_err(dev, "can't get phy_refsel_diff_single, error = %d\n", ret);
		return ret;
	}

	if (check_flag == 0) {
		dev_err(dev, "USB refsel Must be choosed\n");
		return -EINVAL;
	}

	return 0;
}

static int exynos_usbdrd_get_sub_phyinfo(struct exynos_usbdrd_phy *phy_drd)
{
	struct device *dev = phy_drd->dev;
	struct device_node *tune_node;
	int ret;
	int value;
	int mode;

	if (of_property_read_u32(dev->of_node, "sub_phy_version", &value)) {
		dev_err(dev, "can't get sub_phy_version\n");
		return -EINVAL;
	}
	if (of_property_read_u32(dev->of_node, "usbdp_mode", &mode)) {
		dev_err(dev, "can't get usbdp_mode\n");
		return -EINVAL;
	}

	phy_drd->usbphy_sub_info.version = value;
	phy_drd->usbphy_sub_info.refclk = phy_drd->extrefclk;
	phy_drd->usbphy_sub_info.usbdp_mode = mode;

	phy_drd->usbphy_sub_info.regs_base = phy_drd->reg_dpphy_ctrl;
	phy_drd->usbphy_sub_info.regs_base_2nd = phy_drd->reg_dpphy_tca;
	phy_drd->usbphy_sub_info.link_base = phy_drd->reg_link;
	phy_drd->usbphy_sub_info.ctrl_base = phy_drd->reg_phy;

	usbdp_combo_phy_reg = phy_drd->usbphy_sub_info.regs_base;

	tune_node = of_parse_phandle(dev->of_node, "ss_tune_param", 0);
	if (tune_node) {
		ret = exynos_usbdrd_fill_sstune_param(phy_drd, tune_node);
		if (ret < 0) {
			dev_err(dev, "can't fill super speed tuning param\n");
			return -EINVAL;
		}
	}

	return 0;
}

static int exynos_usbdrd_get_phyinfo(struct exynos_usbdrd_phy *phy_drd)
{
	struct device *dev = phy_drd->dev;
	struct device_node *tune_node;
	int ret;
	int value;

	phy_drd->usbphy_info.hs_rewa = 1;
	phy_drd->usbphy_blkcon_info.hs_rewa = 1;

	if (!of_property_read_u32(dev->of_node, "phy_version", &value)) {
		phy_drd->usbphy_blkcon_info.version = value;
	} else {
		dev_err(dev, "can't get phy_version\n");
		return -EINVAL;
	}

	if (!of_property_read_u32(dev->of_node, "phy_eusb_version", &value)) {
		dev_info(dev, "phy_eusb_version = %x\n", value);
		phy_drd->usbphy_info.version = value;
	} else {
		dev_err(dev, "can't get phy_eusb_version\n");
		return -EINVAL;
	}

	if (!of_property_read_u32(dev->of_node, "use_io_for_ovc", &value)) {
		phy_drd->usbphy_info.use_io_for_ovc = value ? true : false;
	} else {
		dev_err(dev, "can't get io_for_ovc\n");
		return -EINVAL;
	}

	if (!of_property_read_u32(dev->of_node, "common_block_disable", &value)) {
		phy_drd->usbphy_info.common_block_disable = value ? true : false;
	} else {
		dev_err(dev, "can't get common_block_disable\n");
		return -EINVAL;
	}

	phy_drd->usbphy_info.refclk = phy_drd->extrefclk;
	phy_drd->usbphy_info.regs_base = phy_drd->reg_eusb_ctrl;
	phy_drd->usbphy_info.regs_base_2nd = phy_drd->reg_eusb_phy;
	phycon_base_addr = phy_drd->usbphy_info.regs_base;

	if (!of_property_read_u32(dev->of_node, "is_not_vbus_pad", &value)) {
		phy_drd->usbphy_info.not_used_vbus_pad = value ? true : false;
	} else {
		dev_err(dev, "can't get vbus_pad\n");
		return -EINVAL;
	}

	if (!of_property_read_u32(dev->of_node, "used_phy_port", &value)) {
		phy_drd->usbphy_info.used_phy_port = value ? true : false;
	} else {
		dev_err(dev, "can't get used_phy_port\n");
		return -EINVAL;
	}

	ret = exynos_usbdrd_get_phy_refsel(phy_drd);
	if (ret < 0) {
		dev_err(dev, "can't get phy refsel\n");
		return -EINVAL;
	}

	tune_node = of_parse_phandle(dev->of_node, "ss_tune_info", 0);
	if (tune_node)
		dev_dbg(dev, "don't need usbphy tuning value for super speed\n");

	if (of_device_is_available(tune_node)) {
		ret = exynos_usbdrd_fill_sstune(phy_drd, tune_node);
		if (ret < 0) {
			dev_err(dev, "can't fill super speed tuning value\n");
			return -EINVAL;
		}
	}

	tune_node = of_parse_phandle(dev->of_node, "hs_tune_info", 0);
	if (tune_node)
		dev_dbg(dev, "don't need usbphy tuning value for high speed\n");

	if (of_device_is_available(tune_node)) {
		ret = exynos_usbdrd_fill_hstune(phy_drd, tune_node);
		if (ret < 0) {
			dev_err(dev, "can't fill high speed tuning value\n");
			return -EINVAL;
		}
	}

	tune_node = of_parse_phandle(dev->of_node, "hs_tune_param", 0);
	if (tune_node) {
		ret = exynos_usbdrd_fill_hstune_param(phy_drd, tune_node);
		if (ret < 0) {
			dev_err(dev, "can't fill high speed tuning param\n");
			return -EINVAL;
		}
	} else {
		dev_dbg(dev, "don't need usbphy tuning param for high speed\n");
	}

	dev_info(phy_drd->dev, "usbphy info: version:0x%x, refclk:0x%x\n",
		 phy_drd->usbphy_info.version, phy_drd->usbphy_info.refclk);

	return 0;
}

static int exynos_usbdrd_get_iptype(struct exynos_usbdrd_phy *phy_drd)
{
	struct device *dev = phy_drd->dev;
	int ret, value;

	ret = of_property_read_u32(dev->of_node, "ip_type", &value);
	if (ret) {
		dev_err(dev, "can't get ip type");
		return ret;
	}

	switch (value) {
	case TYPE_USB3DRD:
		phy_drd->ip_type = TYPE_USB3DRD;
		dev_info(dev, "IP is TYPE USB3DRD");
		break;
	case TYPE_USB3HOST:
		phy_drd->ip_type = TYPE_USB3HOST;
		dev_info(dev, "IP is TYPE USB3HOST");
		break;
	case TYPE_USB2DRD:
		phy_drd->ip_type = TYPE_USB2DRD;
		dev_info(dev, "IP is TYPE USB2DRD");
		break;
	case TYPE_USB2HOST:
		phy_drd->ip_type = TYPE_USB2HOST;
		dev_info(dev, "IP is TYPE USB2HOST");
		break;
	default:
		break;
	}

	return 0;
}

static void exynos_usbdrd_update_phy_value(struct exynos_usbdrd_phy *phy_drd)
{
	struct exynos_usb_tune_param *hs_tune_param = phy_drd->usbphy_info.tune_param;
	int i;

	for (i = 0; hs_tune_param[i].value != EXYNOS_USB_TUNE_LAST; i++) {
		if (i == EXYNOS_DRD_MAX_TUNEPARAM_NUM)
			break;
		hs_tune_param[i].value = phy_drd->hs_tune_param_value[i][USBPHY_MODE_DEV];
	}

	return;
}

static int exynos_usbdrd_usb_update(struct notifier_block *nb,
				    unsigned long action, void *dev)
{
	struct exynos_usbdrd_phy *phy_drd = container_of(nb, struct exynos_usbdrd_phy, usb_nb);
	union extcon_property_value property = { 0 };

	if (action) {
		extcon_get_property(phy_drd->edev, EXTCON_USB, EXTCON_PROP_USB_TYPEC_POLARITY,
				    &property);
		phy_drd->usbphy_info.used_phy_port = property.intval;
		phy_drd->usbphy_sub_info.used_phy_port = property.intval;

		dev_info(phy_drd->dev, "%s: phy port[%d]\n", __func__,
			 phy_drd->usbphy_info.used_phy_port);
	}

	return NOTIFY_OK;
}

static int exynos_usbdrd_usb_host_update(struct notifier_block *nb,
					 unsigned long action, void *dev)
{
	struct exynos_usbdrd_phy *phy_drd =
		container_of(nb, struct exynos_usbdrd_phy, usb_host_nb);
	union extcon_property_value property = { 0 };

	if (action) {
		extcon_get_property(phy_drd->edev, EXTCON_USB_HOST, EXTCON_PROP_USB_TYPEC_POLARITY,
				    &property);
		phy_drd->usbphy_info.used_phy_port = property.intval;
		phy_drd->usbphy_sub_info.used_phy_port = property.intval;

		dev_info(phy_drd->dev, "%s: phy port[%d]\n", __func__,
			 phy_drd->usbphy_info.used_phy_port);
	}

	return NOTIFY_OK;
}

static int exynos_usbdrd_extcon_register(struct exynos_usbdrd_phy *phy_drd)
{
	struct device *dev = phy_drd->dev;
	int ret = 0;

	if (!of_property_read_bool(dev->of_node, "extcon"))
		return -EINVAL;

	phy_drd->edev = extcon_get_edev_by_phandle(dev, 0);
	if (IS_ERR_OR_NULL(phy_drd->edev)) {
		dev_err(dev, "couldn't get extcon\n");
		return phy_drd->edev ? PTR_ERR(phy_drd->edev) : -ENODEV;
	}

	phy_drd->usb_nb.notifier_call = exynos_usbdrd_usb_update;
	ret = extcon_register_notifier(phy_drd->edev, EXTCON_USB, &phy_drd->usb_nb);
	if (ret < 0) {
		dev_err(dev, "EXTCON_USB notifier register failed\n");
		return ret;
	}

	phy_drd->usb_host_nb.notifier_call = exynos_usbdrd_usb_host_update;
	ret = extcon_register_notifier(phy_drd->edev, EXTCON_USB_HOST, &phy_drd->usb_host_nb);
	if (ret < 0)
		dev_err(dev, "EXTCON_USB_HOST notifier register failed\n");

	return ret;
}

static void exynos_usbdrd_pipe3_exit(struct exynos_usbdrd_phy *phy_drd)
{
	/* pipe3 phy disable is exucuted in utmi_exit.
	 * Later divide the exit of main and sub phy if necessary
	 */
}

static void exynos_usbdrd_utmi_exit(struct exynos_usbdrd_phy *phy_drd)
{
	if (phy_drd->use_phy_umux) {
		/*USB User MUX disable */
		exynos_usbdrd_clk_disable(phy_drd, true);
	}
	phy_exynos_eusb_terminate(&phy_drd->usbphy_info);
	phy_exynos_snps_usbdp_phy_disable(&phy_drd->usbphy_sub_info);

	exynos_usbcon_disable_pipe3_phy(&phy_drd->usbphy_blkcon_info);

	exynos_usbdrd_clk_disable(phy_drd, false);

	exynos_usbdrd_utmi_phy_isol(&phy_drd->phys[0], 1,
				    phy_drd->phys[0].pmu_mask);
	exynos_usbdrd_pipe3_phy_isol(&phy_drd->phys[1], 1,
				     phy_drd->phys[1].pmu_mask);

#if IS_ENABLED(CONFIG_PHY_EXYNOS_EUSB_REPEATER)
	eusb_repeater_power_off();
#endif
}

static int exynos_usbdrd_phy_exit(struct phy *phy)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	/* UTMI or PIPE3 specific exit */
	inst->phy_cfg->phy_exit(phy_drd);

	return 0;
}

static void exynos_usbdrd_pipe3_init(struct exynos_usbdrd_phy *phy_drd)
{
	struct phy_usb_instance *inst = &phy_drd->phys[1];
	int ret = 0;
	union extcon_property_value property = { 0 };

	inst->phy_cfg->phy_isol(inst, 0, inst->pmu_mask);

	if (!phy_drd->edev) {
		ret = exynos_usbdrd_extcon_register(phy_drd);
		if (!ret) {
			if (extcon_get_state(phy_drd->edev, EXTCON_USB)) {
				ret = extcon_get_property(phy_drd->edev, EXTCON_USB,
							EXTCON_PROP_USB_TYPEC_POLARITY, &property);
			} else if (extcon_get_state(phy_drd->edev, EXTCON_USB_HOST)) {
				ret = extcon_get_property(phy_drd->edev, EXTCON_USB_HOST,
							EXTCON_PROP_USB_TYPEC_POLARITY, &property);
			}

			phy_drd->usbphy_info.used_phy_port = property.intval;
			phy_drd->usbphy_sub_info.used_phy_port = property.intval;

			dev_info(phy_drd->dev, "phy port[%d]\n",
				 phy_drd->usbphy_info.used_phy_port);
		}
	}

	/* Fill USBDP Combo phy init */
	exynos_usbdrd_pipe3_phy_isol(&phy_drd->phys[1], 0,
				     phy_drd->phys[1].pmu_mask);
	exynos_usbcon_ready_to_pipe3_phy(&phy_drd->usbphy_blkcon_info);
	phy_exynos_snps_usbdp_phy_enable(&phy_drd->usbphy_sub_info);
}

static void exynos_usbdrd_utmi_init(struct exynos_usbdrd_phy *phy_drd)
{
	int ret;
	struct phy_usb_instance *inst = &phy_drd->phys[0];
#if IS_ENABLED(CONFIG_EXYNOS_OTP)
	struct tune_bits *otp_data;
	u8 otp_type;
	u8 otp_index;
	u8 i;
#endif

#if IS_ENABLED(CONFIG_PHY_EXYNOS_EUSB_REPEATER)
	eusb_repeater_power_on();
#endif

	//phy power on
	inst->phy_cfg->phy_isol(inst, 0, inst->pmu_mask);

	ret = exynos_usbdrd_clk_enable(phy_drd, false);
	if (ret) {
		dev_err(phy_drd->dev, "%s: Failed to enable clk\n", __func__);
		return;
	}

	exynos_usbcon_init_link(&phy_drd->usbphy_blkcon_info);

	exynos_usbdrd_update_phy_value(phy_drd);

	phy_exynos_eusb_initiate(&phy_drd->usbphy_info);

	if (phy_drd->use_phy_umux) {
		/* USB User MUX enable */
		ret = exynos_usbdrd_clk_enable(phy_drd, true);
		if (ret) {
			dev_err(phy_drd->dev, "%s: Failed to enable clk\n", __func__);
			return;
		}
	}
#if IS_ENABLED(CONFIG_EXYNOS_OTP)
	if (phy_drd->ip_type < TYPE_USB2DRD) {
		otp_type = phy_drd->otp_type[OTP_USB3PHY_INDEX];
		otp_index = phy_drd->otp_index[OTP_USB3PHY_INDEX];
		otp_data = phy_drd->otp_data[OTP_USB3PHY_INDEX];
	} else {
		otp_type = phy_drd->otp_type[OTP_USB2PHY_INDEX];
		otp_index = phy_drd->otp_index[OTP_USB2PHY_INDEX];
		otp_data = phy_drd->otp_data[OTP_USB2PHY_INDEX];
	}

	for (i = 0; i < otp_index; i++) {
		samsung_exynos_cal_usb3phy_write_register(&phy_drd->usbphy_info,
							  otp_data[i].index * otp_type,
							  otp_data[i].value);
	}
#endif
}

static int exynos_usbdrd_phy_init(struct phy *phy)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	/* UTMI or PIPE3 specific init */
	inst->phy_cfg->phy_init(phy_drd);

	return 0;
}

static void exynos_usbdrd_utmi_ilbk(struct exynos_usbdrd_phy *phy_drd)
{

}

static void exynos_usbdrd_pipe3_ilbk(struct exynos_usbdrd_phy *phy_drd)
{

}

static int exynos_usbdrd_pipe3_vendor_set(struct exynos_usbdrd_phy *phy_drd,
					  int is_enable, int is_cancel)
{
	if (is_cancel == 0) {
		exynos_usbcon_u3_rewa_enable(&phy_drd->usbphy_blkcon_info, 0);
		enable_irq(phy_drd->usb3_irq_wakeup);
	} else {
		disable_irq_nosync(phy_drd->usb3_irq_wakeup);
		exynos_usbcon_u3_rewa_disable(&phy_drd->usbphy_blkcon_info);
	}

	return 0;
}

static int exynos_usbdrd_utmi_vendor_set(struct exynos_usbdrd_phy *phy_drd,
					 int is_enable, int is_cancel)
{
	int ret = 0;

	dev_dbg(phy_drd->dev, "rewa irq : %d, enable: %d, cancel: %d\n",
		 phy_drd->is_irq_enabled, is_enable, is_cancel);

	if (is_cancel) {
		if (is_enable) {
			if (phy_drd->is_irq_enabled == 1) {
				dev_info(phy_drd->dev, "[%s] REWA CANCEL\n", __func__);
				exynos_usbcon_rewa_cancel(&phy_drd->usbphy_blkcon_info);

				dev_info(phy_drd->dev, "REWA wakeup/conn IRQ disable\n");

				disable_irq_nosync(phy_drd->irq_wakeup);
				disable_irq_nosync(phy_drd->irq_conn);
				phy_drd->is_irq_enabled = 0;
			} else {
				dev_dbg(phy_drd->dev, "Vendor set by interrupt, Do not REWA cancel\n");
			}
		}
	} else {
		if (is_enable) {
			ret = exynos_usbcon_enable_rewa(&phy_drd->usbphy_blkcon_info);
			if (ret) {
				dev_err(phy_drd->dev, "REWA ENABLE FAIL, ret : %d\n", ret);
				return ret;
			}

			/* inform what USB state is idle to IDLE_IP */
			//exynos_update_ip_idle_status(phy_drd->idle_ip_idx, 1);

			dev_info(phy_drd->dev, "REWA ENABLE Complete\n");

			if (phy_drd->is_irq_enabled == 0) {
				enable_irq(phy_drd->irq_wakeup);
				enable_irq(phy_drd->irq_conn);
				phy_drd->is_irq_enabled = 1;
			} else {
				dev_dbg(phy_drd->dev, "rewa irq already enabled\n");
			}
		} else {
			dev_dbg(phy_drd->dev, "REWA Disconn & Wakeup IRQ DISABLE\n");

			/* inform what USB state is not idle to IDLE_IP */
			//exynos_update_ip_idle_status(phy_drd->idle_ip_idx, 0);

			ret = exynos_usbcon_rewa_disable(&phy_drd->usbphy_blkcon_info);
			if (ret) {
				dev_err(phy_drd->dev, "REWA DISABLE FAIL, ret : %d\n", ret);
				return ret;
			}

			disable_irq_nosync(phy_drd->irq_wakeup);
			disable_irq_nosync(phy_drd->irq_conn);
			phy_drd->is_irq_enabled = 0;

			dev_dbg(phy_drd->dev, "REWA DISABLE Complete\n");
		}
	}
	return ret;
}

static void exynos_usbdrd_pipe3_tune(struct exynos_usbdrd_phy *phy_drd,
				     int phy_state)
{
	struct exynos_usb_tune_param *ss_tune_param = phy_drd->usbphy_sub_info.tune_param;
	int i;

	if (!ss_tune_param) {
		dev_err(phy_drd->dev, "no tune param\n");
		return;
	}

	for (i = 0; ss_tune_param[i].value != EXYNOS_USB_TUNE_LAST; i++) {
		if (i == EXYNOS_DRD_MAX_TUNEPARAM_NUM)
			break;
		ss_tune_param[i].value = phy_drd->ss_tune_param_value[i][USBPHY_MODE_DEV];
	}
	phy_exynos_snps_usbdp_tune(&phy_drd->usbphy_sub_info);
}

static void exynos_usbdrd_utmi_tune(struct exynos_usbdrd_phy *phy_drd,
				    int phy_state)
{

}

int exynos_usbdrd_phy_tune(struct phy *phy, int phy_state)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	inst->phy_cfg->phy_tune(phy_drd, phy_state);

	return 0;
}
EXPORT_SYMBOL_GPL(exynos_usbdrd_phy_tune);

/*
 * USB LDO control was moved to phy_conn API from OTG
 * without adding one more phy interface
 */
void exynos_usbdrd_phy_conn(struct phy *phy, int is_conn)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	/* ldo control is moved to power sw */
	if (is_conn) {
		dev_info(phy_drd->dev, "USB PHY Conn Set\n");
		phy_drd->is_conn = 1;
	} else {
		dev_info(phy_drd->dev, "USB PHY Conn Clear\n");
		phy_drd->is_conn = 0;
	}
}
EXPORT_SYMBOL_GPL(exynos_usbdrd_phy_conn);

int exynos_usbdrd_dp_ilbk(struct phy *phy)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	inst->phy_cfg->phy_ilbk(phy_drd);

	return 0;
}
EXPORT_SYMBOL_GPL(exynos_usbdrd_dp_ilbk);

int exynos_usbdrd_phy_vendor_set(struct phy *phy, int is_enable,
				 int is_cancel)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);
	int ret;

	ret = inst->phy_cfg->phy_vendor_set(phy_drd, is_enable, is_cancel);

	return ret;
}
EXPORT_SYMBOL_GPL(exynos_usbdrd_phy_vendor_set);

static void exynos_usbdrd_pipe3_set(struct exynos_usbdrd_phy *phy_drd,
				    int option, void *info)
{
	/* Fill USBDP Combo phy set */
}

static void exynos_usbdrd_utmi_set(struct exynos_usbdrd_phy *phy_drd,
				   int option, void *info)
{
	pr_info("%s blkcon fix en:%d\n", __func__, option);
	exynos_usbcon_dp_pullup_en(&phy_drd->usbphy_blkcon_info, option);
}

int exynos_usbdrd_phy_link_rst(struct phy *phy)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	pr_info("%s\n", __func__);
	phy_exynos_eusb_reset(&phy_drd->usbphy_info);
	return 0;
}

int exynos_usbdrd_phy_set(struct phy *phy, enum phy_mode mode, int submode)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	inst->phy_cfg->phy_set(phy_drd, (int)mode, (void *)&phy_drd->usbphy_info);

	return 0;
}
EXPORT_SYMBOL_GPL(exynos_usbdrd_phy_set);

static int exynos_usbdrd_phy_power_on(struct phy *phy)
{
	int ret = 0;
#ifdef SKIP_DWC3_CORE_POWER_CONTROL
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	dev_dbg(phy_drd->dev, "Request to power_on usbdrd_phy phy\n");

	/* Enable VBUS supply */
	if (phy_drd->vbus) {
		ret = regulator_enable(phy_drd->vbus);
		if (ret) {
			dev_err(phy_drd->dev, "Failed to enable VBUS supply\n");
			return ret;
		}
	}

	inst->phy_cfg->phy_isol(inst, 0, inst->pmu_mask);
#endif

	return ret;
}

static struct device_node *exynos_usbdrd_parse_dt(void)
{
	struct device_node *np = NULL;

	np = of_find_compatible_node(NULL, NULL, "samsung,exynos-usbdrd-phy");
	if (!np)
		pr_err("%s: failed to get the usbdrd node\n", __func__);

	return np;
}

static struct exynos_usbdrd_phy *exynos_usbdrd_get_struct(void)
{
	struct device_node *np = NULL;
	struct platform_device *pdev = NULL;
	struct device *dev;
	struct exynos_usbdrd_phy *phy_drd;

	np = exynos_usbdrd_parse_dt();
	if (!np) {
		pr_err("%s: failed to get the device node\n", __func__);
		return NULL;
	}

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		pr_err("%s: failed to get the platform_device\n", __func__);
		return NULL;
	}
	dev = &pdev->dev;
	of_node_put(np);
	phy_drd = dev->driver_data;

	return phy_drd;
}

#if defined(PHY_IDLE_IP_SET)
static int exynos_usbdrd_get_idle_ip(void)
{
	struct device_node *np = NULL;
	struct platform_device *pdev = NULL;
	struct device *dev;
	int idle_ip_idx;

	np = of_find_compatible_node(NULL, NULL, "samsung,exynos9-dwusb");
	if (!np) {
		pr_err("%s: failed to get the device node\n", __func__);
		return -1;
	}

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		pr_err("%s: failed to get the platform_device\n", __func__);
		return NULL;
	}
	dev = &pdev->dev;
	of_node_put(np);
	dev_info(dev, "%s: get the %s platform_device\n", __func__, pdev->name);

	idle_ip_idx = exynos_get_idle_ip_index(dev_name(dev));
	dev_info(dev, "%s, idle ip = %d\n", __func__, idle_ip_idx);
	return idle_ip_idx;
}
#endif

static int exynos_usbdrd_phy_power_off(struct phy *phy)
{
#ifdef SKIP_DWC3_CORE_POWER_CONTROL
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	dev_info(phy_drd->dev, "Request to power_off usbdrd_phy phy\n");

	inst->phy_cfg->phy_isol(inst, 1, inst->pmu_mask);

	/* Disable VBUS supply */
	if (phy_drd->vbus)
		regulator_disable(phy_drd->vbus);
#endif

	return 0;
}

int exynos_usbdrd_s2mpu_manual_control(bool on)
{
	struct exynos_usbdrd_phy *phy_drd;
	int (*__s2mpu_notify)(struct device *dev, bool on);

	pr_debug("%s s2mpu = %d\n", __func__, on);

	phy_drd = exynos_usbdrd_get_struct();

	if (!phy_drd) {
		pr_err("[%s] exynos_usbdrd_get_struct error\n", __func__);
		return -ENODEV;
	}

	/* Paired with cmpxchg_release in exynos_usbdrd_set_s2mpu_pm_ops. */
	__s2mpu_notify = smp_load_acquire(&s2mpu_notify);
	if (!phy_drd->s2mpu || !__s2mpu_notify)
		return 0;

	__s2mpu_notify(phy_drd->s2mpu, on);

	return 0;
}
EXPORT_SYMBOL_GPL(exynos_usbdrd_s2mpu_manual_control);

int exynos_usbdrd_pipe3_enable(struct phy *phy)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	/* Fill USBDP Combo phy init */
	exynos_usbcon_ready_to_pipe3_phy(&phy_drd->usbphy_blkcon_info);
	phy_exynos_snps_usbdp_phy_enable(&phy_drd->usbphy_sub_info);

	return 0;
}
EXPORT_SYMBOL_GPL(exynos_usbdrd_pipe3_enable);

int exynos_usbdrd_pipe3_disable(struct phy *phy)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);

	exynos_usbcon_detach_pipe3_phy(&phy_drd->usbphy_blkcon_info);
	phy_exynos_snps_usbdp_phy_disable(&phy_drd->usbphy_sub_info);

	return 0;
}
EXPORT_SYMBOL_GPL(exynos_usbdrd_pipe3_disable);

void exynos_usbdrd_usbdp_tca_set(struct phy *phy, int mux, int low_power_en)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);
	union extcon_property_value property = { 0 };

	extcon_get_property(phy_drd->edev, EXTCON_USB_HOST, EXTCON_PROP_USB_TYPEC_POLARITY,
			    &property);
	phy_drd->usbphy_info.used_phy_port = property.intval;
	phy_drd->usbphy_sub_info.used_phy_port = property.intval;

	phy_exynos_snps_usbdp_tca_set(&phy_drd->usbphy_sub_info, mux, low_power_en);
}
EXPORT_SYMBOL_GPL(exynos_usbdrd_usbdp_tca_set);

void exynos_usbdrd_dp_use_notice(int lane)
{
	struct exynos_usbdrd_phy *phy_drd;

	pr_info("%s: lane: %d\n", __func__, lane);

	phy_drd = exynos_usbdrd_get_struct();

	if (!phy_drd) {
		pr_err("[%s] exynos_usbdrd_get_struct error\n", __func__);
		return;
	}

	exynos_usbcon_detach_pipe3_phy(&phy_drd->usbphy_blkcon_info);
}
EXPORT_SYMBOL_GPL(exynos_usbdrd_dp_use_notice);

static struct phy *exynos_usbdrd_phy_xlate(struct device *dev,
					   struct of_phandle_args *args)
{
	struct exynos_usbdrd_phy *phy_drd = dev_get_drvdata(dev);

	if (WARN_ON(args->args[0] > EXYNOS_DRDPHYS_NUM))
		return ERR_PTR(-ENODEV);

	return phy_drd->phys[args->args[0]].phy;
}

static irqreturn_t exynos_usbdrd_usb3_phy_wakeup_interrupt(int irq, void *_phydrd)
{
	struct exynos_usbdrd_phy *phy_drd = (struct exynos_usbdrd_phy *)_phydrd;

	exynos_usbcon_u3_rewa_disable(&phy_drd->usbphy_blkcon_info);
	dev_dbg(phy_drd->dev, "[%s] USB3 ReWA disabled...\n", __func__);

	return IRQ_HANDLED;
}

static irqreturn_t exynos_usbdrd_phy_wakeup_interrupt(int irq, void *_phydrd)
{
	struct exynos_usbdrd_phy *phy_drd = (struct exynos_usbdrd_phy *)_phydrd;
	int ret;

	ret = exynos_usbcon_rewa_req_sys_valid(&phy_drd->usbphy_blkcon_info);
	dev_dbg(phy_drd->dev, "[%s] rewa sys valid set : %s \n",
		 __func__, (ret == 1) ? "Disable" : "Disconnect");

	return IRQ_HANDLED;
}

static irqreturn_t exynos_usbdrd_phy_conn_interrupt(int irq, void *_phydrd)
{
	struct exynos_usbdrd_phy *phy_drd = (struct exynos_usbdrd_phy *)_phydrd;
	int ret;

	ret = exynos_usbcon_rewa_req_sys_valid(&phy_drd->usbphy_blkcon_info);
	dev_dbg(phy_drd->dev, "[%s] rewa sys valid set : %s \n",
		 __func__, (ret == 1) ? "Disable" : "Disconnect");

	return IRQ_HANDLED;
}

static struct phy_ops exynos_usbdrd_phy_ops = {
	.init		= exynos_usbdrd_phy_init,
	.exit		= exynos_usbdrd_phy_exit,
	.power_on	= exynos_usbdrd_phy_power_on,
	.power_off	= exynos_usbdrd_phy_power_off,
	.reset		= exynos_usbdrd_phy_link_rst,
	.set_mode	= exynos_usbdrd_phy_set,
	.owner		= THIS_MODULE,
};

static const struct exynos_usbdrd_phy_config phy_cfg_exynos[] = {
	{
		.id		= EXYNOS_DRDPHY_UTMI,
		.phy_isol	= exynos_usbdrd_utmi_phy_isol,
		.phy_init	= exynos_usbdrd_utmi_init,
		.phy_exit	= exynos_usbdrd_utmi_exit,
		.phy_tune	= exynos_usbdrd_utmi_tune,
		.phy_vendor_set	= exynos_usbdrd_utmi_vendor_set,
		.phy_ilbk	= exynos_usbdrd_utmi_ilbk,
		.phy_set	= exynos_usbdrd_utmi_set,
		.set_refclk	= exynos_usbdrd_utmi_set_refclk,
	},
	{
		.id		= EXYNOS_DRDPHY_PIPE3,
		.phy_isol	= exynos_usbdrd_pipe3_phy_isol,
		.phy_init	= exynos_usbdrd_pipe3_init,
		.phy_exit	= exynos_usbdrd_pipe3_exit,
		.phy_tune	= exynos_usbdrd_pipe3_tune,
		.phy_vendor_set	= exynos_usbdrd_pipe3_vendor_set,
		.phy_ilbk	= exynos_usbdrd_pipe3_ilbk,
		.phy_set	= exynos_usbdrd_pipe3_set,
		.set_refclk	= exynos_usbdrd_pipe3_set_refclk,
	},
};

static const struct exynos_usbdrd_phy_drvdata exynos_usbdrd_phy = {
	.phy_cfg		= phy_cfg_exynos,
};

static const struct of_device_id exynos_usbdrd_phy_of_match[] = {
	{
		.compatible = "samsung,exynos-usbdrd-phy",
		.data = &exynos_usbdrd_phy
	},
	{ },
};
MODULE_DEVICE_TABLE(of, exynos_usbdrd_phy_of_match);

void __iomem *phy_exynos_usbdp_get_address(void)
{
	return usbdp_combo_phy_reg;
}

static int exynos_usbdrd_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct exynos_usbdrd_phy *phy_drd;
	struct phy_provider *phy_provider;
	struct resource *res;
	const struct of_device_id *match;
	const struct exynos_usbdrd_phy_drvdata *drv_data;
	struct regmap *reg_pmu;
	struct device_node *syscon_np;
	struct resource pmu_res;
	struct device_node	*s2mpu_np;
	struct platform_device	*s2mpu_pdev;
	u32 pmu_offset, pmu_offset_dp, pmu_offset_tcxo;
	u32 pmu_mask, pmu_mask_tcxo, pmu_mask_pll;
	u32 phy_ref_clock;
	int i, ret;

	s2mpu_np = of_parse_phandle(dev->of_node, "s2mpus", 0);
	if (s2mpu_np) {
		s2mpu_pdev = of_find_device_by_node(s2mpu_np);
		of_node_put(s2mpu_np);
	}

#if IS_ENABLED(CONFIG_EXYNOS_PD_HSI0)
	if (!exynos_pd_hsi0_get_ldo_status()) {
		dev_err(dev, "pd-hsi0 is not powered, deferred probe!");
		return -EPROBE_DEFER;
	}
#endif

	phy_drd = devm_kzalloc(dev, sizeof(*phy_drd), GFP_KERNEL);
	if (!phy_drd)
		return -ENOMEM;

	dev_set_drvdata(dev, phy_drd);
	phy_drd->dev = dev;
	if (s2mpu_pdev)
		phy_drd->s2mpu = &s2mpu_pdev->dev;
	else
		phy_drd->s2mpu = NULL;

	match = of_match_node(exynos_usbdrd_phy_of_match, pdev->dev.of_node);

	drv_data = match->data;
	phy_drd->drv_data = drv_data;

	phy_drd->irq_wakeup = platform_get_irq(pdev, 0);
	irq_set_status_flags(phy_drd->irq_wakeup, IRQ_NOAUTOEN);
	ret = devm_request_irq(dev, phy_drd->irq_wakeup, exynos_usbdrd_phy_wakeup_interrupt,
			       0, "phydrd-wakeup", phy_drd);
	if (ret) {
		dev_err(dev, "failed to request irq #%d --> %d\n",
			phy_drd->irq_wakeup, ret);
		return ret;
	}
	irq_set_irq_wake(phy_drd->irq_wakeup, 1);

	phy_drd->irq_conn = platform_get_irq(pdev, 1);
	irq_set_status_flags(phy_drd->irq_conn, IRQ_NOAUTOEN);
	ret = devm_request_irq(dev, phy_drd->irq_conn, exynos_usbdrd_phy_conn_interrupt,
			       0, "usb2-phydrd-conn", phy_drd);
	if (ret) {
		dev_err(dev, "failed to request irq #%d --> %d\n",
			phy_drd->irq_conn, ret);
		return ret;
	}
	irq_set_irq_wake(phy_drd->irq_conn, 1);

	phy_drd->usb3_irq_wakeup = platform_get_irq(pdev, 2);
	irq_set_status_flags(phy_drd->usb3_irq_wakeup, IRQ_NOAUTOEN);
	ret = devm_request_irq(dev, phy_drd->usb3_irq_wakeup,
					exynos_usbdrd_usb3_phy_wakeup_interrupt,
					0, "usb3-phydrd-wakeup", phy_drd);
	if (ret) {
		dev_err(dev, "failed to request irq #%d --> %d (For SS ReWA)\n",
				phy_drd->usb3_irq_wakeup, ret);
		/* Don't return probe failure for compatibility */
		dev_err(dev, "Don't return probe failure for compatibility.\n");
	} else {
		irq_set_irq_wake(phy_drd->usb3_irq_wakeup, 1);
	}

	/* ioremap for blkcon */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	phy_drd->reg_phy = devm_ioremap_resource(dev, res);
	if (IS_ERR(phy_drd->reg_phy))
		return PTR_ERR(phy_drd->reg_phy);

	phy_drd->usbphy_blkcon_info.refclk = phy_drd->extrefclk;
	phy_drd->usbphy_blkcon_info.regs_base = phy_drd->reg_phy;
	phycon_base_addr = phy_drd->reg_phy; // ESS_CTL

	/* ioremap for eusb phy */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	phy_drd->reg_eusb_ctrl = devm_ioremap_resource(dev, res);
	if (IS_ERR(phy_drd->reg_eusb_ctrl))
		return PTR_ERR(phy_drd->reg_eusb_ctrl);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	phy_drd->reg_eusb_phy = devm_ioremap_resource(dev, res);
	if (IS_ERR(phy_drd->reg_eusb_phy))
		return PTR_ERR(phy_drd->reg_eusb_phy);

	/*
	 * Both has_other_phy and has_combo_phy can't be enabled
	 * at the same time. It's alternative.
	 */
	if (!of_property_read_u32(dev->of_node, "has_other_phy", &ret)) {
		if (ret) {
			res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
			phy_drd->reg_phy2 = devm_ioremap_resource(dev, res);
			if (IS_ERR(phy_drd->reg_phy2))
				return PTR_ERR(phy_drd->reg_phy2);
		}
	}

	ret = exynos_usbdrd_get_iptype(phy_drd);
	if (ret) {
		dev_err(dev, "%s: Failed to get ip_type\n", __func__);
		return ret;
	}

	ret = exynos_usbdrd_clk_get(phy_drd);
	if (ret) {
		dev_err(dev, "%s: Failed to get clocks\n", __func__);
		goto skip_clock;
	}

	ret = exynos_usbdrd_clk_prepare(phy_drd);
	if (ret) {
		dev_err(dev, "%s: Failed to prepare clocks\n", __func__);
		return ret;
	}

	ret = of_property_read_u32(dev->of_node, "phy_ref_clock", &phy_ref_clock);
	if (ret < 0) {
		dev_err(dev, "%s: Couldn't read phy_ref_clock %s node, error = %d\n",
			__func__, dev->of_node->name, ret);
		phy_ref_clock = 0;
	} else {
		clk_set_rate(phy_drd->ref_clk, phy_ref_clock);
	}

	ret = exynos_rate_to_clk(phy_drd);
	if (ret) {
		dev_err(phy_drd->dev, "%s: Not supported ref clock\n",
			__func__);
		goto err1;
	}
skip_clock:

	reg_pmu = syscon_regmap_lookup_by_phandle(dev->of_node,
						  "samsung,pmu-syscon");
	if (IS_ERR(reg_pmu)) {
		dev_err(dev, "Failed to lookup PMU regmap\n");
		goto err1;
	}

	syscon_np = of_parse_phandle(dev->of_node, "samsung,pmu-syscon", 0);
	if (!syscon_np) {
		dev_err(dev, "syscon device node not found\n");
		ret = -EINVAL;
		goto err1;
	}

	if (of_address_to_resource(syscon_np, 0, &pmu_res)) {
		dev_err(dev, "failed to get syscon base address\n");
		ret = -ENOMEM;
		goto err1;
	}

	ret = of_property_read_u32(dev->of_node, "pmu_offset", &pmu_offset);
	if (ret < 0) {
		dev_err(dev, "couldn't read pmu_offset on %s node, error = %d\n",
			dev->of_node->name, ret);
		goto err1;
	}
	ret = of_property_read_u32(dev->of_node, "pmu_offset_dp", &pmu_offset_dp);
	if (ret < 0) {
		dev_err(dev, "couldn't read pmu_offset_dp on %s node, error = %d\n",
			dev->of_node->name, ret);
		goto err1;
	}
	ret = of_property_read_u32(dev->of_node, "pmu_mask", &pmu_mask);
	if (ret < 0) {
		dev_err(dev, "couldn't read pmu_mask on %s node, error = %d\n",
			dev->of_node->name, ret);
		goto err1;
	} else {
		pmu_mask = (u32)BIT(pmu_mask);
	}

	ret = of_property_read_u32(dev->of_node,
				   "pmu_offset_tcxobuf", &pmu_offset_tcxo);
	if (ret < 0) {
		dev_err(dev, "couldn't read pmu_offset_tcxo on %s node, error = %d\n",
			dev->of_node->name, ret);
	}
	ret = of_property_read_u32(dev->of_node,
				   "pmu_mask_tcxobuf", &pmu_mask_tcxo);
	if (ret < 0) {
		dev_err(dev, "couldn't read pmu_mask_tcxo on %s node, error = %d\n",
			dev->of_node->name, ret);
		pmu_mask_tcxo = 0;
	} else {
		pmu_mask_tcxo = (u32)BIT(pmu_mask_tcxo);
	}

	ret = of_property_read_u32(dev->of_node,
				   "pmu_mask_pll", &pmu_mask_pll);
	if (ret < 0) {
		dev_err(dev, "couldn't read pmu_mask_pll on %s node, error = %d\n",
			dev->of_node->name, ret);
		pmu_mask_pll = 0;
	} else {
		pmu_mask_pll = (u32)BIT(pmu_mask_pll);
	}

	if (pmu_mask_pll)
		pmu_mask |= (u32)pmu_mask_pll;
	dev_info(dev, "pmu_mask = 0x%x\n", pmu_mask);

	dev_vdbg(dev, "Creating usbdrd_phy phy\n");
	phy_drd->phy_port =  of_get_named_gpio(dev->of_node,
					       "phy,gpio_phy_port", 0);
	if (gpio_is_valid(phy_drd->phy_port)) {
		dev_info(dev, "PHY CON Selection OK\n");

		ret = gpio_request(phy_drd->phy_port, "PHY_CON");
		if (ret)
			dev_err(dev, "fail to request gpio %s:%d\n", "PHY_CON", ret);
		else
			gpio_direction_input(phy_drd->phy_port);
	} else {
		dev_err(dev, "non-DT: PHY CON Selection\n");
	}

	ret = exynos_usbdrd_extcon_register(phy_drd);
	if (ret < 0)
		phy_drd->edev = 0;

	ret = of_property_read_u32(dev->of_node, "reverse_con_dir", &phy_drd->reverse_phy_port);
	dev_dbg(dev, "reverse_con_dir = %d\n", phy_drd->reverse_phy_port);
	if (ret < 0)
		phy_drd->reverse_phy_port = 0;

	ret = exynos_usbdrd_get_phyinfo(phy_drd);
	if (ret)
		goto err1;

	if (!of_property_read_u32(dev->of_node, "use_default_tune_val", &ret)) {
		if (ret) {
			dev_info(dev, "Use default tune value for SS/SSP\n");
			phy_drd->use_default_tune_val = 1;
		} else {
			phy_drd->use_default_tune_val = 0;
		}
	}

	if (!of_property_read_u32(dev->of_node, "has_combo_phy", &ret)) {
		if (ret) {
			res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
			phy_drd->reg_dpphy_ctrl = devm_ioremap_resource(dev, res);
			if (IS_ERR(phy_drd->reg_dpphy_ctrl))
				return PTR_ERR(phy_drd->reg_dpphy_ctrl);

			res = platform_get_resource(pdev, IORESOURCE_MEM, 4);
			phy_drd->reg_dpphy_tca = devm_ioremap_resource(dev, res);
			if (IS_ERR(phy_drd->reg_dpphy_tca))
				return PTR_ERR(phy_drd->reg_dpphy_tca);

			res = platform_get_resource(pdev, IORESOURCE_MEM, 5);
			/* In case of phy driver, we use ioremap() function
			 * because same address will be used at USB driver.
			 */
			phy_drd->reg_link =
				ioremap(res->start, resource_size(res));
			if (IS_ERR(phy_drd->reg_link))
				return PTR_ERR(phy_drd->reg_link);

			exynos_usbdrd_get_sub_phyinfo(phy_drd);
		}
	}

#if IS_ENABLED(CONFIG_EXYNOS_OTP)
	exynos_usbdrd_phy_get_otp_info(phy_drd);
#endif

	for (i = 0; i < EXYNOS_DRDPHYS_NUM; i++) {
		struct phy *phy = devm_phy_create(dev, NULL,
						  &exynos_usbdrd_phy_ops);
		if (IS_ERR(phy)) {
			dev_err(dev, "Failed to create usbdrd_phy phy\n");
			goto err1;
		}

		phy_drd->phys[i].phy = phy;
		phy_drd->phys[i].index = i;
		phy_drd->phys[i].reg_pmu = reg_pmu;
		phy_drd->phys[i].pmu_alive_pa = pmu_res.start;
		phy_drd->phys[i].pmu_offset = pmu_offset;
		phy_drd->phys[i].pmu_offset_dp = pmu_offset_dp;
		phy_drd->phys[i].pmu_mask = pmu_mask;
		phy_drd->phys[i].pmu_offset_tcxobuf = pmu_offset_tcxo;
		phy_drd->phys[i].pmu_mask_tcxobuf = pmu_mask_tcxo;
		phy_drd->phys[i].phy_cfg = &drv_data->phy_cfg[i];
		phy_set_drvdata(phy, &phy_drd->phys[i]);
	}
#if IS_ENABLED(CONFIG_PHY_EXYNOS_DEBUGFS)
	ret = exynos_usbdrd_debugfs_init(phy_drd);
	if (ret) {
		dev_err(dev, "Failed to initialize debugfs\n");
		goto err1;
	}
#endif

#if IS_ENABLED(CONFIG_PHY_EXYNOS_DP_DEBUGFS)
	ret = exynos_usbdrd_dp_debugfs_init(phy_drd);
	if (ret) {
		dev_err(dev, "Failed to initialize dp debugfs\n");
		goto err1;
	}
#endif

	/*
	 *phy_drd->idle_ip_idx = exynos_usbdrd_get_idle_ip();
	 *if (phy_drd->idle_ip_idx < 0)
	 *	dev_err(dev, "Failed to get idle ip index\n");
	 */
	phy_provider = devm_of_phy_provider_register(dev,
						     exynos_usbdrd_phy_xlate);
	if (IS_ERR(phy_provider))
		goto err1;

	spin_lock_init(&phy_drd->lock);

	phy_drd->is_irq_enabled = 0;
	phy_drd->is_usb3_rewa_enabled = 0;
	pm_runtime_enable(dev);

	return 0;
err1:
	exynos_usbdrd_clk_unprepare(phy_drd);

	return ret;
}

#ifdef CONFIG_PM
static int exynos_usbdrd_phy_resume(struct device *dev)
{
	struct exynos_usbdrd_phy *phy_drd = dev_get_drvdata(dev);

	dev_dbg(dev, "%s, is_conn = %d\n",
		 __func__, phy_drd->is_conn);

	return 0;
}

static const struct dev_pm_ops exynos_usbdrd_phy_dev_pm_ops = {
	.resume	= exynos_usbdrd_phy_resume,
};

#define EXYNOS_USBDRD_PHY_PM_OPS	(&(exynos_usbdrd_phy_dev_pm_ops))
#else
#define EXYNOS_USBDRD_PHY_PM_OPS	NULL
#endif

static struct platform_driver phy_exynos_usbdrd = {
	.probe	= exynos_usbdrd_phy_probe,
	.driver = {
		.of_match_table	= exynos_usbdrd_phy_of_match,
		.name		= "phy_exynos_usbdrd",
		.dev_groups	= exynos_usbdrd_groups,
		.pm		= EXYNOS_USBDRD_PHY_PM_OPS,
	}
};

module_platform_driver(phy_exynos_usbdrd);
MODULE_DESCRIPTION("Samsung EXYNOS SoCs USB DRD controller PHY driver");
MODULE_AUTHOR("Vivek Gautam <gautam.vivek@samsung.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:phy_exynos_usbdrd");

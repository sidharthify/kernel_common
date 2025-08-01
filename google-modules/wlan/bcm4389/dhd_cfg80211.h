/*
 * Linux cfg80211 driver - Dongle Host Driver (DHD) related
 *
 * Copyright (C) 2025, Broadcom.
 *
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 *
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 *
 *
 * <<Broadcom-WL-IPTag/Open:>>
 *
 * $Id$
 */

#ifndef __DHD_CFG80211__
#define __DHD_CFG80211__

#include <wl_cfg80211.h>
#include <wl_cfgp2p.h>
#include <brcm_nl80211.h>

#ifndef WL_ERR
#define WL_ERR CFG80211_ERR
#endif
#ifndef WL_TRACE
#define WL_TRACE CFG80211_TRACE
#endif

s32 dhd_cfg80211_init(struct bcm_cfg80211 *cfg);
s32 dhd_cfg80211_deinit(struct bcm_cfg80211 *cfg);
s32 dhd_cfg80211_down(struct bcm_cfg80211 *cfg);
s32 dhd_cfg80211_set_p2p_info(struct bcm_cfg80211 *cfg, int val);
s32 dhd_cfg80211_clean_p2p_info(struct bcm_cfg80211 *cfg);
s32 dhd_config_dongle(struct bcm_cfg80211 *cfg);
int dhd_cfgvendor_priv_string_handler(struct bcm_cfg80211 *cfg,
	struct wireless_dev *wdev, const struct bcm_nlmsg_hdr *nlioc, void  *data);
s32 wl_dongle_roam(struct net_device *ndev, u32 roamvar, u32 bcn_timeout);
int dhd_set_wsec_info(dhd_pub_t *dhd, uint32 data, int tag);
#ifdef RPM_FAST_TRIGGER
void dhd_trigger_rpm_fast(struct bcm_cfg80211 *cfg);
#endif /* RPM_FAST_TRIGGER */
#endif /* __DHD_CFG80211__ */

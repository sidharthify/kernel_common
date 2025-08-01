/*
 * bcmevent read-only data shared by kernel or app layers
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
 * <<Broadcom-WL-IPTag/Dual:>>
 */

#include <typedefs.h>
#include <bcmutils.h>
#include <bcmendian.h>
#include <ethernet.h>
#include <bcmeth.h>
#include <bcmevent.h>
#include <802.11.h>
#include <802.11brcm.h>

/* Table of event name strings for UIs and debugging dumps */
typedef struct {
	uint event;
	const char *name;
} bcmevent_name_str_t;

/* Use the actual name for event tracing */
#define BCMEVENT_NAME(_event) {(_event), #_event}

/* this becomes static data when all code is changed to use
 * the bcmevent_get_name() API
 */
static const bcmevent_name_str_t bcmevent_names[] = {
	BCMEVENT_NAME(WLC_E_SET_SSID),
	BCMEVENT_NAME(WLC_E_JOIN),
	BCMEVENT_NAME(WLC_E_START),
	BCMEVENT_NAME(WLC_E_AUTH),
	BCMEVENT_NAME(WLC_E_AUTH_IND),
	BCMEVENT_NAME(WLC_E_DEAUTH),
	BCMEVENT_NAME(WLC_E_DEAUTH_IND),
	BCMEVENT_NAME(WLC_E_ASSOC),
	BCMEVENT_NAME(WLC_E_ASSOC_IND),
	BCMEVENT_NAME(WLC_E_REASSOC),
	BCMEVENT_NAME(WLC_E_REASSOC_IND),
	BCMEVENT_NAME(WLC_E_DISASSOC),
	BCMEVENT_NAME(WLC_E_DISASSOC_IND),
	BCMEVENT_NAME(WLC_E_QUIET_START),
	BCMEVENT_NAME(WLC_E_QUIET_END),
	BCMEVENT_NAME(WLC_E_BEACON_RX),
	BCMEVENT_NAME(WLC_E_LINK),
	BCMEVENT_NAME(WLC_E_MIC_ERROR),
	BCMEVENT_NAME(WLC_E_NDIS_LINK),
	BCMEVENT_NAME(WLC_E_ROAM),
	BCMEVENT_NAME(WLC_E_TXFAIL),
	BCMEVENT_NAME(WLC_E_PMKID_CACHE),
	BCMEVENT_NAME(WLC_E_RETROGRADE_TSF),
	BCMEVENT_NAME(WLC_E_PRUNE),
	BCMEVENT_NAME(WLC_E_AUTOAUTH),
	BCMEVENT_NAME(WLC_E_EAPOL_MSG),
	BCMEVENT_NAME(WLC_E_SCAN_COMPLETE),
	BCMEVENT_NAME(WLC_E_ADDTS_IND),
	BCMEVENT_NAME(WLC_E_DELTS_IND),
	BCMEVENT_NAME(WLC_E_BCNSENT_IND),
	BCMEVENT_NAME(WLC_E_BCNRX_MSG),
	BCMEVENT_NAME(WLC_E_BCNLOST_MSG),
	BCMEVENT_NAME(WLC_E_ROAM_PREP),
	BCMEVENT_NAME(WLC_E_PFN_NET_FOUND),
	BCMEVENT_NAME(WLC_E_PFN_SCAN_ALLGONE),
	BCMEVENT_NAME(WLC_E_PFN_NET_LOST),
	BCMEVENT_NAME(WLC_E_JOIN_START),
	BCMEVENT_NAME(WLC_E_ROAM_START),
	BCMEVENT_NAME(WLC_E_ASSOC_START),
#ifdef EXT_STA
	BCMEVENT_NAME(WLC_E_RESET_COMPLETE),
	BCMEVENT_NAME(WLC_E_JOIN_START),
	BCMEVENT_NAME(WLC_E_ROAM_START),
	BCMEVENT_NAME(WLC_E_ASSOC_START),
	BCMEVENT_NAME(WLC_E_ASSOC_RECREATED),
	BCMEVENT_NAME(WLC_E_SPEEDY_RECREATE_FAIL),
#endif /* EXT_STA */
#if defined(IBSS_PEER_DISCOVERY_EVENT)
	BCMEVENT_NAME(WLC_E_IBSS_ASSOC),
#endif /* defined(IBSS_PEER_DISCOVERY_EVENT) */
	BCMEVENT_NAME(WLC_E_RADIO),
	BCMEVENT_NAME(WLC_E_PSM_WATCHDOG),
	BCMEVENT_NAME(WLC_E_PROBREQ_MSG),
	BCMEVENT_NAME(WLC_E_SCAN_CONFIRM_IND),
	BCMEVENT_NAME(WLC_E_PSK_SUP),
	BCMEVENT_NAME(WLC_E_COUNTRY_CODE_CHANGED),
	BCMEVENT_NAME(WLC_E_EXCEEDED_MEDIUM_TIME),
	BCMEVENT_NAME(WLC_E_ICV_ERROR),
	BCMEVENT_NAME(WLC_E_UNICAST_DECODE_ERROR),
	BCMEVENT_NAME(WLC_E_MULTICAST_DECODE_ERROR),
	BCMEVENT_NAME(WLC_E_TRACE),
	BCMEVENT_NAME(WLC_E_IF),
#ifdef WLP2P
	BCMEVENT_NAME(WLC_E_P2P_DISC_LISTEN_COMPLETE),
#endif
	BCMEVENT_NAME(WLC_E_RSSI),
	BCMEVENT_NAME(WLC_E_PFN_SCAN_COMPLETE),
	BCMEVENT_NAME(WLC_E_ACTION_FRAME),
	BCMEVENT_NAME(WLC_E_ACTION_FRAME_RX),
	BCMEVENT_NAME(WLC_E_ACTION_FRAME_COMPLETE),
#if defined(NDIS)
	BCMEVENT_NAME(WLC_E_PRE_ASSOC_IND),
	BCMEVENT_NAME(WLC_E_PRE_REASSOC_IND),
	BCMEVENT_NAME(WLC_E_CHANNEL_ADOPTED),
	BCMEVENT_NAME(WLC_E_AP_STARTED),
	BCMEVENT_NAME(WLC_E_DFS_AP_STOP),
	BCMEVENT_NAME(WLC_E_DFS_AP_RESUME),
	BCMEVENT_NAME(WLC_E_ASSOC_IND_NDIS),
	BCMEVENT_NAME(WLC_E_REASSOC_IND_NDIS),
	BCMEVENT_NAME(WLC_E_ACTION_FRAME_RX_NDIS),
	BCMEVENT_NAME(WLC_E_AUTH_REQ),
	BCMEVENT_NAME(WLC_E_IBSS_COALESCE),
#endif /* #if defined(NDIS) */

#ifdef BCMWAPI_WAI
	BCMEVENT_NAME(WLC_E_WAI_STA_EVENT),
	BCMEVENT_NAME(WLC_E_WAI_MSG),
#endif /* BCMWAPI_WAI */

	BCMEVENT_NAME(WLC_E_ESCAN_RESULT),
	BCMEVENT_NAME(WLC_E_ACTION_FRAME_OFF_CHAN_COMPLETE),
#ifdef WLP2P
	BCMEVENT_NAME(WLC_E_PROBRESP_MSG),
	BCMEVENT_NAME(WLC_E_P2P_PROBREQ_MSG),
#endif
#ifdef PROP_TXSTATUS
	BCMEVENT_NAME(WLC_E_FIFO_CREDIT_MAP),
#endif
	BCMEVENT_NAME(WLC_E_WAKE_EVENT),
	BCMEVENT_NAME(WLC_E_DCS_REQUEST),
	BCMEVENT_NAME(WLC_E_RM_COMPLETE),
	BCMEVENT_NAME(WLC_E_OVERLAY_REQ),
	BCMEVENT_NAME(WLC_E_CSA_COMPLETE_IND),
	BCMEVENT_NAME(WLC_E_EXCESS_PM_WAKE_EVENT),
	BCMEVENT_NAME(WLC_E_PFN_SCAN_NONE),
	BCMEVENT_NAME(WLC_E_PFN_SCAN_ALLGONE),
#ifdef SOFTAP
	BCMEVENT_NAME(WLC_E_GTK_PLUMBED),
#endif
	BCMEVENT_NAME(WLC_E_ASSOC_REQ_IE),
	BCMEVENT_NAME(WLC_E_ASSOC_RESP_IE),
	BCMEVENT_NAME(WLC_E_BEACON_FRAME_RX),
#ifdef WLTDLS
	BCMEVENT_NAME(WLC_E_TDLS_PEER_EVENT),
#endif /* WLTDLS */
	BCMEVENT_NAME(WLC_E_NATIVE),
#ifdef WLPKTDLYSTAT
	BCMEVENT_NAME(WLC_E_PKTDELAY_IND),
#endif /* WLPKTDLYSTAT */
	BCMEVENT_NAME(WLC_E_SERVICE_FOUND),
	BCMEVENT_NAME(WLC_E_GAS_FRAGMENT_RX),
	BCMEVENT_NAME(WLC_E_GAS_COMPLETE),
	BCMEVENT_NAME(WLC_E_P2PO_ADD_DEVICE),
	BCMEVENT_NAME(WLC_E_P2PO_DEL_DEVICE),
#ifdef WLWNM
	BCMEVENT_NAME(WLC_E_WNM_STA_SLEEP),
#endif /* WLWNM */
#if defined(WL_PROXDETECT) || defined(RTT_SUPPORT)
	BCMEVENT_NAME(WLC_E_PROXD),
#endif
	BCMEVENT_NAME(WLC_E_CCA_CHAN_QUAL),
	BCMEVENT_NAME(WLC_E_BSSID),
#ifdef PROP_TXSTATUS
	BCMEVENT_NAME(WLC_E_BCMC_CREDIT_SUPPORT),
#endif
	BCMEVENT_NAME(WLC_E_PSTA_PRIMARY_INTF_IND),
	BCMEVENT_NAME(WLC_E_TXFAIL_THRESH),
#ifdef GSCAN_SUPPORT
	BCMEVENT_NAME(WLC_E_PFN_GSCAN_FULL_RESULT),
	BCMEVENT_NAME(WLC_E_PFN_SSID_EXT),
#endif /* GSCAN_SUPPORT */
#ifdef WLBSSLOAD_REPORT
	BCMEVENT_NAME(WLC_E_BSS_LOAD),
#endif
#ifdef WLFBT
	BCMEVENT_NAME(WLC_E_FBT),
#endif /* WLFBT */
	BCMEVENT_NAME(WLC_E_AUTHORIZED),
	BCMEVENT_NAME(WLC_E_PROBREQ_MSG_RX),

	BCMEVENT_NAME(WLC_E_CSA_START_IND),
	BCMEVENT_NAME(WLC_E_CSA_DONE_IND),
	BCMEVENT_NAME(WLC_E_CSA_FAILURE_IND),
	BCMEVENT_NAME(WLC_E_RMC_EVENT),
	BCMEVENT_NAME(WLC_E_DPSTA_INTF_IND),
	BCMEVENT_NAME(WLC_E_ALLOW_CREDIT_BORROW),
	BCMEVENT_NAME(WLC_E_MSCH),
	BCMEVENT_NAME(WLC_E_ULP),
	BCMEVENT_NAME(WLC_E_NAN),
	BCMEVENT_NAME(WLC_E_PKT_FILTER),
	BCMEVENT_NAME(WLC_E_DMA_TXFLUSH_COMPLETE),
	BCMEVENT_NAME(WLC_E_PSK_AUTH),
	BCMEVENT_NAME(WLC_E_SDB_TRANSITION),
	BCMEVENT_NAME(WLC_E_PFN_SCAN_BACKOFF),
	BCMEVENT_NAME(WLC_E_PFN_BSSID_SCAN_BACKOFF),
	BCMEVENT_NAME(WLC_E_AGGR_EVENT),
	BCMEVENT_NAME(WLC_E_TVPM_MITIGATION),
	BCMEVENT_NAME(WLC_E_SCAN),
	BCMEVENT_NAME(WLC_E_SLOTTED_BSS_PEER_OP),
	BCMEVENT_NAME(WLC_E_PHY_CAL),
#ifdef WL_NAN
	BCMEVENT_NAME(WLC_E_NAN_CRITICAL),
	BCMEVENT_NAME(WLC_E_NAN_NON_CRITICAL),
	BCMEVENT_NAME(WLC_E_NAN),
#endif /* WL_NAN */
	BCMEVENT_NAME(WLC_E_RPSNOA),
	BCMEVENT_NAME(WLC_E_WA_LQM),
	BCMEVENT_NAME(WLC_E_OBSS_DETECTION),
	BCMEVENT_NAME(WLC_E_SC_CHAN_QUAL),
	BCMEVENT_NAME(WLC_E_DYNSAR),
	BCMEVENT_NAME(WLC_E_ROAM_CACHE_UPDATE),
	BCMEVENT_NAME(WLC_E_AP_BCN_DRIFT),
	BCMEVENT_NAME(WLC_E_PFN_SCAN_ALLGONE_EXT),
#ifdef WL_CLIENT_SAE
	BCMEVENT_NAME(WLC_E_AUTH_START),
#endif /* WL_CLIENT_SAE */
#ifdef WL_TWT
	BCMEVENT_NAME(WLC_E_TWT),
#endif /* WL_TWT */
	BCMEVENT_NAME(WLC_E_AMT),
	BCMEVENT_NAME(WLC_E_ROAM_SCAN_RESULT),
#if defined(XRAPI)
	BCMEVENT_NAME(WLC_E_XR_SOFTAP_PSMODE),
#endif /* XRAPI */
#ifdef WL_SIB_COEX
	BCMEVENT_NAME(WLC_E_SIB),
#endif /* WL_SIB_COEX */
	BCMEVENT_NAME(WLC_E_MSCS),
	BCMEVENT_NAME(WLC_E_RXDMA_RECOVERY_ATMPT),
#ifdef WL_SCHED_SCAN
	BCMEVENT_NAME(WLC_E_PFN_PARTIAL_RESULT),
#endif /* WL_SCHED_SCAN */
	BCMEVENT_NAME(WLC_E_MLO_LINK_INFO),
	BCMEVENT_NAME(WLC_E_C2C),
	BCMEVENT_NAME(WLC_E_BCN_TSF),
	BCMEVENT_NAME(WLC_E_OWE_INFO),
};

const char *bcmevent_get_name(uint event_type)
{
	/* note:  first coded this as a static const but some
	 * ROMs already have something called event_name so
	 * changed it so we don't have a variable for the
	 * 'unknown string
	 */
	const char *event_name = NULL;

	uint idx;
	for (idx = 0; idx < (uint)ARRAYSIZE(bcmevent_names); idx++) {

		if (bcmevent_names[idx].event == event_type) {
			event_name = bcmevent_names[idx].name;
			break;
		}
	}

	/* if we find an event name in the array, return it.
	 * otherwise return unknown string.
	 */
	return ((event_name) ? event_name : "Unknown Event");
}

void
wl_event_to_host_order(wl_event_msg_t * evt)
{
	/* Event struct members passed from dongle to host are stored in network
	* byte order. Convert all members to host-order.
	*/
	evt->event_type = ntoh32(evt->event_type);
	evt->flags = ntoh16(evt->flags);
	evt->status = ntoh32(evt->status);
	evt->reason = ntoh32(evt->reason);
	evt->auth_type = ntoh32(evt->auth_type);
	evt->datalen = ntoh32(evt->datalen);
	evt->version = ntoh16(evt->version);
}

void
wl_event_to_network_order(wl_event_msg_t * evt)
{
	/* Event struct members passed from dongle to host are stored in network
	* byte order. Convert all members to host-order.
	*/
	evt->event_type = hton32(evt->event_type);
	evt->flags = hton16(evt->flags);
	evt->status = hton32(evt->status);
	evt->reason = hton32(evt->reason);
	evt->auth_type = hton32(evt->auth_type);
	evt->datalen = hton32(evt->datalen);
	evt->version = hton16(evt->version);
}

/*
 * Validate if the event is proper and if valid copy event header to event.
 * If proper event pointer is passed, to just validate, pass NULL to event.
 *
 * Return values are
 *	BCME_OK - It is a BRCM event or BRCM dongle event
 *	BCME_NOTFOUND - Not BRCM, not an event, may be okay
 *	BCME_BADLEN - Bad length, should not process, just drop
 */
int
is_wlc_event_frame(void *pktdata, uint pktlen, uint16 exp_usr_subtype,
	bcm_event_msg_u_t *out_event)
{
	uint16 evlen = 0;	/* length in bcmeth_hdr */
	uint16 subtype;
	uint16 usr_subtype;
	bcm_event_t *bcm_event;
	uint8 *pktend;
	uint8 *evend;
	int err = BCME_OK;
	uint32 data_len = 0; /* data length in bcm_event */

	pktend = (uint8 *)pktdata + pktlen;
	bcm_event = (bcm_event_t *)pktdata;

	/* only care about 16-bit subtype / length versions */
	if ((uint8 *)&bcm_event->bcm_hdr < pktend) {
		uint8 short_subtype = *(uint8 *)&bcm_event->bcm_hdr;
		if (!(short_subtype & 0x80)) {
			err = BCME_NOTFOUND;
			goto done;
		}
	}

	/* must have both ether_header and bcmeth_hdr */
	if (pktlen < OFFSETOF(bcm_event_t, event)) {
		err = BCME_BADLEN;
		goto done;
	}

	/* check length in bcmeth_hdr */

#ifdef BCMDONGLEHOST
	/* temporary - header length not always set properly. When the below
	 * !BCMDONGLEHOST is in all branches that use trunk DHD, the code
	 * under BCMDONGLEHOST can be removed.
	 */
	evlen = (uint16)(pktend - (uint8 *)&bcm_event->bcm_hdr.version);
#else
	evlen = ntoh16_ua((void *)&bcm_event->bcm_hdr.length);
#endif /* BCMDONGLEHOST */
	evend = (uint8 *)&bcm_event->bcm_hdr.version + evlen;
	if (evend != pktend) {
		err = BCME_BADLEN;
		goto done;
	}

	/* match on subtype, oui and usr subtype for BRCM events */
	subtype = ntoh16_ua((void *)&bcm_event->bcm_hdr.subtype);
	if (subtype != BCMILCP_SUBTYPE_VENDOR_LONG) {
		err = BCME_NOTFOUND;
		goto done;
	}

	if (bcmp(BRCM_OUI, &bcm_event->bcm_hdr.oui[0], DOT11_OUI_LEN)) {
		err = BCME_NOTFOUND;
		goto done;
	}

	/* if it is a bcm_event or bcm_dngl_event_t, validate it */
	usr_subtype = ntoh16_ua((void *)&bcm_event->bcm_hdr.usr_subtype);
	switch (usr_subtype) {
	case BCMILCP_BCM_SUBTYPE_EVENT:
		/* check that header length and pkt length are sufficient */
		if ((pktlen < sizeof(bcm_event_t)) ||
			(evend < ((uint8 *)bcm_event + sizeof(bcm_event_t)))) {
			err = BCME_BADLEN;
			goto done;
		}

		/* ensure data length in event is not beyond the packet. */
		data_len = ntoh32_ua((void *)&bcm_event->event.datalen);
		if ((sizeof(bcm_event_t) + data_len +
			BCMILCP_BCM_SUBTYPE_EVENT_DATA_PAD) != pktlen) {
			err = BCME_BADLEN;
			goto done;
		}

		if (exp_usr_subtype && (exp_usr_subtype != usr_subtype)) {
			err = BCME_NOTFOUND;
			goto done;
		}

		if (out_event) {
			/* ensure BRCM event pkt aligned */
			memcpy(&out_event->event, &bcm_event->event, sizeof(wl_event_msg_t));
		}

		break;

	case BCMILCP_BCM_SUBTYPE_DNGLEVENT:
#if defined(HEALTH_CHECK) || defined(DNGL_EVENT_SUPPORT)
		if ((pktlen < sizeof(bcm_dngl_event_t)) ||
			(evend < ((uint8 *)bcm_event + sizeof(bcm_dngl_event_t)))) {
			err = BCME_BADLEN;
			goto done;
		}

		/* ensure data length in event is not beyond the packet. */
		data_len = ntoh16_ua((void *)&((bcm_dngl_event_t *)pktdata)->dngl_event.datalen);
		if ((sizeof(bcm_dngl_event_t) + data_len +
			BCMILCP_BCM_SUBTYPE_EVENT_DATA_PAD) != pktlen) {
			err = BCME_BADLEN;
			goto done;
		}

		if (exp_usr_subtype && (exp_usr_subtype != usr_subtype)) {
			err = BCME_NOTFOUND;
			goto done;
		}

		if (out_event) {
			/* ensure BRCM dngl event pkt aligned */
			memcpy(&out_event->dngl_event, &((bcm_dngl_event_t *)pktdata)->dngl_event,
				sizeof(bcm_dngl_event_msg_t));
		}

		break;
#else
		err = BCME_UNSUPPORTED;
		break;
#endif /* HEALTH_CHECK  || DNGL_EVENT_SUPPORT */

	default:
		err = BCME_NOTFOUND;
		goto done;
	}

	BCM_REFERENCE(data_len);
done:
	return err;
}

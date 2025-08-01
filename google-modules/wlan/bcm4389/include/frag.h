/*
 * IE/TLV (de)fragmentation declarations/definitions for
 * Broadcom 802.11abgn Networking Device Driver
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
 *
 */

#ifndef __FRAG_H__
#define __FRAG_H__

int bcm_tlv_dot11_defrag(const void *buf, uint buf_len, uint8 id, bool id_ext,
	uint8 *out, uint *out_len, uint *data_len);

int bcm_tlv_dot11_frag_tot_len(const void *buf, uint buf_len,
	uint8 id, bool id_ext, uint *ie_len);
int bcm_tlv_dot11_frag_tot_data_len(const void *buf, uint buf_len,
	uint8 id, bool id_ext, uint *ie_len, uint *data_len);

/* To support IE fragmentation.
 * data will be fit into a series of elements consisting of one leading element
 * followed by Fragment elements if needed.
 * data_len is in/out parameter, it has the length of all the elements, including
 * the leading element header size.
 * Caller can get elements length by passing dst as NULL.
 */
int bcm_tlv_dot11_frag(uint8 id, bool id_ext, const void *data, uint data_len,
	uint8 *dst, uint *ie_len);
#endif /* __FRAG_H__ */

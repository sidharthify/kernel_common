/*
 * Interface definitions for reversed TLVs
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

#ifndef _dngl_rtlv_h_
#define _dngl_rtlv_h_

#ifndef	_LANGUAGE_ASSEMBLY

#include <typedefs.h>

/* Types of reverse TLVs downloaded to the top of dongle RAM.
 * A reverse TLV consists of:
 *     data <variable length>
 *     len  <4 bytes>
 *     type <4 bytes>
 */

typedef uint32	dngl_rtlv_type_t;
typedef uint32	dngl_rtlv_len_t;

/* Search for a reversed TLV with the given type, starting at the given address */
int dngl_rtlv_find(const uint8 *rtlv_ptr, const uint8 *addr_limit, dngl_rtlv_type_t type,
	dngl_rtlv_len_t *out_len, const uint8 **out_data);

/* Search for a reversed TLV with the given type, starting at the top of RAM */
int dngl_rtlv_find_from_ramtop(dngl_rtlv_type_t type, dngl_rtlv_len_t *out_len,
	const uint8 **out_data);

/* Search for the end of the reversed TLVs at the top of RAM to return the next RAM address */
const uint8* dngl_rtlv_skipall(void);

#ifdef RTLV_DEBUG
void dbg_log_rtlv(const char* str, const void* p1, const void* p2, const void *p3,
	const void* p4, const void *p5);
#else /* RTLV_DEBUG */
#define dbg_log_rtlv(str, p1, p2, p3, p4, p5)
#endif /* RTLV_DEBUG */

#endif /* !_LANGUAGE_ASSEMBLY */

/* All DHD->firmware TLV type codes are declared here.
 * This file may be included into assembly files
 */

#define DNGL_RTLV_TYPE_NONE			0
/* RNG random data. replaces bcmrand.h BCM_NVRAM_RNG_SIGNATURE */
#define DNGL_RTLV_TYPE_RNG_SIGNATURE		0xFEEDC0DEu
/* FW signature */
#define DNGL_RTLV_TYPE_FW_SIGNATURE		0xFEEDFE51u
/* NVRAM signature */
#define DNGL_RTLV_TYPE_NVRAM_SIGNATURE		0xFEEDFE52u
/* FW signing memory map */
#define DNGL_RTLV_TYPE_FWSIGN_MEM_MAP		0xFEEDFE53u
/* signature verification status */
#define DNGL_RTLV_TYPE_FWSIGN_STATUS		0xFEEDFE54u
/* host page location */
#define DNGL_RTLV_TYPE_HOST_PAGE_LOCATION	0xFEED10C5u
/* host fwtrace buffer location */
#define DNGL_RTLV_HOST_FWTRACE_BUF_LOCATION	0xFEED10C6u
/* end of rTLVs marker */
#define DNGL_RTLV_TYPE_END_MARKER		0xFEED0E2Du

#endif /* _dngl_rtlv_h_ */

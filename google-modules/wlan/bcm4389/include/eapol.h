/*
 * 802.1x EAPOL definitions
 *
 * See
 * IEEE Std 802.1X-2001
 * IEEE 802.1X RADIUS Usage Guidelines
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

#ifndef _eapol_h_
#define _eapol_h_

#ifndef _TYPEDEFS_H_
#include <typedefs.h>
#endif

/* This marks the start of a packed structure section. */
#include <packed_section_start.h>

#if !defined(BCMCRYPTO_COMPONENT)
#include <bcmcrypto/aeskeywrap.h>
#endif /* !BCMCRYPTO_COMPONENT */

/* EAPOL for 802.3/Ethernet */
typedef BWL_PRE_PACKED_STRUCT struct {
	struct ether_header eth;		/* 802.3/Ethernet header */
	unsigned char version;			/* EAPOL protocol version */
	unsigned char type;			/* EAPOL type */
	unsigned short length;			/* Length of body */
	unsigned char body[BCM_FLEX_ARRAY];	/* Body (optional) */
} BWL_POST_PACKED_STRUCT eapol_header_t;

#define EAPOL_HEADER_LEN 18

typedef struct {
	unsigned char version;		/* EAPOL protocol version */
	unsigned char type;		/* EAPOL type */
	unsigned short length;		/* Length of body */
} eapol_hdr_t;

#define EAPOL_HDR_LEN 4u

/* EAPOL version */
#define WPA2_EAPOL_VERSION	2u
#define WPA_EAPOL_VERSION	1u
#define LEAP_EAPOL_VERSION	1u
#define SES_EAPOL_VERSION	1u

/* EAPOL types */
#define EAP_PACKET		0
#define EAPOL_START		1u
#define EAPOL_LOGOFF		2u
#define EAPOL_KEY		3u
#define EAPOL_ASF		4u

/* EAPOL-Key types */
#define EAPOL_RC4_KEY		1u
#define EAPOL_WPA2_KEY		2u	/* 802.11i/WPA2 */
#define EAPOL_WPA_KEY		254u	/* WPA */

/* RC4 EAPOL-Key header field sizes */
#define EAPOL_KEY_REPLAY_LEN	8u
#define EAPOL_KEY_IV_LEN	16u
#define EAPOL_KEY_SIG_LEN	16u

/* RC4 EAPOL-Key */
typedef BWL_PRE_PACKED_STRUCT struct {
	unsigned char type;				/* Key Descriptor Type */
	unsigned short length;				/* Key Length (unaligned) */
	unsigned char replay[EAPOL_KEY_REPLAY_LEN];	/* Replay Counter */
	unsigned char iv[EAPOL_KEY_IV_LEN];		/* Key IV */
	unsigned char index;				/* Key Flags & Index */
	unsigned char signature[EAPOL_KEY_SIG_LEN];	/* Key Signature */
	unsigned char key[BCM_FLEX_ARRAY];		/* Key (optional) */
} BWL_POST_PACKED_STRUCT eapol_key_header_t;

#define EAPOL_KEY_HEADER_LEN	44u

/* RC4 EAPOL-Key flags */
#define EAPOL_KEY_FLAGS_MASK	0x80u
#define EAPOL_KEY_BROADCAST	0u
#define EAPOL_KEY_UNICAST	0x80u

/* RC4 EAPOL-Key index */
#define EAPOL_KEY_INDEX_MASK	0x7fu

/* WPA/802.11i/WPA2 EAPOL-Key header field sizes */
#define EAPOL_AKW_BLOCK_LEN 8
#define EAPOL_WPA_KEY_REPLAY_LEN	8u
#define EAPOL_WPA_KEY_NONCE_LEN		32u
#define EAPOL_WPA_KEY_IV_LEN		16u
#define EAPOL_WPA_KEY_RSC_LEN		8u
#define EAPOL_WPA_KEY_ID_LEN		8u
#define EAPOL_WPA_KEY_DATA_LEN		(EAPOL_WPA_MAX_KEY_SIZE + EAPOL_AKW_BLOCK_LEN)
#define EAPOL_WPA_MAX_KEY_SIZE		32u
#define EAPOL_WPA_KEY_MAX_MIC_LEN	32u
#define EAPOL_WPA_ENCR_KEY_MAX_LEN	64u
#define EAPOL_WPA_TEMP_ENCR_KEY_MAX_LEN	32u

#define EAPOL_WPA_PMK_MAX_LEN           64u
#define EAPOL_WPA_PMK_SHA384_LEN        48u
#define EAPOL_WPA_PMK_DEFAULT_LEN	32u
#define EAPOL_WPA_KCK_DEFAULT_LEN	16u
#define EAPOL_WPA_KCK_SHA384_LEN	24u
#define EAPOL_WPA_KCK_MIC_DEFAULT_LEN	16u
#define EAPOL_WPA_KCK_MIC_SHA384_LEN	24u
#define EAPOL_WPA_ENCR_KEY_DEFAULT_LEN	16u

#define EAPOL_WPA_KEK2_SHA256_LEN	16u
#define EAPOL_WPA_KEK2_SHA384_LEN	32u
#define EAPOL_WPA_KCK2_SHA256_LEN	16u
#define EAPOL_WPA_KCK2_SHA384_LEN	24u

#ifndef EAPOL_KEY_HDR_VER_V2
#define EAPOL_WPA_KEY_MIC_LEN		16u /* deprecated */
#define EAPOL_WPA_KEY_LEN		95u /* deprecated */
#endif

/* If a KDK is derived, KDK bits is equal to PMK bits  */
#define EAPOL_WPA_KDK_MAX_LEN	EAPOL_WPA_PMK_MAX_LEN

#define EAPOL_PTK_KEY_MAX_LEN	(EAPOL_WPA_KEY_MAX_MIC_LEN +\
				EAPOL_WPA_ENCR_KEY_MAX_LEN +\
				EAPOL_WPA_TEMP_ENCR_KEY_MAX_LEN +\
				EAPOL_WPA_KCK2_SHA384_LEN +\
				EAPOL_WPA_KEK2_SHA384_LEN +\
				EAPOL_WPA_KDK_MAX_LEN)

#ifndef EAPOL_KEY_HDR_VER_V2

/* WPA EAPOL-Key : deprecated */
typedef BWL_PRE_PACKED_STRUCT struct {
	unsigned char type;		/* Key Descriptor Type */
	unsigned short key_info;	/* Key Information (unaligned) */
	unsigned short key_len;		/* Key Length (unaligned) */
	unsigned char replay[EAPOL_WPA_KEY_REPLAY_LEN];	/* Replay Counter */
	unsigned char nonce[EAPOL_WPA_KEY_NONCE_LEN];	/* Nonce */
	unsigned char iv[EAPOL_WPA_KEY_IV_LEN];		/* Key IV */
	unsigned char rsc[EAPOL_WPA_KEY_RSC_LEN];	/* Key RSC */
	unsigned char id[EAPOL_WPA_KEY_ID_LEN];		/* WPA:Key ID, 802.11i/WPA2: Reserved */
	unsigned char mic[EAPOL_WPA_KEY_MIC_LEN];	/* Key MIC */
	unsigned short data_len;			/* Key Data Length */
	unsigned char data[EAPOL_WPA_KEY_DATA_LEN];	/* Key data */
} BWL_POST_PACKED_STRUCT eapol_wpa_key_header_t;
#else
/* WPA EAPOL-Key : new structure to consider dynamic MIC length */
typedef BWL_PRE_PACKED_STRUCT struct {
	unsigned char type;             /* Key Descriptor Type */
	unsigned short key_info;        /* Key Information (unaligned) */
	unsigned short key_len;         /* Key Length (unaligned) */
	unsigned char replay[EAPOL_WPA_KEY_REPLAY_LEN]; /* Replay Counter */
	unsigned char nonce[EAPOL_WPA_KEY_NONCE_LEN];   /* Nonce */
	unsigned char iv[EAPOL_WPA_KEY_IV_LEN];         /* Key IV */
	unsigned char rsc[EAPOL_WPA_KEY_RSC_LEN];       /* Key RSC */
	unsigned char id[EAPOL_WPA_KEY_ID_LEN];         /* WPA:Key ID, 802.11i/WPA2: Reserved */
} BWL_POST_PACKED_STRUCT eapol_wpa_key_header_v2_t;

typedef eapol_wpa_key_header_v2_t eapol_wpa_key_header_t;
#endif /* EAPOL_KEY_HDR_VER_V2 */

#define EAPOL_WPA_KEY_DATA_LEN_SIZE	2u

#ifdef  EAPOL_KEY_HDR_VER_V2
#define EAPOL_WPA_KEY_HDR_SIZE(mic_len) (sizeof(eapol_wpa_key_header_v2_t) \
	+ mic_len + EAPOL_WPA_KEY_DATA_LEN_SIZE)

/* WPA EAPOL-Key header macros to reach out mic/data_len/data field */
#define EAPOL_WPA_KEY_HDR_MIC_PTR(pos) ((uint8 *)pos + sizeof(eapol_wpa_key_header_v2_t))
#define EAPOL_WPA_KEY_HDR_DATA_LEN_PTR(pos, mic_len) \
	((uint8 *)pos + sizeof(eapol_wpa_key_header_v2_t) + mic_len)
#define EAPOL_WPA_KEY_HDR_DATA_PTR(pos, mic_len) \
	((uint8 *)pos + EAPOL_WPA_KEY_HDR_SIZE(mic_len))
#else
#define EAPOL_WPA_KEY_HDR_SIZE(mic_len) EAPOL_WPA_KEY_LEN
#define EAPOL_WPA_KEY_HDR_MIC_PTR(pos) ((uint8 *)&pos->mic)
#define EAPOL_WPA_KEY_HDR_DATA_LEN_PTR(pos, mic_len) ((uint8 *)&pos->data_len)
#define EAPOL_WPA_KEY_HDR_DATA_PTR(pos, mic_len) ((uint8 *)&pos->data)
#endif /* EAPOL_KEY_HDR_VER_V2 */

/* WPA/802.11i/WPA2 KEY KEY_INFO bits */
#define WPA_KEY_DESC_OSEN	0x0
#define WPA_KEY_DESC_V0		0x0
#define WPA_KEY_DESC_V1		0x01
#define WPA_KEY_DESC_V2		0x02
#define WPA_KEY_DESC_V3		0x03
#define WPA_KEY_PAIRWISE	0x08
#define WPA_KEY_INSTALL		0x40
#define WPA_KEY_ACK		0x80
#define WPA_KEY_MIC		0x100
#define WPA_KEY_SECURE		0x200
#define WPA_KEY_ERROR		0x400
#define WPA_KEY_REQ		0x800
#define WPA_KEY_ENC_KEY_DATA	0x01000		/* Encrypted Key Data */
#define WPA_KEY_SMK_MESSAGE	0x02000		/* SMK Message */
#define WPA_KEY_DESC_VER(_ki)   ((_ki) & 0x03u)

#define WPA_KEY_DESC_V2_OR_V3 WPA_KEY_DESC_V2

/* WPA-only KEY KEY_INFO bits */
#define WPA_KEY_INDEX_0		0x00
#define WPA_KEY_INDEX_1		0x10
#define WPA_KEY_INDEX_2		0x20
#define WPA_KEY_INDEX_3		0x30
#define WPA_KEY_INDEX_MASK	0x30
#define WPA_KEY_INDEX_SHIFT	0x04

/* 802.11i/WPA2-only KEY KEY_INFO bits */
#define WPA_KEY_ENCRYPTED_DATA	0x1000

/* Key Data encapsulation */
/* this is really just a vendor-specific info element.  should define
 * this in 802.11.h
 */
typedef BWL_PRE_PACKED_STRUCT struct {
	uint8 type;
	uint8 length;
	uint8 oui[3];
	uint8 subtype;
	uint8 data[BCM_FLEX_ARRAY];
} BWL_POST_PACKED_STRUCT eapol_wpa2_encap_data_t;

#define EAPOL_WPA2_ENCAP_DATA_HDR_LEN		6u

#define WPA2_KEY_DATA_SUBTYPE_GTK		1
#define WPA2_KEY_DATA_SUBTYPE_STAKEY		2
#define WPA2_KEY_DATA_SUBTYPE_MAC		3
#define WPA2_KEY_DATA_SUBTYPE_PMKID		4
#define WPA2_KEY_DATA_SUBTYPE_IGTK		9
#define WPA2_KEY_DATA_SUBTYPE_OCI		13
#define WPA2_KEY_DATA_SUBTYPE_BIGTK		14
#define WPA2_KEY_DATA_SUBTYPE_MLO_GTK		16
#define WPA2_KEY_DATA_SUBTYPE_MLO_IGTK		17
#define WPA2_KEY_DATA_SUBTYPE_MLO_BIGTK		18
#define WPA2_KEY_DATA_SUBTYPE_MLO_LINK_KDE	19

#define WPA2_GTK_INDEX_MASK			0x03
#define WPA2_GTK_INDEX_SHIFT			0x00
#define WPA2_GTK_TRANSMIT			0x04
#define WPA2_MLO_GTK_LINK_ID_MASK		0xF0u
#define WPA2_MLO_GTK_LINK_ID_SHIFT		0x4u
#define EAPOL_WPA2_KEY_GTK_ENCAP_HDR_LEN	2u

/* GTK encapsulation */
typedef BWL_PRE_PACKED_STRUCT struct {
	uint8	flags;
	uint8	reserved;
	uint8	gtk[EAPOL_WPA_MAX_KEY_SIZE];
} BWL_POST_PACKED_STRUCT eapol_wpa2_key_gtk_encap_t;

#define EAPOL_WPA2_KEY_MLO_GTK_ENCAP_HDR_LEN	7u
/* MLO GTK encapsulation */
typedef BWL_PRE_PACKED_STRUCT struct {
	uint8	flags;			/* KeyID [0-1], Tx [2], rsvd [3], link_id [4-7] */
	uint8	PN[6];			/* Packet number */
	uint8	gtk[EAPOL_WPA_MAX_KEY_SIZE];
} BWL_POST_PACKED_STRUCT eapol_wpa2_key_mlo_gtk_encap_t;

#define EAPOL_WPA2_KEY_IGTK_ENCAP_HDR_LEN	8u

/* IGTK encapsulation */
#define EAPOL_RSN_IPN_SIZE	6u
typedef BWL_PRE_PACKED_STRUCT struct {
	uint16	key_id;
	uint8	ipn[EAPOL_RSN_IPN_SIZE];
	uint8	key[EAPOL_WPA_MAX_KEY_SIZE];
} BWL_POST_PACKED_STRUCT eapol_wpa2_key_igtk_encap_t;

#define EAPOL_WPA2_KEY_MLO_IGTK_ENCAP_HDR_LEN	9u

/* MLO IGTK encapsulation */
typedef BWL_PRE_PACKED_STRUCT struct {
	uint16	key_id;
	uint8	ipn[EAPOL_RSN_IPN_SIZE];
	uint8	link_id;		/* rsvd [0-3], link_id [4-7] */
	uint8	key[EAPOL_WPA_MAX_KEY_SIZE];
} BWL_POST_PACKED_STRUCT eapol_wpa2_key_mlo_igtk_encap_t;

/* BIGTK encapsulation */
#define EAPOL_RSN_BIPN_SIZE	6u
#define EAPOL_WPA2_KEY_BIGTK_ENCAP_HDR_LEN	8u

typedef BWL_PRE_PACKED_STRUCT struct {
	uint16	key_id;
	uint8	bipn[EAPOL_RSN_BIPN_SIZE];
	uint8	key[EAPOL_WPA_MAX_KEY_SIZE];
} BWL_POST_PACKED_STRUCT eapol_wpa2_key_bigtk_encap_t;

/* MLO BIGTK encapsulation */
#define EAPOL_RSN_MLO_BIPN_SIZE	6u
#define EAPOL_WPA2_KEY_MLO_BIGTK_ENCAP_HDR_LEN	9u

typedef BWL_PRE_PACKED_STRUCT struct {
	uint16	key_id;
	uint8	bipn[EAPOL_RSN_MLO_BIPN_SIZE];
	uint8	link_id;		/* rsvd [0-3], link_id [4-7] */
	uint8	key[EAPOL_WPA_MAX_KEY_SIZE];
} BWL_POST_PACKED_STRUCT eapol_wpa2_key_mlo_bigtk_encap_t;

#define EAPOL_WPA2_LINK_INFO_LINKID_MASK	(0xFu)
#define EAPOL_WPA2_LINK_INFO_RSNE_PRESENT	(0x1u << 4u)
#define EAPOL_WPA2_LINK_INFO_RSNXE_PRESENT	(0x1u << 5u)
#define EAPOL_WPA2_LINK_KDE_ENCAP_HDR_LEN	7u
/* Minimum length of WPA2 GTK encapsulation in EAPOL */
#define EAPOL_WPA2_LINK_KDE_ENCAP_MIN_LEN  (EAPOL_WPA2_ENCAP_DATA_HDR_LEN - \
	TLV_HDR_LEN + EAPOL_WPA2_LINK_KDE_ENCAP_HDR_LEN)

/* MLO KDE encapsulation */
typedef BWL_PRE_PACKED_STRUCT struct {
	uint8	link_info;		/* link_id [0-3], Rxneinfo [4], rsvd [5-7] */
	uint8	mac[ETHER_ADDR_LEN];
	uint8	data[BCM_FLEX_ARRAY];
} BWL_POST_PACKED_STRUCT eapol_wpa2_key_mlo_link_encap_t;

/* STAKey encapsulation */
typedef BWL_PRE_PACKED_STRUCT struct {
	uint8	reserved[2];
	uint8	mac[ETHER_ADDR_LEN];
	uint8	stakey[EAPOL_WPA_MAX_KEY_SIZE];
} BWL_POST_PACKED_STRUCT eapol_wpa2_key_stakey_encap_t;

#define WPA2_KEY_DATA_PAD	0xdd

/* This marks the end of a packed structure section. */
#include <packed_section_end.h>

#endif /* _eapol_h_ */

/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2018 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Format Header file for Exynos DPU driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __EXYNOS_FORMAT_H__
#define __EXYNOS_FORMAT_H__

#include <linux/types.h>
#include <linux/kernel.h>

#include <drm/drm_connector.h>

#define DPU_UNDEF_BITS_DEPTH		0xabcd
#define has_all_bits(bits, mask)	(((bits) & (mask)) == (bits))

enum dpu_colorspace {
	DPU_COLORSPACE_RGB,
	DPU_COLORSPACE_YUV420,
	DPU_COLORSPACE_YUV422,
};

struct dpu_fmt {
	const char *name;
	u32 fmt;		   /* user-interfaced color format */
	u32 dma_fmt;		   /* applied color format to DPU_DMA(In) */
	u32 dpp_fmt;		   /* applied color format to DPP(Out) */
	u8 bpp;			   /* bits per pixel */
	u8 padding;		   /* padding bits per pixel */
	u8 bpc;			   /* bits per each color component */
	u8 num_planes;		   /* plane(s) count of color format */
	u8 len_alpha;		   /* length of alpha bits */
	enum dpu_colorspace cs;
};

/* format */
#define IS_10BPC(f)		((f) && (f)->bpc == 10)
#define IS_YUV420(f)		((f) && (f)->cs == DPU_COLORSPACE_YUV420)
#define IS_YUV422(f)		((f) && (f)->cs == DPU_COLORSPACE_YUV422)
#define IS_YUV(f)		(IS_YUV420(f) || IS_YUV422(f))
#define IS_YUV10(f)		(IS_YUV(f) && IS_10BPC(f))
#define IS_RGB(f)		((f) && (f)->cs == DPU_COLORSPACE_RGB)
#define IS_RGB32(f)		(IS_RGB(f) && (((f)->bpp + (f)->padding) == 32))
#define IS_OPAQUE(f)		((f) && (f)->len_alpha == 0)

#ifdef CONFIG_SOC_ZUMA
#define SBWC_PAYLOAD_32B_STRIDE_ALIGN   32
#define SBWC_10B_STRIDE_32B(w)	(__ALIGN_UP((10 / 2) * \
								SBWC_BLOCK_WIDTH,  \
								SBWC_PAYLOAD_32B_STRIDE_ALIGN) *   \
								SBWC_H_BLOCKS(w))

#define SBWC_10B_Y_SIZE_32B(w, h)	((SBWC_10B_STRIDE_32B(w) *          \
									SBWC_Y_VSTRIDE_BLOCKS(h, 16)) + 64)
#define SBWC_10B_CBCR_SIZE_32B(w, h)	((SBWC_10B_STRIDE_32B(w) *          \
										SBWC_CBCR_VSTRIDE_BLOCKS(h, 16)) + 64)
#define SBWC_10B_Y_HEADER_SIZE_32B(w, h)	((SBWC_HEADER_STRIDE(w) *       \
											SBWC_Y_VSTRIDE_BLOCKS(h, 16)) + 256)
#define SBWC_10B_CBCR_HEADER_SIZE_32B(w, h)	((SBWC_HEADER_STRIDE(w) *       \
											SBWC_CBCR_VSTRIDE_BLOCKS(h, 16)) + 128)
#endif

#define Y_SIZE_8P2(w, h)	(NV12N_10B_Y_8B_SIZE(w, h) +		\
					NV12N_10B_Y_2B_SIZE(w, h))
#define UV_SIZE_8P2(w, h)	(NV12N_10B_CBCR_8B_SIZE(w, h) +		\
					NV12N_10B_CBCR_2B_SIZE(w, h))
#define Y_SIZE_SBWC_8B(w, h)	(SBWC_8B_Y_SIZE(w, h) +			\
					SBWC_8B_Y_HEADER_SIZE(w, h))
#define UV_SIZE_SBWC_8B(w, h)	(SBWC_8B_CBCR_SIZE(w, h) +		\
					SBWC_8B_CBCR_HEADER_SIZE(w, h))
#ifdef CONFIG_SOC_ZUMA
#define Y_SIZE_SBWC_10B(w, h)	(SBWC_10B_Y_SIZE_32B(w, h) +		\
					SBWC_10B_Y_HEADER_SIZE_32B(w, h))
#define UV_SIZE_SBWC_10B(w, h)	(SBWC_10B_CBCR_SIZE_32B(w, h) +		\
					SBWC_10B_CBCR_HEADER_SIZE_32B(w, h))
#define Y_PL_SIZE_SBWC(w, h, bpc)	((bpc) ? SBWC_10B_Y_SIZE_32B(w, h) :\
						SBWC_8B_Y_SIZE(w, h))
#define UV_PL_SIZE_SBWC(w, h, bpc)	((bpc) ? SBWC_10B_CBCR_SIZE_32B(w, h) :\
						SBWC_8B_CBCR_SIZE(w, h))
#else
#define Y_SIZE_SBWC_10B(w, h)	(SBWC_10B_Y_SIZE(w, h) +		\
					SBWC_10B_Y_HEADER_SIZE(w, h))
#define UV_SIZE_SBWC_10B(w, h)	(SBWC_10B_CBCR_SIZE(w, h) +		\
					SBWC_10B_CBCR_HEADER_SIZE(w, h))
#define Y_PL_SIZE_SBWC(w, h, bpc)	((bpc) ? SBWC_10B_Y_SIZE(w, h) :\
						SBWC_8B_Y_SIZE(w, h))
#define UV_PL_SIZE_SBWC(w, h, bpc)	((bpc) ? SBWC_10B_CBCR_SIZE(w, h) :\
						SBWC_8B_CBCR_SIZE(w, h))
#endif
#define Y_SIZE_SBWC(w, h, bpc)	((bpc) ? Y_SIZE_SBWC_10B(w, h) :	\
					Y_SIZE_SBWC_8B(w, h))
#define UV_SIZE_SBWC(w, h, bpc)	((bpc) ? UV_SIZE_SBWC_10B(w, h) :	\
					UV_SIZE_SBWC_8B(w, h))

#define HD_STRIDE_SIZE_SBWC(w)		(SBWC_HEADER_STRIDE(w))
#define PL_STRIDE_SIZE_SBWC(w, bpc)	((bpc) ? SBWC_10B_STRIDE(w) :	\
						SBWC_8B_STRIDE(w))

const struct dpu_fmt *dpu_find_fmt_info(u32 fmt);

static inline const char *dpu_get_fmt_name(const struct dpu_fmt *fmt)
{
	return (fmt && fmt->name) ? fmt->name : "Unknown";
}

#define HDR_DOLBY_VISION BIT(1)
#define HDR_HDR10 BIT(2)
#define HDR_HLG BIT(3)

struct drm_property *exynos_create_hdr_formats_drm_property(struct drm_device *dev, int prop_flags);

#endif /* __EXYNOS_FORMAT_H__ */

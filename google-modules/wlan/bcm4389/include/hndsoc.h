/*
 * Broadcom HND chip & on-chip-interconnect-related definitions.
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

#ifndef	_HNDSOC_H
#define	_HNDSOC_H

/* Include the soci specific files */
#include <sbconfig.h>
#include <aidmp.h>

/*
 * SOC Interconnect Address Map.
 * All regions may not exist on all chips.
 */
#define SI_SDRAM_BASE		0x00000000	/* Physical SDRAM */
#define SI_PCI_MEM		0x08000000	/* Host Mode sb2pcitranslation0 (64 MB) */
#define SI_PCI_MEM_SZ		(64 * 1024 * 1024)
#define SI_PCI_CFG		0x0c000000	/* Host Mode sb2pcitranslation1 (64 MB) */
#define	SI_SDRAM_SWAPPED	0x10000000	/* Byteswapped Physical SDRAM */
#define SI_SDRAM_R2		0x80000000	/* Region 2 for sdram (512 MB) */

#ifndef SI_ENUM_BASE_DEFAULT
#define SI_ENUM_BASE_DEFAULT		0x18000000	/* Enumeration space base */
#endif

#ifndef SI_WRAP_BASE_DEFAULT
#define SI_WRAP_BASE_DEFAULT		0x18100000	/* Wrapper space base */
#endif

#define WL_BRIDGE1_S	(0x18132000)
#define WL_BRIDGE2_S	(0x18133000)

/** new(er) chips started locating their chipc core at a different BP address than 0x1800_0000 */
#ifdef DONGLEBUILD
// firmware is always compiled for a particular chip
#define SI_ENUM_BASE(sih)	SI_ENUM_BASE_DEFAULT
#define SI_WRAP_BASE(sih)	SI_WRAP_BASE_DEFAULT
#else
// NIC and DHD driver binaries should support both old(er) and new(er) chips at the same time
#define SI_ENUM_BASE(sih)	((sih)->enum_base)
#define SI_WRAP_BASE(sih)	(SI_ENUM_BASE(sih) + 0x00100000)
#endif /* DONGLEBUILD */

#define SI_CORE_SIZE		0x1000		/* each core gets 4Kbytes for registers */

#define SI_NIC400_GPV_BASE	0x18200000	/* NIC-400 Global Programmers View (GPV) */
#define SI_GPV_WR_CAP_ADDR	0x4008	/* WR-CAP offset */
#define SI_GPV_RD_CAP_EN	0x1	/* issue read */
#define SI_GPV_WR_CAP_EN	0x2	/* issue write */

#define SI_GPV_SL4_BM_ADDR  0x44024 /* NIC-400 Slave interface 4 Bypass merge */
#define SI_GPV_SL6_BM_ADDR  0x46024 /* NIC-400 Slave interface 6 Bypass merge */
#define SI_GPV_SL8_BM_ADDR  0x4a024 /* NIC-400 Slave interface 8 Bypass merge */
#define SI_GPV_SL9_BM_ADDR  0x4b024 /* NIC-400 Slave interface 9 Bypass merge */

/* AXI Slave Interface Block (ASIB) offsets */
#define ASIB_FN_MOD2		0x24

#ifndef SI_MAXCORES
#ifdef _RTE_
#define	SI_MAXCORES		16		/* Max cores (this is arbitrary, for software
					 * convenience and could be changed if we
					 * make any larger chips
					 */
#else
#define	SI_MAXCORES		32		/* NorthStar has more cores */
#endif /* _RTE_ */
#endif /* SI_MAXCORES */

#define	SI_MAXBR		4		/* Max bridges (this is arbitrary, for software
					 * convenience and could be changed if we
					 * make any larger chips
					 */

#define	SI_FASTRAM		0x19000000	/* On-chip RAM on chips that also have DDR */
#define	SI_FASTRAM_SWAPPED	0x19800000

#define	SI_FLASH2		0x1c000000	/* Flash Region 2 (region 1 shadowed here) */
#define	SI_FLASH2_SZ		0x02000000	/* Size of Flash Region 2 */
#define	SI_ARMCM3_ROM		0x1e000000	/* ARM Cortex-M3 ROM */
#define	SI_FLASH1		0x1fc00000	/* MIPS Flash Region 1 */
#define	SI_FLASH1_SZ		0x00400000	/* MIPS Size of Flash Region 1 */
#define	SI_FLASH_WINDOW		0x01000000	/* Flash XIP Window */

#define SI_NS_NANDFLASH		0x1c000000	/* NorthStar NAND flash base */
#define SI_NS_NORFLASH		0x1e000000	/* NorthStar NOR flash base */
#define SI_NS_ROM		0xfffd0000	/* NorthStar ROM */
#define	SI_NS_FLASH_WINDOW	0x02000000	/* Flash XIP Window */

#define	SI_ARM7S_ROM		0x20000000	/* ARM7TDMI-S ROM */
#define	SI_ARMCR4_ROM		0x000f0000	/* ARM Cortex-R4 ROM */
#define	SI_ARMCM3_SRAM2		0x60000000	/* ARM Cortex-M3 SRAM Region 2 */
#define	SI_ARM7S_SRAM2		0x80000000	/* ARM7TDMI-S SRAM Region 2 */
#define	SI_ARMCA7_ROM		0x00000000	/* ARM Cortex-A7 ROM */
#ifndef SI_ARMCA7_RAM
#define	SI_ARMCA7_RAM		0x00200000	/* ARM Cortex-A7 RAM */
#endif
#define	SI_ARM_FLASH1		0xffff0000	/* ARM Flash Region 1 */
#define	SI_ARM_FLASH1_SZ	0x00010000	/* ARM Size of Flash Region 1 */

#define SI_SFLASH		0x14000000
#define SI_PCI_DMA		0x40000000	/* Client Mode sb2pcitranslation2 (1 GB) */
#define SI_PCI_DMA2		0x80000000	/* Client Mode sb2pcitranslation2 (1 GB) */
#define SI_PCI_DMA_SZ		0x40000000	/* Client Mode sb2pcitranslation2 size in bytes */
#define SI_PCIE_DMA_L32		0x00000000	/* PCIE Client Mode sb2pcitranslation2
						 * (2 ZettaBytes), low 32 bits
						 */
#define SI_PCIE_DMA_H32		0x80000000	/* PCIE Client Mode sb2pcitranslation2
						 * (2 ZettaBytes), high 32 bits
						 */

/* APB bridge code */
#define	APB_BRIDGE_ID		0x135		/* APB Bridge 0, 1, etc. */

/* ADB bridge code */
#define ADB_BRIDGE_ID		0x031

/* AXI-AHB bridge code */
#define	AXI2AHB_BRIDGE_ID		0x240		/* AXI_AHB Bridge */

/* core codes */
#define	NODEV_CORE_ID		0x700		/* Invalid coreid */
#define	CC_CORE_ID		0x800		/* chipcommon core */
#define	ILINE20_CORE_ID		0x801		/* iline20 core */
#define	SRAM_CORE_ID		0x802		/* sram core */
#define	SDRAM_CORE_ID		0x803		/* sdram core */
#define	PCI_CORE_ID		0x804		/* pci core */
#define	MIPS_CORE_ID		0x805		/* mips core */
#define	ENET_CORE_ID		0x806		/* enet mac core */
#define	CODEC_CORE_ID		0x807		/* v90 codec core */
#define	USB_CORE_ID		0x808		/* usb 1.1 host/device core */
#define	ADSL_CORE_ID		0x809		/* ADSL core */
#define	ILINE100_CORE_ID	0x80a		/* iline100 core */
#define	IPSEC_CORE_ID		0x80b		/* ipsec core */
#define	UTOPIA_CORE_ID		0x80c		/* utopia core */
#define	PCMCIA_CORE_ID		0x80d		/* pcmcia core */
#define	SOCRAM_CORE_ID		0x80e		/* internal memory core */
#define	MEMC_CORE_ID		0x80f		/* memc sdram core */
#define	OFDM_CORE_ID		0x810		/* OFDM phy core */
#define	EXTIF_CORE_ID		0x811		/* external interface core */
#define	D11_CORE_ID		0x812		/* 802.11 MAC core */
#define	APHY_CORE_ID		0x813		/* 802.11a phy core */
#define	BPHY_CORE_ID		0x814		/* 802.11b phy core */
#define	GPHY_CORE_ID		0x815		/* 802.11g phy core */
#define	MIPS33_CORE_ID		0x816		/* mips3302 core */
#define	USB11H_CORE_ID		0x817		/* usb 1.1 host core */
#define	USB11D_CORE_ID		0x818		/* usb 1.1 device core */
#define	USB20H_CORE_ID		0x819		/* usb 2.0 host core */
#define	USB20D_CORE_ID		0x81a		/* usb 2.0 device core */
#define	SDIOH_CORE_ID		0x81b		/* sdio host core */
#define	ROBO_CORE_ID		0x81c		/* roboswitch core */
#define	ATA100_CORE_ID		0x81d		/* parallel ATA core */
#define	SATAXOR_CORE_ID		0x81e		/* serial ATA & XOR DMA core */
#define	GIGETH_CORE_ID		0x81f		/* gigabit ethernet core */
#define	PCIE_CORE_ID		0x820		/* pci express core */
#define	NPHY_CORE_ID		0x821		/* 802.11n 2x2 phy core */
#define	SRAMC_CORE_ID		0x822		/* SRAM controller core */
#define	MINIMAC_CORE_ID		0x823		/* MINI MAC/phy core */
#define	ARM11_CORE_ID		0x824		/* ARM 1176 core */
#define	ARM7S_CORE_ID		0x825		/* ARM7tdmi-s core */
#define	LPPHY_CORE_ID		0x826		/* 802.11a/b/g phy core */
#define	PMU_CORE_ID		0x827		/* PMU core */
#define	SSNPHY_CORE_ID		0x828		/* 802.11n single-stream phy core */
#define	SDIOD_CORE_ID		0x829		/* SDIO device core */
#define	ARMCM3_CORE_ID		0x82a		/* ARM Cortex M3 core */
#define	HTPHY_CORE_ID		0x82b		/* 802.11n 4x4 phy core */
#define	MIPS74K_CORE_ID		0x82c		/* mips 74k core */
#define	GMAC_CORE_ID		0x82d		/* Gigabit MAC core */
#define	DMEMC_CORE_ID		0x82e		/* DDR1/2 memory controller core */
#define	PCIERC_CORE_ID		0x82f		/* PCIE Root Complex core */
#define	OCP_CORE_ID		0x830		/* OCP2OCP bridge core */
#define	SC_CORE_ID		0x831		/* shared common core */
#define	AHB_CORE_ID		0x832		/* OCP2AHB bridge core */
#define	SPIH_CORE_ID		0x833		/* SPI host core */
#define	I2S_CORE_ID		0x834		/* I2S core */
#define	DMEMS_CORE_ID		0x835		/* SDR/DDR1 memory controller core */
#define	DEF_SHIM_COMP		0x837		/* SHIM component in ubus/6362 */

#define ACPHY_CORE_ID		0x83b		/* Dot11 ACPHY */
#define PCIE2_CORE_ID		0x83c		/* pci express Gen2 core */
#define USB30D_CORE_ID		0x83d		/* usb 3.0 device core */
#define ARMCR4_CORE_ID		0x83e		/* ARM CR4 CPU */
#define GCI_CORE_ID		0x840		/* GCI Core */
#define SR_CORE_ID		0x841		/* SR_CORE ID */
#define M2MDMA_CORE_ID          0x844           /* memory to memory dma */
#define CMEM_CORE_ID		0x846		/* CNDS DDR2/3 memory controller */
#define ARMCA7_CORE_ID		0x847		/* ARM CA7 CPU */
#define SYSMEM_CORE_ID		0x849		/* System memory core */
#define HUB_CORE_ID		0x84b           /* Hub core ID */
#define HWA_CORE_ID		0x851		/* HWA Core ID */
#define SPMI_SLAVE_CORE_ID	0x855		/* SPMI Slave Core ID */
#define BT_CORE_ID		0x857		/* Bluetooth Core ID */
#define HND_OOBR_CORE_ID	0x85c		/* Hnd oob router core ID */
#define SOE_CORE_ID		0x85d		/* SOE core */
#define APB_BRIDGE_CORE_ID	0x135		/* APB bridge core ID */
#define AXI_CORE_ID		0x301		/* AXI/GPV core ID */
#define EROM_CORE_ID		0x366		/* EROM core ID */
#define OOB_ROUTER_CORE_ID	0x367		/* OOB router core ID */
#define CCI400_CORE_ID		0x420		/* CCI-400 (Cache Coherent Interconnect) core ID */
#define DEF_AI_COMP		0xfff		/* Default component, in ai chips it maps all
						 * unused address ranges
						 */

#define NS_PCIEG2_CORE_ID	0x501		/* PCIE Gen 2 core */
#define NS_DMA_CORE_ID		0x502		/* DMA core */
#define NS_SDIO3_CORE_ID	0x503		/* SDIO3 core */
#define NS_USB20_CORE_ID	0x504		/* USB2.0 core */
#define NS_USB30_CORE_ID	0x505		/* USB3.0 core */
#define NS_A9JTAG_CORE_ID	0x506		/* ARM Cortex A9 JTAG core */
#define NS_DDR23_CORE_ID	0x507		/* Denali DDR2/DDR3 memory controller */
#define NS_ROM_CORE_ID		0x508		/* ROM core */
#define NS_NAND_CORE_ID		0x509		/* NAND flash controller core */
#define NS_QSPI_CORE_ID		0x50a		/* SPI flash controller core */
#define NS_CCB_CORE_ID		0x50b		/* ChipcommonB core */
#define NS_SOCRAM_CORE_ID	0x50e		/* internal memory core */
#define	ARMCA9_CORE_ID		0x510		/* ARM Cortex A9 core (ihost) */
#define	NS_IHOST_CORE_ID	ARMCA9_CORE_ID	/* ARM Cortex A9 core (ihost) */
#define AMEMC_CORE_ID		0x52e		/* DDR1/2 memory controller core */
#define ALTA_CORE_ID		0x534		/* I2S core */
#define DDR23_PHY_CORE_ID	0x5dd

#define SI_PCI1_MEM		0x40000000	/* Host Mode sb2pcitranslation0 (64 MB) */
#define SI_PCI1_CFG		0x44000000	/* Host Mode sb2pcitranslation1 (64 MB) */
#define SI_PCIE1_DMA_H32	0xc0000000	/* PCIE Client Mode sb2pcitranslation2
						 * (2 ZettaBytes), high 32 bits
						 */
#define NS_PCIEG2_CORE_REV_B0	0x7		/* NS-B0 PCIE Gen 2 core rev */

/* There are TWO constants on all HND chips: SI_ENUM_BASE_DEFAULT above,
 * and chipcommon being the first core:
 */
#define	SI_CC_IDX		0
/* SOC Interconnect types (aka chip types) */
#define	SOCI_SB			0u
#define	SOCI_AI			1u
#define	SOCI_UBUS		2u
#define	SOCI_NAI		3u
#define SOCI_DVTBUS		4u /* BCM7XXX Digital Video Tech bus */
#define SOCI_NCI		6u /* NCI (non coherent interconnect) i.e. BOOKER */

/* Common core control flags */
#define	SICF_BIST_EN		0x8000
#define	SICF_PME_EN		0x4000
#define	SICF_CORE_BITS		0x3ffc
#define	SICF_PCEN		0x0004
#define	SICF_FGC		0x0002
#define	SICF_CLOCK_EN		0x0001

/* Common core status flags */
#define	SISF_BIST_DONE		0x8000
#define	SISF_BIST_ERROR		0x4000
#define	SISF_GATED_CLK		0x2000
#define	SISF_DMA64		0x1000
#define	SISF_CORE_BITS		0x0fff
#define	SISF_CORE_BITS_SCAN	0x0010		/* SCAN core */

/* Norstar core status flags */
#define SISF_NS_BOOTDEV_MASK	0x0003	/* ROM core */
#define SISF_NS_BOOTDEV_NOR	0x0000	/* ROM core */
#define SISF_NS_BOOTDEV_NAND	0x0001	/* ROM core */
#define SISF_NS_BOOTDEV_ROM	0x0002	/* ROM core */
#define SISF_NS_BOOTDEV_OFFLOAD	0x0003	/* ROM core */
#define SISF_NS_SKUVEC_MASK	0x000c	/* ROM core */

/* dot11 core-specific status flags */
#define	SISF_MINORREV_D11_SHIFT	16
#define	SISF_MINORREV_D11_MASK	0xF		/**< minor corerev (corerev == 61) */

/* A register that is common to all cores to
 * communicate w/PMU regarding clock control.
 */
#define SI_CLK_CTL_ST		0x1e0		/* clock control and status */
#define SI_PWR_CTL_ST		0x1e8		/* For memory clock gating */

/* clk_ctl_st register */
#define	CCS_FORCEALP		0x00000001	/* force ALP request */
#define	CCS_FORCEHT		0x00000002	/* force HT request */
#define	CCS_FORCEILP		0x00000004	/* force ILP request */
#define	CCS_ALPAREQ		0x00000008	/* ALP Avail Request */
#define	CCS_HTAREQ		0x00000010	/* HT Avail Request */
#define	CCS_FORCEHWREQOFF	0x00000020	/* Force HW Clock Request Off */
#define CCS_HQCLKREQ		0x00000040	/* HQ Clock Required */
#define CCS_USBCLKREQ		0x00000100	/* USB Clock Req */
#define CCS_SECICLKREQ		0x00000100	/* SECI Clock Req */
#define CCS_ARMFASTCLOCKREQ	0x00000100	/* ARM CR4/CA7 fast clock request */
#define CCS_SFLASH_CLKREQ	0x00000200	/* Sflash clk request */
#define CCS_AVBCLKREQ		0x00000400	/* AVB Clock enable request */
#define CCS_ERSRC_REQ_MASK	0x00000700	/* external resource requests */
#define CCS_ERSRC_REQ_SHIFT	8
#define	CCS_ALPAVAIL		0x00010000	/* ALP is available */
#define	CCS_HTAVAIL		0x00020000	/* HT is available */
#define CCS_BP_ON_APL		0x00040000	/* RO: Backplane is running on ALP clock */
#define CCS_BP_ON_HT		0x00080000	/* RO: Backplane is running on HT clock */
#define CCS_ARMFASTCLOCKSTATUS	0x01000000	/* Fast CPU clock is running */
#define CCS_ERSRC_STS_MASK	0x07000000	/* external resource status */
#define CCS_ERSRC_STS_SHIFT	24
#define CCS_SECI_AVAIL		0x01000000	/* RO: SECI is available  */

/* Not really related to SOC Interconnect, but a couple of software
 * conventions for the use the flash space:
 */

/* Minumum amount of flash we support */
#define FLASH_MIN		0x00020000	/* Minimum flash size */

/* A boot/binary may have an embedded block that describes its size  */
#define	BISZ_OFFSET		0x3e0		/* At this offset into the binary */
#define	BISZ_MAGIC		0x4249535a	/* Marked with this value: 'BISZ' */
#define	BISZ_MAGIC_IDX		0		/* Word 0: magic */
#define	BISZ_TXTST_IDX		1		/*	1: text start */
#define	BISZ_TXTEND_IDX		2		/*	2: text end */
#define	BISZ_DATAST_IDX		3		/*	3: data start */
#define	BISZ_DATAEND_IDX	4		/*	4: data end */
#define	BISZ_BSSST_IDX		5		/*	5: bss start */
#define	BISZ_BSSEND_IDX		6		/*	6: bss end */
#define BISZ_SIZE		7		/* descriptor size in 32-bit integers */

/* Boot/Kernel related defintion and functions */
#define	SOC_BOOTDEV_ROM		0x00000001
#define	SOC_BOOTDEV_PFLASH	0x00000002
#define	SOC_BOOTDEV_SFLASH	0x00000004
#define	SOC_BOOTDEV_NANDFLASH	0x00000008

#define	SOC_KNLDEV_NORFLASH	0x00000002
#define	SOC_KNLDEV_NANDFLASH	0x00000004

#if !defined(_LANGUAGE_ASSEMBLY) && !defined(__ASSEMBLY__)
int soc_boot_dev(void *sih);
int soc_knl_dev(void *sih);
#endif	/* !defined(_LANGUAGE_ASSEMBLY) && !defined(__ASSEMBLY__) */

#define PMU_BASE_OFFSET	0x00012000	/* PMU offset is changed for ccrev >= 56 */
#endif /* _HNDSOC_H */

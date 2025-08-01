/*
 * HND SiliconBackplane chipcommon support - OS independent.
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

#ifndef _hndchipc_h_
#define _hndchipc_h_

#include <typedefs.h>
#include <siutils.h>

#ifdef RTE_UART
typedef void (*si_serial_init_fn)(si_t *sih, void *regs, uint irq, uint baud_base, uint reg_shift);
#else
typedef void (*si_serial_init_fn)(void *regs, uint irq, uint baud_base, uint reg_shift);
#endif
extern void si_serial_init(si_t *sih, si_serial_init_fn add);

extern volatile void *hnd_jtagm_init(si_t *sih, uint clkd, bool exttap, uint32 *prev_jtagctrl);
extern void hnd_jtagm_disable(si_t *sih, volatile void *h, uint32 *prev_jtagctrl);
extern uint32 jtag_scan(si_t *sih, volatile void *h, uint irsz, uint32 ir0, uint32 ir1,
                        uint drsz, uint32 dr0, uint32 *dr1, bool rti);
extern uint32 jtag_read_128(si_t *sih, volatile void *h, uint irsz, uint32 ir0, uint drsz,
	uint32 dr0, uint32 *dr1, uint32 *dr2, uint32 *dr3);
extern uint32 jtag_write_128(si_t *sih, volatile void *h, uint irsz, uint32 ir0, uint drsz,
	uint32 dr0, uint32 *dr1, uint32 *dr2, uint32 *dr3);
extern int jtag_setbit_128(si_t *sih, uint32 jtagureg_addr, uint8 bit_pos, uint8 bit_val);

#endif /* _hndchipc_h_ */

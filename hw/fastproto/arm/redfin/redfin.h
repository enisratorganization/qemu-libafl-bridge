/*
 * Redfin (Google Pixel 4a 5G / 5, Qualcomm SM7250 "Saipan") re-hosting.
 *
 * Boot chain emulated from the PBL (boot ROM) on: PBL -> XBL_SEC -> XBL/SBL1
 * -> TZ (QSEE) -> XBL UEFI. Each stage has its own hook file instr_<stage>.c
 * registering an FpHook table.
 */

#pragma once

#include "hw/fastproto/arm/fp_arm.h"

/* Hook registration per boot stage (instr_*.c) */
void brom_instrument(void);
void xbl_sec_instrument(void);
void sbl1_instrument(void);
void tz_instrument(void);
void xbl_uefi_instrument(void);

/* qsee_interface.c: patches the SBL -> QSEE boot image table */
bool hook_qsee_start(CPUState *cs, vaddr pc, void *opaque);

/*
 * MT6768 (MediaTek Helio G85 class) re-hosting: ATF (BL31) + TEEI (TEE) +
 * LK, all loaded as raw images (-L dir with atf, tee, lk, ... files).
 */

#pragma once

#include "hw/fastproto/arm/fp_arm.h"

void atf_teei_instrument(void);   /* instr_atf.c */
void teei_instrument(void);       /* instr_teei.c */

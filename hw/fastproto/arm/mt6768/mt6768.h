/*
 * MT6768 (MediaTek Helio G85 class) re-hosting: ATF (BL31) + TEEI (TEE) +
 * LK, all loaded as raw images (-L dir with atf, tee, lk, ... files).
 */

#pragma once

#include "hw/fastproto/arm/fp_arm.h"

#define MT6768_CPU_TYPE_NAME "mt6768-a55"

void atf_teei_instrument(void);   /* instr_atf.c */
void teei_instrument(void);       /* instr_teei.c */

#ifndef COVERAGE_ARM_H
#define COVERAGE_ARM_H

#include "qemu/osdep.h"
#include "translate.h"
#include "tcg/coverage-tcg.h"

/* To avoid recording multiple conditional instructions based on the same flags and condition code.
 * It will actively REMOVE some TCGOps.
 */
void arm_tcg_cc_recording_check_and_remove(DisasContext *s, int current_cc,
										   TCGOp *current_cc_rec_start, TCGOp *currenct_cc_rec_end);

void arm_tcg_cc_recording_reset(DisasContext *s);

void arm_tcg_gen_rec_edge(DisasContext *s, TCGv cpu_pc, TCGv_i32 out_edge_id);

#endif
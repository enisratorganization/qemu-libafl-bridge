#ifndef COVERAGE_ARM_H
#define COVERAGE_ARM_H

#include "qemu/osdep.h"
#include "translate.h"
#include "tcg/coverage-tcg.h"

void arm_tcg_gen_rec_edge(DisasContext *s, TCGv cpu_pc, TCGv_i32 out_edge_id);

#endif
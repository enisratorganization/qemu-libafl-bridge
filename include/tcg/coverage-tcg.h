#ifndef COVERAGE_TCG_H
#define COVERAGE_TCG_H

#include "tcg/tcg.h"
#include "tcg/coverage-tcg-helper-gen.h"
#include "exec/translation-block.h"

static inline bool edge_coverage_is_enabled(TranslationBlock *tb) {
	return edge_coverage_record_tcg_enabled && tb->covrec_enabled;
}
static inline bool comp_coverage_is_enabled(TranslationBlock *tb) {
	return comp_coverage_record_tcg_enabled && tb->covrec_enabled;
}

/**
 * Do a fast hash like CRC32. Used for coverage recording and fuzzing.
 */
void tcg_gen_fast_hash_i32(TCGv_i32 dst, TCGv_i32 src, TCGv_i32 src2);
void tcg_gen_fast_hash_i64(TCGv_i32 dst, TCGv_i32 src, TCGv_i64 src2);

/**
 * Record an edge at the current @pc basic block and the id of the outgoing edge.
 * @out_edge_id can be for example 0|1 or the next pc (indirect jump).
 *
 * @pc and @out_edge_id are not clobbered
 *
 * The way you use this is to insert a call to this in the guest translation routines
 * for conditional branches, once the out_edge_id is known.
 *
 * This will generate coverage hashmap that is accurate.
 * Both input parameters are hashed together.
 *
 */
void tcg_gen_rec_edge_i32(TCGv_i32 pc, TCGv_i32 out_edge_id);
void tcg_gen_rec_edge_i64(TCGv_i64 pc, TCGv_i32 out_edge_id);

/**
 * ADD [mem+idx*str+ofs], val instruction. This is declared because it optimizes well for x86.
 * Corresponds to: add byte|word|dword|qword ptr [base + index*elem_sz + ofs], val
 */
void tcg_gen_add_mem_idx_i64(TCGv_i64 base, TCGv_i64 index, TCGv_i64 val, int elem_sz, int ofs);


#endif /* COVERAGE_TCG_H */

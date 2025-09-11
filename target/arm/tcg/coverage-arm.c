
#include "coverage-arm.h"

/* To avoid recording multiple conditional instructions based on the same flags and condition code.
 * It will actively REMOVE some TCGops.
 */
void arm_tcg_cc_recording_check_and_remove(DisasContext *s, int current_cc, 
	TCGOp *current_cc_rec_start, TCGOp *currenct_cc_rec_end ) {

	TCGOp *op, *tmp = NULL;
	struct tcg_arm_edge_coverage *cov = &s->cov;

	if (current_cc & cov->last_cc && 
		cov->last_cc_rec_start && cov->last_cc_rec_end ) {
		// delete ops, starting with last. The first op is exclusive
		for (op = cov->last_cc_rec_end;
			tmp != cov->last_cc_rec_start;
			op = tmp) {
			tmp = QTAILQ_PREV(op, link);
			tcg_op_remove(tcg_ctx, op);
		}
	};

	cov->last_cc_rec_start = current_cc_rec_start;
	cov->last_cc_rec_end = currenct_cc_rec_end;
	cov->last_cc = current_cc;
}

void arm_tcg_cc_recording_reset(DisasContext *s) {
	struct tcg_arm_edge_coverage *cov = &s->cov;

	cov->last_cc_rec_start = NULL;
	cov->last_cc_rec_end = NULL;
	cov->last_cc = 0;
}

void arm_tcg_gen_rec_edge(DisasContext *s, TCGv cpu_pc, TCGv_i32 out_edge_id)
{

	/* now record the edge */
	if(s->aarch64){
		TCGv_i64 pc_here = tcg_temp_new_i64();
		/* update pc to current program address to make coverage deterministic */

		assert(s->pc_save != -1);

		if (tb_cflags(s->base.tb) & CF_PCREL) {
			tcg_gen_addi_i64(pc_here, cpu_pc, (s->pc_curr - s->pc_save));
		} else {
			tcg_gen_movi_i64(pc_here, s->pc_curr);
		}

		tcg_gen_rec_edge_i64(pc_here, out_edge_id);
	} else {
		//@TODO 32 and 64 bit temporaries assumed interchangeable (as in x86_64)!
		TCGv_i64 pc_here = tcg_temp_new_i64();
	
		assert(s->pc_save != -1);

		if (tb_cflags(s->base.tb) & CF_PCREL) {
			tcg_gen_addi_i32((TCGv_i32)pc_here, cpu_pc, (s->pc_curr - s->pc_save));
		} else {
			tcg_gen_movi_i32((TCGv_i32)pc_here, s->pc_curr);
		}

		tcg_gen_rec_edge_i64(pc_here, out_edge_id);
	}
}
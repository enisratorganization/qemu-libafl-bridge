
#include "coverage-arm.h"

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
		TCGv_i32 pc_here = tcg_temp_new_i32();
		/* @TODO implement */
	}
}
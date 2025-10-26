#ifndef  COVERAGE_ARM_DISASCONTEXT_EXT_H
#define COVERAGE_ARM_DISASCONTEXT_EXT_H

struct tcg_arm_edge_coverage {
	    
    /* We try not to record "mov pc, lr", "ldm lr, pc, ... [sp]"
     * as these are very likely function return insns.
     * Semantically, store_reg_probably_ret is true:
	 * - if the src register is LR for BX or MOV
     * - in case of ldr, ldm
     * It will be false:
     * - For TBB
     * - For BX (other than BX LR)
     * 
     * Reset to false in ops->insn_start()
     */
    bool store_reg_probably_ret;

    /* To avoid double recording of conditional insns with the same flags.
     * last_cc is a bitmask with the last condition code checked.
     * A CMP insn will reset this to 0.
     * Note: A new Translation Block will also start with last_cc = 0 .
     */
	unsigned int last_cc;
	/* Also record the last recording TCG ops produced. 
	 * These can be REMOVED (using tcg_op_remove) in a succeeding conditional insn 
	 * iff the condition codes match.
	 */
	TCGOp *last_cc_rec_start;
	TCGOp *last_cc_rec_end;
};

#endif
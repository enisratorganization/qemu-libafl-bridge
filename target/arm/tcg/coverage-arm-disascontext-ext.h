#ifndef  COVERAGE_ARM_DISASCONTEXT_EXT_H
#define COVERAGE_ARM_DISASCONTEXT_EXT_H

struct tcg_arm_edge_coverage {
	    
    /* We try not to record "mov pc, lr", "ldm lr, pc, ... [sp]"
     * "mov pc, #0x..." as these are very likely function 
     * return insns (or immediate branches)
     * Semantically, this field is true:
	 * - if the src register is LR for BX or MOV
     * - MOV to PC with only imm
     * - in case of ldr, ldm
     * It will be false:
     * - For TBB
     * - For BX (other than BX LR)
     * 
     * Reset to false in ops->insn_start()
     */
    bool ignore_this_store_reg_for_coverage;

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
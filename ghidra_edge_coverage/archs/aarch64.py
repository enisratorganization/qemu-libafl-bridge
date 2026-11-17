"""
AArch64 specifics.

QEMU (target/arm/tcg/translate-a64.c) records a decision for:
  - b.cond / bc.cond                 (arm_gen_test_cc -> arm_test_cc)
  - csel/csinc/csinv/csneg/ccmp/ccmn/fcsel/fccmp/fccmpe   (a64_test_cc);
    cset/csetm/cinc/cinv/cneg are aliases of csinc/csinv/csneg
  - cbz/cbnz, tbz/tbnz               (arm_tcg_gen_rec_edge, edge_id is the
    tested value -> register-dependent, replayed with Rt = 0 and the bit set)
  - br/blr                           (edge_id = register value: ignored,
    cannot be computed statically)

Ghidra prints the *alias* condition for cset/cinc/..., which is the inverse
of the encoded one -> always take the cond field from the encoding.
"""
import logging

from . import ArchBase, register
from .arm_shared import (FLAG_CASES, branch_target, cond_label, cond_runs,
                         insn_word)

log = logging.getLogger("edgecov")

# Instructions that consume a condition without branching.  QEMU calls
# arm_test_cc() for them as well, so they are recorded.  cond is bits 15:12.
CC_CONSUMERS = {"csel", "csinc", "csinv", "csneg", "cset", "csetm", "cinc", "cinv", "cneg",
                "ccmp", "ccmn", "fcsel", "fccmp", "fccmpe"}


@register
class AArch64(ArchBase):
    NAME = "aarch64"
    TOOL = "qemu-edge-coverage-aarch64"

    @staticmethod
    def detect(program):
        return str(program.getLanguageID()).split(":")[0] == "AARCH64"

    @staticmethod
    def plan_runs(fapi, ins):
        m = ins.getMnemonicString().lower()
        pc = ins.getAddress().getOffset()
        word = insn_word(ins)
        ft = ins.getFlowType()

        if m in CC_CONSUMERS:
            cond = (word >> 12) & 0xF
            if cond >= 14:  # "al"/"nv": arm_test_cc() returns before recording
                return []
            # both outcomes continue at pc+4 (like a fall-through edge)
            return [(pc + 4, cond_label(cond, f), f, (), {})
                    for f in FLAG_CASES[cond >> 1]]

        if not ft.isConditional() or ft.isComputed():
            return []  # fall-through, b, bl, br, blr, ret, ...: not recorded / not computable
        taken, fall = branch_target(fapi, ins), ins.getFallThrough()
        if taken is None or fall is None:
            log.warning("%s %s: conditional without static target/fallthrough, skipped", ins.getAddress(), ins)
            return []
        fall = fall.getOffset()

        if m in ("cbz", "cbnz"):  # QEMU's edge id is (Rt != 0) -> try Rt = 0 and 1
            rt = word & 0x1F
            return [(taken if (val == 0) == (m == "cbz") else fall,
                     "%s x%d=%d" % (m, rt, val), 0, ((rt, val),), {})
                    for val in (0, 1)]
        if m in ("tbz", "tbnz"):  # QEMU's edge id is the tested bit
            rt, bit = word & 0x1F, (((word >> 31) & 1) << 5) | ((word >> 19) & 0x1F)
            return [(taken if (val == 0) == (m == "tbz") else fall,
                     "%s x%d=%#x" % (m, rt, val), 0, ((rt, val),), {})
                    for val in (0, 1 << bit)]
        if m.startswith("b.") or m.startswith("bc."):
            cond = word & 0xF
            if cond >= 14:
                return []
            return cond_runs(cond, taken, fall)

        log.warning("%s %s: unhandled conditional instruction, skipped", ins.getAddress(), ins)
        return []

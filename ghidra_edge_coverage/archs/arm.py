"""
AArch32 (arm) specifics.

QEMU (target/arm/tcg/translate.c) records a decision via arm_test_cc() for:
  - every A32 instruction with cond < 0xe ("conditional everything":
    data processing, loads/stores, svc, ... and b.cond / conditional bl(x)),
    through arm_skip_unless() in disas_arm_insn()
  - thumb b.cond (T16/T32 encodings, outside IT blocks), trans_B_cond_thumb
  - every instruction inside a Thumb IT block (cond from the IT state,
    arm_skip_unless() in thumb_tr_translate_insn())
NOT recorded for arm32 (unlike AArch64): thumb cbz/cbnz (plain brcond in
trans_CBZ) and tbb/tbh.  Indirect control flow (bx, blx reg, ldm {..., pc},
mov pc, ...) is recorded with the register value as edge id -> ignored.

QEMU dedups *consecutive* recordings of the same condition pair within one
TB (arm_tcg_cc_recording_check_and_remove), but the state resets per TB, so
an instruction can always be the first of a TB -> every one of them is a
valid candidate; the single-instruction replay in the tool never dedups.

Thumb/IT replay: the tool gets the T bit and the CPSR IT bits (reconstructed
condexec state for the k-th instruction of the block) through -f/-m, see
tool_flags() and it_gate().
"""
import logging

from . import ArchBase, register
from .arm_shared import branch_target, cond_runs, insn_word

log = logging.getLogger("edgecov")

# CPSR bits the tool must write for Thumb / IT-block state (target/arm/cpu.h)
CPSR_T = 1 << 5
CPSR_IT_0_1 = 3 << 25
CPSR_IT_2_7 = 0xFC00

# per-program state, filled in by bind()
TMODE = None        # the language's TMode context register (None: no Thumb)
CONTEXT = None      # program context, for the TMode default value


def is_thumb(ins):
    """Thumb state via the TMode processor context register (default false)."""
    if TMODE is None:
        return False
    rv = ins.getRegisterValue(TMODE)
    if rv is not None:
        return rv.getUnsignedValue().testBit(0)  # BigInteger (jpype)
    default = CONTEXT.getDefaultValue(TMODE, ins.getAddress())
    return default is not None and default.getUnsignedValue().testBit(0)


# how many instructions an IT mask gates (ARM ARM table for IT <mask>);
# every other mask value is UNPREDICTABLE and gates 4
IT_LEN = {0x8: 1, 0x4: 2, 0xC: 2, 0x2: 3, 0x6: 3, 0xA: 3, 0xE: 3}


def parse_it(ins):
    """(cond, mask) of a Thumb IT instruction, None if `ins` is not one."""
    if ins.getLength() != 2:
        return None
    w = insn_word(ins) & 0xFFFF
    cond, mask = (w >> 4) & 0xF, w & 0xF
    if (w & 0xFF00) != 0xBF00 or mask == 0 or cond >= 0xE:
        # not IT / a hint insn / "IT al" gates nothing (UNPREDICTABLE)
        return None
    return cond, mask


def it_gate(fapi, ins):
    """(cond_k, mode) if `ins` is the k-th (1..4) instruction of a Thumb IT
    block, None otherwise.  `mode` carries the CPSR IT/T state the tool must
    install; it mirrors QEMU's IT state machine (trans_IT + the per-insn
    advance in thumb_tr_translate_insn, reconstructed through CPSR the way
    init_disas_context reads it back: condexec_mask = (condexec & 0xf) << 1,
    condexec_cond = condexec >> 4)."""
    k, cur = 1, ins
    while k <= 4:
        prev = fapi.getInstructionBefore(cur.getAddress())
        if prev is None:
            return None
        ft = prev.getFallThrough()
        if ft is None or ft.getOffset() != cur.getAddress().getOffset():
            return None  # nothing falls through into here: no IT above
        it = parse_it(prev)
        if it is not None:
            cond, mask = it
            if k > IT_LEN.get(mask, 4):
                return None  # `ins` is past the end of this IT block (e.g. the next IT)
            # QEMU condexec state at the k-th instruction
            m = ((mask | ((cond & 1) << 4)) << (k - 1)) & 0x1F
            cond_k = (cond & 0xE) | ((m >> 4) & 1)
            condexec = ((cond_k & 0xE) << 4) | (m >> 1)
            return cond_k, {"T": 1, "condexec": condexec}
        cur, k = prev, k + 1
    return None


@register
class ARM(ArchBase):
    NAME = "arm"
    TOOL = "qemu-edge-coverage-arm"

    @staticmethod
    def detect(program):
        return str(program.getLanguageID()).split(":")[0] == "ARM"

    @staticmethod
    def tool_flags(nzcv, mode):
        """NZCV plus, for Thumb, the T bit and the IT[7:0] state in CPSR."""
        flags, mask = nzcv << 28, 0xF0000000
        if mode.get("T"):
            flags |= CPSR_T
            mask |= CPSR_T
        if "condexec" in mode:
            ce = mode["condexec"]
            flags |= ((ce & 3) << 25) | ((ce & 0xFC) << 8)
            mask |= CPSR_IT_0_1 | CPSR_IT_2_7
        return flags, mask

    @staticmethod
    def bind(program):
        global TMODE, CONTEXT
        TMODE = program.getLanguage().getRegister("TMode")
        CONTEXT = program.getProgramContext()
        log.info("arch arm: TMode register %s", "found" if TMODE else "absent (ARM-only language)")

    @staticmethod
    def plan_runs(fapi, ins):
        if is_thumb(ins):
            return ARM.plan_runs_thumb(fapi, ins)
        return ARM.plan_runs_a32(fapi, ins)

    @staticmethod
    def plan_runs_a32(fapi, ins):
        """A32: the cond field (bits 31:28) of *every* instruction; QEMU
        records the decision whenever it is not al/nv."""
        pc = ins.getAddress().getOffset()
        cond = insn_word(ins) >> 28
        if cond >= 0xE:
            return []
        ft = ins.getFlowType()
        if ft.isConditional() and not ft.isComputed():
            # b.cond, conditional bl(x): taken -> static target
            taken, fall = branch_target(fapi, ins), ins.getFallThrough()
            if taken is None or fall is None:
                log.warning("%s %s: conditional without static target/fallthrough, skipped",
                            ins.getAddress(), ins)
                return []
            return cond_runs(cond, taken, fall.getOffset())
        # conditional data processing / loads / svc / ...: both outcomes
        # continue at pc+4 (the recorded decision is just "did it execute")
        return cond_runs(cond, pc + 4, pc + 4)

    @staticmethod
    def plan_runs_thumb(fapi, ins):
        """Thumb: b.cond outside IT blocks, everything inside an IT block
        (except uncomputable control flow).  cbz/cbnz/tbb/tbh are NOT
        recorded by QEMU for arm32 -> no runs for them."""
        pc = ins.getAddress().getOffset()
        nxt = pc + ins.getLength()

        gate = it_gate(fapi, ins)
        if gate is not None:
            cond_k, mode = gate
            if cond_k >= 0xE:  # "al" in an IT block: no arm_test_cc() call
                return []
            ft = ins.getFlowType()
            flows = ins.getFlows()
            if ft.isConditional() or ft.isComputed():
                # cbz/b.cond-in-IT etc: whether execution actually branches
                # depends on more than the IT condition -> not computable
                log.debug("%s %s: conditional flow inside IT block, skipped", ins.getAddress(), ins)
                return []
            if ft.isJump() or ft.isCall():
                if len(flows) != 1:  # indirect (bx/blx reg, table branch)
                    return []
                # direct b/bl inside the IT block: cond holds -> taken
                return cond_runs(cond_k, flows[0].getOffset(), nxt, mode)
            return cond_runs(cond_k, nxt, nxt, mode)

        # outside IT blocks only b.cond is recorded in Thumb
        m = ins.getMnemonicString().lower()
        if m.startswith("cbz") or m.startswith("cbnz"):
            return []  # cbz/cbnz: plain brcond in QEMU's trans_CBZ, NOT recorded for arm32
        ft = ins.getFlowType()
        if not ft.isConditional() or ft.isComputed():
            return []
        taken, fall = branch_target(fapi, ins), ins.getFallThrough()
        if taken is None or fall is None:
            log.warning("%s %s: conditional without static target/fallthrough, skipped",
                        ins.getAddress(), ins)
            return []
        # b.cond T16: 1101 cond:4 imm8; T32: 1111 0. cond:4 ... in hw1
        if ins.getLength() == 2:
            cond = (insn_word(ins) >> 8) & 0xF
        else:
            cond = (insn_word(ins) >> 22) & 0xF
        if cond >= 0xE:
            return []
        return cond_runs(cond, taken, fall.getOffset())

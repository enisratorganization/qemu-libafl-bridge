"""
Helpers shared by the ARM architecture modules (aarch64.py, arm.py).

The edge_id computation itself lives in QEMU's arm_test_cc()
(target/arm/tcg/translate.c) and is *common* between AArch64 and AArch32,
so the condition-code knowledge (which NZCV combinations to replay) is
shared here rather than duplicated per arch.
"""
import logging

log = logging.getLogger("edgecov")

# --------------------------------------------------------------------------
# ARM condition codes (common between AArch64 and AArch32)
# --------------------------------------------------------------------------
N, Z, C, V = 8, 4, 2, 1  # NZCV as a nibble; the tool gets it via -f (nzcv << 28)
COND_NAMES = "eq ne cs cc mi pl vs vc hi ls ge lt gt le al nv".split()

# NZCV combinations fed to the tool, per condition pair (cond >> 1).
# With QEMU's corner-case recording (enabled in the tool), !Z is folded into
# the edge id, so each pair needs "its" flags x Z -- but only the combinations
# a real flag producer (cmp/cmn/tst/subs/adds/ands/fcmp/ccmp) can generate:
#   - N=1 & Z=1 cannot happen (a zero result is never negative)
#   - N!=V & Z=1 only via adds/cmn overflowing to exactly 0 -> ignored (rare)
#   - C=0 & Z=1 does happen (tst/ands, adds 0+0, "ccmp ..., #4, cond")
FLAG_CASES = {
    0: [0, Z],            # eq/ne: edge_id = !Z                 -> 2 edge ids
    1: [0, Z, C, C | Z],  # cs/cc: edge_id = -C ^ !Z            -> 4 edge ids
    2: [0, Z, N],         # mi/pl: edge_id = (N>>31) ^ !Z       -> 3 edge ids
    3: [0, V],            # vs/vc: edge_id = V>>31 (no Z)       -> 2 edge ids
    4: [0, Z, C, C | Z],  # hi/ls: edge_id = -C ^ !Z            -> 4 edge ids
    5: [0, Z, N],         # ge/lt: edge_id = ((N^V)>>31) ^ !Z   -> 3 edge ids
    6: [0, Z, N],         # gt/le: edge_id = ((N^V)>>31) ^ !Z   -> 3 edge ids
}


def cond_holds(cond, nzcv):
    """ARM ARM ConditionHolds()."""
    n, z, c, v = (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1
    res = [z, c, n, v, c and not z, n == v, (not z) and n == v, True][cond >> 1]
    return bool(res) != bool(cond & 1 and cond != 15)


def cond_runs(cond, taken_dst, fall_dst, mode=None):
    """Tool runs for one recorded (cond<0xe) instruction decision:
    every NZCV combination QEMU can produce for this condition pair, each
    labeled with whether the condition holds and where execution continues.
    A conditional branch passes the two destinations; everything else
    (csel, conditional data processing, IT-gated insns, ...) continues at
    the next instruction for both outcomes."""
    runs = []
    for f in FLAG_CASES[cond >> 1]:
        holds = cond_holds(cond, f)
        runs.append((taken_dst if holds else fall_dst,
                     ("taken " if holds else "fallthrough ") + cond_label(cond, f),
                     f, (), mode if mode else {}))
    return runs


def cond_label(cond, nzcv):
    return "enc.cond %s=%s NZCV=%x" % (COND_NAMES[cond], str(cond_holds(cond, nzcv)).lower(), nzcv)


# --------------------------------------------------------------------------
# Ghidra instruction helpers
# --------------------------------------------------------------------------
def insn_word(ins):
    """Instruction bytes as one little-endian integer (works for ins[len 2|4])."""
    return int.from_bytes(bytes(b & 0xFF for b in ins.getBytes()), "little")


def branch_target(fapi, ins):
    """Static target of a conditional branch: the conditional flow reference (fallback: getFlows)."""
    for ref in fapi.getReferencesFrom(ins.getAddress()):
        rt = ref.getReferenceType()
        if rt.isFlow() and rt.isConditional():
            return ref.getToAddress().getOffset()
    flows = ins.getFlows()
    return flows[0].getOffset() if len(flows) == 1 else None

"""
Filter stages: each one may only *remove* entries from ctx.hit_edges.

Every hit edge is a probabilistic hash match, so some are false positives
caused by collisions.  A stage checks a property that a really-executed
edge must have (e.g. "the next recorded instruction was executed, too")
and drops the edges that violate it -- keeping an edge whenever it cannot
tell (unknown control flow, step limit, ...).

Each stage decides on a snapshot of hit_edges taken at its start (no
cascading inside a stage).  To add a stage: write `def stage_xyz(ctx,
fapi, args)` and add it to FILTER_STAGES below (selectable via --stages,
the edge count per stage is logged).

Ideas for later stages:
  - collision/ambiguity: an offset matched by several candidate edges only
    proves that *one* of them fired -> weaken edges whose offsets are all
    "explained" by better supported edges
  - follow `ret` back to the call sites of the function (if it has only
    direct callers), follow `b` backwards in the predecessor walk
  - exempt the crash/exit location (metadata "elr"): execution stops there,
    so the edge leading to it has no successor hit
"""
import logging

from .core import fmt_edge

log = logging.getLogger("edgecov")


def next_recorded_insn(ctx, fapi, addr, max_steps):
    """Follow the straight-line path from `addr` up to the next instruction QEMU records.
    Follows fall-through and direct `b`, steps into direct `bl` (and back on its `ret`).
    Returns None = unknown (indirect branch/call, return to unknown caller, step limit)."""
    ins, ret_stack = fapi.getInstructionAt(fapi.toAddr(addr)), []
    for _ in range(max_steps):
        if ins is None:
            return None
        if ctx.arch.plan_runs(fapi, ins):
            return ins
        ft, flows = ins.getFlowType(), ins.getFlows()
        if ft.isCall() and not ft.isComputed() and len(flows) == 1:  # bl: into the callee
            ret_stack.append(ins.getFallThrough())
            nxt = flows[0]
        elif ins.getMnemonicString() == "ret" and ret_stack:  # back from a callee we stepped into
            nxt = ret_stack.pop()
        elif ft.isCall() or ft.isComputed() or ft.isTerminal():  # blr, br, ret (unknown caller), eret
            log.debug("    walk from %#x stops at %s %s (%s)", addr, ins.getAddress(), ins, ft)
            return None
        elif ft.isJump():  # b
            nxt = flows[0]
        else:
            nxt = ins.getFallThrough()
        ins = fapi.getInstructionAt(nxt) if nxt is not None else None
    log.debug("    walk from %#x: step limit %d reached", addr, max_steps)
    return None


def prev_recorded_edges(ctx, fapi, addr, max_steps):
    """Mirror of next_recorded_insn(): walk backwards from `addr` through straight-line code
    and collect the computed edges by which execution can arrive there.  Returns a set of
    edge keys, or None = unknown (function entry, b/bl/br/jump table into the path, limit)."""
    edges, ins = set(), fapi.getInstructionAt(fapi.toAddr(addr))
    for _ in range(max_steps):
        if ins is None:
            return None
        a, off = ins.getAddress(), ins.getAddress().getOffset()
        if fapi.getFunctionAt(a) is not None:  # entered by a call, maybe an indirect one
            log.debug("    back-walk from %#x stops at function entry %s", addr, a)
            return None
        for ref in fapi.getReferencesTo(a):
            rt, key = ref.getReferenceType(), (ref.getFromAddress().getOffset(), off)
            if not rt.isFlow():
                continue
            if not (rt.isConditional() and key in ctx.candidates):
                log.debug("    back-walk from %#x stops at %s: %s from %s", addr, a, rt, ref.getFromAddress())
                return None
            edges.add(key)  # conditional branch into the path
        prev = fapi.getInstructionBefore(a)
        ft = prev.getFallThrough() if prev is not None else None
        if ft is None or ft.getOffset() != off:  # nothing falls through into here: block start
            return edges
        if ctx.arch.plan_runs(fapi, prev):  # fall-through outcome of a recorded instruction
            edges.add((prev.getAddress().getOffset(), off))
            return edges
        ins = prev
    log.debug("    back-walk from %#x: step limit %d reached", addr, max_steps)
    return None


def stage_successor_check(ctx, fapi, args):
    """After a hit edge, execution runs straight-line to the next recorded instruction.
    If we computed that instruction's outcomes, one of them must be a hit, too."""
    hit_srcs = {src for src, _ in ctx.hit_edges}
    cand_srcs = {src for src, _ in ctx.candidates}
    for key in sorted(ctx.hit_edges):
        nxt = next_recorded_insn(ctx, fapi, key[1], args.max_walk)
        n = nxt.getAddress().getOffset() if nxt is not None else None
        if nxt is None or n not in cand_srcs:
            log.debug("keep  %s  (successor %s)", fmt_edge(key, ctx.hit_edges[key]),
                      "unknown" if nxt is None else "%#x not analyzed" % n)
        elif n in hit_srcs:
            log.debug("keep  %s  (successor %#x %s is hit)", fmt_edge(key, ctx.hit_edges[key]), n, nxt)
        else:
            log.debug("DROP  %s  (successor %#x %s has no hit)", fmt_edge(key, ctx.hit_edges[key]), n, nxt)
            del ctx.hit_edges[key]


def stage_predecessor_check(ctx, fapi, args):
    """Mirror image: the source instruction of a hit edge must have been reached.  If every
    way into its straight-line block is an edge we computed, one of them must be a hit."""
    snapshot = set(ctx.hit_edges)
    for key in sorted(snapshot):
        preds = prev_recorded_edges(ctx, fapi, key[0], args.max_walk)
        if not preds:
            log.debug("keep  %s  (predecessors %s)", fmt_edge(key, ctx.hit_edges[key]),
                      "unknown" if preds is None else "none")
        elif preds & snapshot:
            log.debug("keep  %s  (predecessor %s is hit)", fmt_edge(key, ctx.hit_edges[key]),
                      ["%#x->%#x" % p for p in sorted(preds & snapshot)])
        else:
            log.debug("DROP  %s  (no predecessor hit: %s)", fmt_edge(key, ctx.hit_edges[key]),
                      ["%#x->%#x" % p for p in sorted(preds)])
            del ctx.hit_edges[key]


FILTER_STAGES = {  # name -> stage, run in this order (see --stages)
    "successor": stage_successor_check,
    "predecessor": stage_predecessor_check,
    # "my_idea": stage_my_idea,
}

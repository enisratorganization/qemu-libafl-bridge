#!/usr/bin/env python3
"""
ghidra_edge_coverage.py -- replay a QEMU (aarch64) edge-coverage hitmap in Ghidra.

The modified QEMU hashes (edge_id, pc) of every *conditional* control-flow
decision (b.cond, cbz/cbnz, tbz/tbnz and the CC consumers csel/ccmp/fcsel/
fccmp/...) into a probabilistic hitmap.  This script
  1. stage "compute": walks all code and, for every such instruction, runs
     `qemu-edge-coverage-aarch64` once per possible outcome to learn which
     hitmap offset that outcome produces; outcomes whose offset is in the
     recorded hitmap become "hit edges" (global HIT_EDGES),
  2. runs the filter stages (--stages) which may only *remove* edges, i.e.
     false positives caused by hash collisions,
  3. logs a summary and optionally (--comment) writes pre-comments to the
     edge targets.
Indirect branches (br/blr, recorded by QEMU with the register value) cannot
be computed statically and are ignored.

Usage:
  source env.sh
  python ghidra_edge_coverage.py --dry-run -v --start 0x5fef0000 --end 0x5fef4000
  python ghidra_edge_coverage.py --comment
"""
import argparse
import json
import logging
import os
import subprocess
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor

log = logging.getLogger("edgecov")

# --------------------------------------------------------------------------
# Globals ("caches"), edges are keyed by (src, dst) address offsets.
# For CC consumers (csel, ccmp, ...) the edge is (pc, pc+4); both outcomes
# ("enc.cond ne=true" / "...=false") share that key, told apart by the label.
# --------------------------------------------------------------------------
HITMAP = set()   # offsets recorded by QEMU (from metadata.json)
CANDIDATES = {}  # (src, dst) -> {"insn": str, "offsets": {offset: label}}  every computable edge
HIT_EDGES = {}   # (src, dst) -> {"insn": str, "offsets": {offset: label}}  only offsets in HITMAP
TOOL_CACHE = {}  # "pc|insn|nzcv|regs" -> [offsets]  results of the qemu tool (optionally persisted)

# --------------------------------------------------------------------------
# AArch64 knowledge
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

# Instructions that consume a condition without branching.  QEMU calls
# arm_test_cc() for them as well, so they are recorded.  Ghidra prints the
# *alias* condition for cset/cinc/..., which is the inverse of the encoded
# one -> always take the cond field from the encoding (bits 15:12).
CC_CONSUMERS = {"csel", "csinc", "csinv", "csneg", "cset", "csetm", "cinc", "cinv", "cneg",
                "ccmp", "ccmn", "fcsel", "fccmp", "fccmpe"}


def cond_holds(cond, nzcv):
    """ARM ARM ConditionHolds()."""
    n, z, c, v = (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1, nzcv & 1
    res = [z, c, n, v, c and not z, n == v, (not z) and n == v, True][cond >> 1]
    return bool(res) != bool(cond & 1 and cond != 15)


def insn_word(ins):
    return int.from_bytes(bytes(b & 0xFF for b in ins.getBytes()), "little")


def branch_target(fapi, ins):
    """Static target of a conditional branch: the conditional flow reference (fallback: getFlows)."""
    for ref in fapi.getReferencesFrom(ins.getAddress()):
        rt = ref.getReferenceType()
        if rt.isFlow() and rt.isConditional():
            return ref.getToAddress().getOffset()
    flows = ins.getFlows()
    return flows[0].getOffset() if len(flows) == 1 else None


def plan_runs(fapi, ins):
    """Tool runs needed for one instruction: [(dst, label, nzcv, regs), ...].
    Empty if QEMU records nothing for it -- or if we cannot compute it (br/blr: indirect)."""
    m = ins.getMnemonicString().lower()
    pc = ins.getAddress().getOffset()
    word = insn_word(ins)
    ft = ins.getFlowType()

    if m in CC_CONSUMERS:
        cond = (word >> 12) & 0xF
        if cond >= 14:  # "al"/"nv": arm_test_cc() returns before recording
            return []
        return [(pc + 4, "enc.cond %s=%s NZCV=%x" % (COND_NAMES[cond], str(cond_holds(cond, f)).lower(), f), f, ())
                for f in FLAG_CASES[cond >> 1]]

    if not ft.isConditional() or ft.isComputed():
        return []  # fall-through, b, bl, br, blr, ret, ...: not conditional / not computable
    taken, fall = branch_target(fapi, ins), ins.getFallThrough()
    if taken is None or fall is None:
        log.warning("%s %s: conditional without static target/fallthrough, skipped", ins.getAddress(), ins)
        return []
    fall = fall.getOffset()

    def run(is_taken, desc, nzcv=0, regs=()):
        return (taken, "taken " + desc, nzcv, regs) if is_taken else (fall, "fallthrough " + desc, nzcv, regs)

    if m in ("cbz", "cbnz"):  # QEMU's edge id is (Rt != 0) -> try Rt = 0 and 1
        rt = word & 0x1F
        return [run((val == 0) == (m == "cbz"), "x%d=%d" % (rt, val), 0, ((rt, val),)) for val in (0, 1)]
    if m in ("tbz", "tbnz"):  # QEMU's edge id is the tested bit
        rt, bit = word & 0x1F, (((word >> 31) & 1) << 5) | ((word >> 19) & 0x1F)
        return [run((val == 0) == (m == "tbz"), "x%d=%#x" % (rt, val), 0, ((rt, val),)) for val in (0, 1 << bit)]
    if m.startswith("b.") or m.startswith("bc."):
        cond = word & 0xF
        if cond >= 14:
            return []
        return [run(cond_holds(cond, f), "%s NZCV=%x" % (COND_NAMES[cond], f), f) for f in FLAG_CASES[cond >> 1]]

    log.warning("%s %s: unhandled conditional instruction, skipped", ins.getAddress(), ins)
    return []


# --------------------------------------------------------------------------
# Inputs: hitmap and the qemu tool
# --------------------------------------------------------------------------
def load_hitmap(path, key, edge_elems):
    """Find `key` anywhere in the json and take the first list (>10 ints) below it."""
    def find_list(o):
        if isinstance(o, list) and len(o) > 10 and all(isinstance(x, int) for x in o):
            return o
        for v in (o.values() if isinstance(o, dict) else o if isinstance(o, list) else ()):
            r = find_list(v)
            if r is not None:
                return r
        return None

    def find_key(o):
        if isinstance(o, dict):
            for k, v in o.items():
                r = find_list(v) if str(k) == key else None
                if r is None:
                    r = find_key(v)
                if r is not None:
                    return r
        elif isinstance(o, list):
            for v in o:
                r = find_key(v)
                if r is not None:
                    return r
        return None

    with open(path) as fh:
        lst = find_key(json.load(fh))
    if lst is None:
        raise SystemExit("no list (>10 ints) found under key %s in %s" % (key, path))
    bad = [x for x in lst if not 0 <= x < edge_elems]
    if bad:
        log.warning("%d hitmap offsets outside [0, %d): %s", len(bad), edge_elems, bad[:10])
    log.info("hitmap: %d offsets under key %s... (fill %.2f%% of %d)",
             len(lst), key[:10], 100.0 * len(set(lst)) / edge_elems, edge_elems)
    return set(lst)


def covrec(args):
    return "edge_elem_sz=%d,edge_elems=%d,edge_enable=on" % (args.edge_elem_sz, args.edge_elems)


def tool_key(pc, insn_hex, nzcv, regs):
    return "%x|%s|%x|%s" % (pc, insn_hex, nzcv, ",".join("%d=%x" % r for r in regs))


def run_tool(args, pc, insn_hex, nzcv, regs):
    """Execute one instruction in the qemu tool, return the list of hitmap offsets."""
    key = tool_key(pc, insn_hex, nzcv, regs)
    if key in TOOL_CACHE:
        return TOOL_CACHE[key]
    cmd = [args.tool, "-a", hex(pc), "-i", insn_hex, "-f", hex(nzcv << 28), "-o", covrec(args)]
    for n, v in regs:
        cmd += ["-r", "%d=%#x" % (n, v)]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    lines = res.stdout.strip().splitlines()
    if res.returncode != 0 or not lines:
        raise RuntimeError("tool failed (%d): %s\n%s" % (res.returncode, " ".join(cmd), res.stderr))
    TOOL_CACHE[key] = json.loads(lines[-1])["edges"]
    log.debug("tool %s -> %s", " ".join(cmd[1:7] + cmd[9:]), TOOL_CACHE[key])
    return TOOL_CACHE[key]


def load_tool_cache(args):
    if args.tool_cache and os.path.exists(args.tool_cache):
        with open(args.tool_cache) as fh:
            data = json.load(fh)
        if data.get("covrec") == covrec(args):
            TOOL_CACHE.update(data["entries"])
            log.info("tool cache: %d entries from %s", len(TOOL_CACHE), args.tool_cache)
        else:
            log.warning("tool cache %s has other covrec options, ignored", args.tool_cache)


def save_tool_cache(args):
    if args.tool_cache and TOOL_CACHE:
        with open(args.tool_cache, "w") as fh:
            json.dump({"covrec": covrec(args), "entries": TOOL_CACHE}, fh)
        log.info("tool cache: %d entries written to %s", len(TOOL_CACHE), args.tool_cache)


# --------------------------------------------------------------------------
# Code walking
# --------------------------------------------------------------------------
def iter_code(fapi, args):
    """All instructions in [--start, --end] (default: whole program)."""
    if args.start is not None:
        start = fapi.toAddr(args.start)
        ins = fapi.getInstructionAt(start) or fapi.getInstructionAfter(start)
    else:
        ins = fapi.getFirstInstruction()
    while ins is not None and (args.end is None or ins.getAddress().getOffset() <= args.end):
        yield ins
        ins = fapi.getInstructionAfter(ins)


def next_recorded_insn(fapi, addr, max_steps):
    """Follow the straight-line path from `addr` up to the next instruction QEMU records.
    Follows fall-through and direct `b`, steps into direct `bl` (and back on its `ret`).
    Returns None = unknown (indirect branch/call, return to unknown caller, step limit)."""
    ins, ret_stack = fapi.getInstructionAt(fapi.toAddr(addr)), []
    for _ in range(max_steps):
        if ins is None:
            return None
        if plan_runs(fapi, ins):
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


def prev_recorded_edges(fapi, addr, max_steps):
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
            if not (rt.isConditional() and key in CANDIDATES):
                log.debug("    back-walk from %#x stops at %s: %s from %s", addr, a, rt, ref.getFromAddress())
                return None
            edges.add(key)  # conditional branch into the path
        prev = fapi.getInstructionBefore(a)
        ft = prev.getFallThrough() if prev is not None else None
        if ft is None or ft.getOffset() != off:  # nothing falls through into here: block start
            return edges
        if plan_runs(fapi, prev):  # fall-through outcome of a recorded instruction
            edges.add((prev.getAddress().getOffset(), off))
            return edges
        ins = prev
    log.debug("    back-walk from %#x: step limit %d reached", addr, max_steps)
    return None


def fmt_edge(key, info):
    return "%#x -> %#x  %-28s [%s]" % (key[0], key[1], info["insn"],
                                       ", ".join("%s @%d" % (l, o) for o, l in sorted(info["offsets"].items())))


# --------------------------------------------------------------------------
# Stage 1: compute hit edges with the qemu tool
# --------------------------------------------------------------------------
def stage_compute(fapi, args):
    plan = []  # (src, dst, label, insn text, tool job)
    n_rec = 0
    for ins in iter_code(fapi, args):
        runs = plan_runs(fapi, ins)
        if not runs:
            continue
        n_rec += 1
        pc = ins.getAddress().getOffset()
        insn_hex = bytes(b & 0xFF for b in ins.getBytes()).hex(" ")
        log.debug("rec insn %#x %-30s %s", pc, ins, [(hex(d), l) for d, l, _, _ in runs])
        for dst, label, nzcv, regs in runs:
            plan.append((pc, dst, label, str(ins), (pc, insn_hex, nzcv, regs)))
        if args.limit and n_rec >= args.limit:
            log.info("--limit %d recorded instructions reached at %s", args.limit, ins.getAddress())
            break

    jobs = {tool_key(*job): job for *_, job in plan}
    todo = [job for key, job in jobs.items() if key not in TOOL_CACHE]
    log.info("stage compute: %d recorded insns, %d outcomes, %d tool runs (%d cached), %d jobs",
             n_rec, len(plan), len(jobs), len(jobs) - len(todo), args.jobs)
    t0 = time.time()
    try:
        with ThreadPoolExecutor(args.jobs) as ex:
            for i, _ in enumerate(ex.map(lambda job: run_tool(args, *job), todo), 1):
                if i % 2000 == 0:
                    log.info("  tool runs: %d/%d (%.0fs)", i, len(todo), time.time() - t0)
    finally:
        if todo:
            log.info("  tool runs done in %.1fs", time.time() - t0)
            save_tool_cache(args)

    for src, dst, label, text, job in plan:
        offsets = TOOL_CACHE[tool_key(*job)]
        if len(offsets) != 1:
            log.warning("%#x %s (%s): tool reported %s, expected exactly one offset", src, text, label, offsets)
        cand = CANDIDATES.setdefault((src, dst), {"insn": text, "offsets": {}})
        for off in offsets:
            cand["offsets"][off] = label
    for key, cand in sorted(CANDIDATES.items()):
        hit = {o: l for o, l in cand["offsets"].items() if o in HITMAP}
        if hit:
            HIT_EDGES[key] = {"insn": cand["insn"], "offsets": hit}
            log.debug("HIT   %s", fmt_edge(key, HIT_EDGES[key]))


# --------------------------------------------------------------------------
# Filter stages -- each one may only *remove* entries from HIT_EDGES.
# Each stage decides on a snapshot of HIT_EDGES taken at its start (no
# cascading inside a stage) and keeps an edge whenever it cannot tell.
# To add a stage: write `def stage_xyz(fapi, args)` and add it to
# FILTER_STAGES below (selectable via --stages, edge count is logged).
# Ideas for later stages:
#   - collision/ambiguity: an offset matched by several candidate edges only
#     proves that *one* of them fired -> weaken edges whose offsets are all
#     "explained" by better supported edges
#   - follow `ret` back to the call sites of the function (if it has only
#     direct callers), follow `b` backwards in the predecessor walk
#   - exempt the crash/exit location (metadata "elr"): execution stops there,
#     so the edge leading to it has no successor hit
# --------------------------------------------------------------------------
def stage_successor_check(fapi, args):
    """After a hit edge, execution runs straight-line to the next recorded instruction.
    If we computed that instruction's outcomes, one of them must be a hit, too."""
    hit_srcs = {src for src, _ in HIT_EDGES}
    cand_srcs = {src for src, _ in CANDIDATES}
    for key in sorted(HIT_EDGES):
        nxt = next_recorded_insn(fapi, key[1], args.max_walk)
        n = nxt.getAddress().getOffset() if nxt is not None else None
        if nxt is None or n not in cand_srcs:
            log.debug("keep  %s  (successor %s)", fmt_edge(key, HIT_EDGES[key]),
                      "unknown" if nxt is None else "%#x not analyzed" % n)
        elif n in hit_srcs:
            log.debug("keep  %s  (successor %#x %s is hit)", fmt_edge(key, HIT_EDGES[key]), n, nxt)
        else:
            log.debug("DROP  %s  (successor %#x %s has no hit)", fmt_edge(key, HIT_EDGES[key]), n, nxt)
            del HIT_EDGES[key]


def stage_predecessor_check(fapi, args):
    """Mirror image: the source instruction of a hit edge must have been reached.  If every
    way into its straight-line block is an edge we computed, one of them must be a hit."""
    snapshot = set(HIT_EDGES)
    for key in sorted(snapshot):
        preds = prev_recorded_edges(fapi, key[0], args.max_walk)
        if not preds:
            log.debug("keep  %s  (predecessors %s)", fmt_edge(key, HIT_EDGES[key]),
                      "unknown" if preds is None else "none")
        elif preds & snapshot:
            log.debug("keep  %s  (predecessor %s is hit)", fmt_edge(key, HIT_EDGES[key]),
                      ["%#x->%#x" % p for p in sorted(preds & snapshot)])
        else:
            log.debug("DROP  %s  (no predecessor hit: %s)", fmt_edge(key, HIT_EDGES[key]),
                      ["%#x->%#x" % p for p in sorted(preds)])
            del HIT_EDGES[key]


FILTER_STAGES = {  # name -> stage, run in this order (see --stages)
    "successor": stage_successor_check,
    "predecessor": stage_predecessor_check,
    # "my_idea": stage_my_idea,
}


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------
TAG = "EDGECOV"


def annotate(fapi, program, args):
    """Pre-comment at every edge target; old EDGECOV lines in the range are replaced."""
    by_dst = defaultdict(list)
    for key, info in sorted(HIT_EDGES.items()):
        #by_dst[key[1]].append("%s: hit from %#x %s [%s]" % (TAG, key[0], info["insn"],
        #                                                  ", ".join(sorted(set(info["offsets"].values())))))
        by_dst[key[1]].append("%s: hit from %#x" % (TAG, key[0]))

    tx = program.startTransaction("edge coverage comments")
    try:
        n_clear = 0
        for ins in iter_code(fapi, args):  # drop stale tags from earlier runs
            old = fapi.getPreComment(ins.getAddress())
            if old and TAG in old:
                keep = [l for l in old.splitlines() if not l.startswith(TAG)]
                fapi.setPreComment(ins.getAddress(), "\n".join(keep) if keep else None)
                n_clear += 1
        for dst, lines in sorted(by_dst.items()):
            addr = fapi.toAddr(dst)
            old = fapi.getPreComment(addr)
            fapi.setPreComment(addr, "\n".join(([old] if old else []) + lines))
            log.debug("comment @%#x: %s", dst, " | ".join(lines))
        log.info("comments: %d stale cleared, %d targets annotated%s",
                 n_clear, len(by_dst), " (dry run: rolled back)" if args.dry_run else "")
        program.endTransaction(tx, not args.dry_run)
        if not args.dry_run:
            from ghidra.util.task import TaskMonitor
            program.save("edge coverage comments", TaskMonitor.DUMMY)
    except Exception:
        program.endTransaction(tx, False)
        raise


def summary(stage_counts):
    explained = set().union(*(set(i["offsets"]) for i in HIT_EDGES.values())) if HIT_EDGES else set()
    cand_offs = set().union(*(set(c["offsets"]) for c in CANDIDATES.values())) if CANDIDATES else set()
    log.info("---- summary ----")
    for name, n in stage_counts:
        log.info("  %-24s %6d hit edges", name, n)
    log.info("  hitmap offsets: %d, explained by final edges: %d, by any candidate: %d, unexplained "
             "(indirect branches / outside range / noise): %d",
             len(HITMAP), len(explained & HITMAP), len(cand_offs & HITMAP), len(HITMAP - cand_offs))
    for key, info in sorted(HIT_EDGES.items()):
        log.debug("FINAL %s", fmt_edge(key, info))


# --------------------------------------------------------------------------
def parse_args():
    auto_int = lambda s: int(s, 0)
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--program", default=os.environ.get("PROG_PATH", ""), help="program path in project")
    p.add_argument("--metadata", required=True, help="json containing the hitmap (input)")
    p.add_argument("--hitmap-key", default="304795868863881800668378148837488880366",
                   help="json key under which the hitmap list is found (searched recursively)")
    p.add_argument("--tool", default="./qemu-edge-coverage-aarch64", help="qemu-edge-coverage-aarch64 binary")
    p.add_argument("--edge-elem-sz", type=int, default=1, help="covrec edge_elem_sz (as used in the fuzz run)")
    p.add_argument("--edge-elems", type=int, default=65536, help="covrec edge_elems (as used in the fuzz run)")
    p.add_argument("--tool-cache", default="", help="persist tool results ('' = off)")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4, help="parallel tool runs")
    p.add_argument("--start", type=auto_int, help="first address to analyze (debug)")
    p.add_argument("--end", type=auto_int, help="last address to analyze (debug)")
    p.add_argument("--limit", type=int, default=0, help="max recorded instructions to analyze (debug)")
    p.add_argument("--stages", default=",".join(FILTER_STAGES), type=lambda s: [x for x in s.split(",") if x],
                   help="filter stages to run, in order (default: %(default)s; '' = none)")
    p.add_argument("--max-walk", type=int, default=256, help="max straight-line steps in filter walks")
    p.add_argument("--comment", action="store_true", help="add pre-comments at hit edge targets")
    p.add_argument("--dry-run", action="store_true", help="do not commit/save any program changes")
    p.add_argument("-v", "--verbose", action="store_true", help="debug logging")
    p.add_argument("--log-file", help="also write the log to this file")
    args = p.parse_args()
    for name in args.stages:
        if name not in FILTER_STAGES:
            p.error("unknown stage %r, choose from %s" % (name, ",".join(FILTER_STAGES)))
    return args


def main():
    args = parse_args()
    handlers = [logging.StreamHandler()] + ([logging.FileHandler(args.log_file, "w")] if args.log_file else [])
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, handlers=handlers,
                        format="%(asctime)s %(levelname)-7s %(message)s", datefmt="%H:%M:%S")
    log.debug("args: %s", vars(args))
    HITMAP.update(load_hitmap(args.metadata, args.hitmap_key, args.edge_elems))
    load_tool_cache(args)

    import pyghidra
    pyghidra.start()
    from ghidra.program.flatapi import FlatProgramAPI

    with pyghidra.open_project(os.environ["GHIDRA_PROJECT_DIR"], os.environ["GHIDRA_PROJECT_NAME"]) as project:
        with pyghidra.program_context(project, args.program) as program:
            fapi = FlatProgramAPI(program)
            log.info("program %s (%s), range %s..%s", program.getName(), program.getLanguageID(),
                     hex(args.start) if args.start is not None else "min",
                     hex(args.end) if args.end is not None else "max")
            t0 = time.time()
            stage_compute(fapi, args)
            counts = [("compute", len(HIT_EDGES))]
            log.info("stage compute: %d candidate edges, %d hit (%.1fs)",
                     len(CANDIDATES), len(HIT_EDGES), time.time() - t0)
            for name in args.stages:
                t0, before = time.time(), len(HIT_EDGES)
                FILTER_STAGES[name](fapi, args)
                counts.append((name, len(HIT_EDGES)))
                log.info("stage %s: %d -> %d hit edges (%.1fs)", name, before, len(HIT_EDGES), time.time() - t0)
            summary(counts)
            if args.comment:
                annotate(fapi, program, args)


if __name__ == "__main__":
    main()

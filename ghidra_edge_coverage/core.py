"""
Arch-independent engine: hitmap loading, tool replay, code walking, the
compute stage and the output.  The filter stages live in filter.py.

Edges are keyed by (src, dst) address offsets.  For decisions where both
outcomes continue at the next instruction (csel, conditional data
processing, IT-gated insns, ...) the edge is (pc, pc+insn_len); both
outcomes share that key, told apart by their label.

State ("caches", module globals like in the original script):
  HITMAP      offsets recorded by QEMU (from the .metadata files)
  CANDIDATES  (src, dst) -> {"insn": str, "offsets": {offset: label}}  every computable edge
  HIT_EDGES   (src, dst) -> {"insn": str, "offsets": {offset: label}}  only offsets in HITMAP
  TOOL_CACHE  "pc|insn|nzcv|regs#mode" -> [offsets]   results of the qemu tool
"""
import json
import logging
import os
import subprocess
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor

log = logging.getLogger("edgecov")

# set by cli.main() after the arch is (auto-)detected
ARCH = None

HITMAP = set()
CANDIDATES = {}
HIT_EDGES = {}
TOOL_CACHE = {}

DEFAULT_FLAGS_MASK = 0xF0000000  # NZCV, the tool's default -m
CACHE_VERSION = 1  # tool caches without/with another version or covrec are ignored


# --------------------------------------------------------------------------
# Inputs: hitmap and the qemu tool
# --------------------------------------------------------------------------
def load_hitmap(paths, key, edge_elems):
    """Find `key` anywhere in each json and take the first list (>10 ints) below it
    (key "" = accept it under any key).  Each path is a .metadata file or a
    folder whose .metadata files are all loaded (a fuzzer corpus: every file
    stores its hitmap under its own input-hash key)."""
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
                r = find_list(v) if not key or str(k).startswith(key) else None
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

    log.info("loading hitmap from %d path(s): %s", len(paths), ", ".join(paths))
    all_edges = set()  # all edge ids collected
    num_files = 0
    for path in paths:
        if os.path.isdir(path):
            fnames = sorted(os.path.join(path, f) for f in os.listdir(path)
                            if f.endswith(".metadata"))
        else:
            fnames = [path]
        for fname in fnames:
            with open(fname) as fh:
                lst = find_key(json.load(fh))
            if lst is None:
                log.info("no list (>10 ints) found under key %s in %s", key or "<any>", fname)
                continue
            all_edges.update(lst)
            num_files += 1
    bad = sorted(x for x in all_edges if not 0 <= x < edge_elems)
    if bad:
        log.debug("%d hitmap offsets outside [0, %d): %s", len(bad), edge_elems, bad[:10])
    log.info("Loaded %d metadata files, total hitmap: %d offsets under key %s (fill %.2f%% of %d)",
             num_files, len(all_edges), key or "<any>", 100.0 * len(all_edges) / edge_elems, edge_elems)

    return all_edges


def covrec(args):
    return "edge_elem_sz=%d,edge_elems=%d,edge_enable=on" % (args.edge_elem_sz, args.edge_elems)


def tool_key(pc, insn_hex, nzcv, regs, mode):
    return "%x|%s|%x|%s#%s" % (pc, insn_hex, nzcv,
                               ",".join("%d=%x" % r for r in regs),
                               json.dumps(mode, sort_keys=True))


def run_tool(args, pc, insn_hex, nzcv, regs, mode):
    """Execute one instruction in the qemu tool, return the list of hitmap offsets."""
    key = tool_key(pc, insn_hex, nzcv, regs, mode)
    if key in TOOL_CACHE:
        return TOOL_CACHE[key]
    flags, fmask = ARCH.tool_flags(nzcv, mode)
    cmd = [args.tool, "-a", hex(pc), "-i", insn_hex, "-f", hex(flags), "-o", covrec(args)]
    if fmask != DEFAULT_FLAGS_MASK:
        cmd += ["-m", hex(fmask)]
    for n, v in regs:
        cmd += ["-r", "%d=%#x" % (n, v)]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    lines = res.stdout.strip().splitlines()
    if res.returncode != 0 or not lines:
        raise RuntimeError("tool failed (%d): %s\n%s" % (res.returncode, " ".join(cmd), res.stderr))
    TOOL_CACHE[key] = json.loads(lines[-1])["edges"]
    log.debug("tool %s -> %s", " ".join(cmd[1:]), TOOL_CACHE[key])
    return TOOL_CACHE[key]


def load_tool_cache(args):
    if args.tool_cache and os.path.exists(args.tool_cache):
        with open(args.tool_cache) as fh:
            data = json.load(fh)
        if data.get("cache_version") == CACHE_VERSION and data.get("covrec") == covrec(args):
            TOOL_CACHE.update(data["entries"])
            log.info("tool cache: %d entries from %s", len(TOOL_CACHE), args.tool_cache)
        else:
            log.warning("tool cache %s has an incompatible format or covrec options, ignored", args.tool_cache)


def save_tool_cache(args):
    if args.tool_cache and TOOL_CACHE:
        with open(args.tool_cache, "w") as fh:
            json.dump({"cache_version": CACHE_VERSION, "covrec": covrec(args), "entries": TOOL_CACHE}, fh)
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
        runs = ARCH.plan_runs(fapi, ins)
        if not runs:
            continue
        n_rec += 1
        pc = ins.getAddress().getOffset()
        insn_hex = bytes(b & 0xFF for b in ins.getBytes()).hex(" ")
        log.debug("rec insn %#x %-30s %s", pc, ins, [(hex(d), l) for d, l, *_ in runs])
        for dst, label, nzcv, regs, mode in runs:
            plan.append((pc, dst, label, str(ins), (pc, insn_hex, nzcv, regs, mode)))
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
                if i % 5000 == 0:
                    log.debug("  tool runs: %d/%d (%.0fs)", i, len(todo), time.time() - t0)
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
# Output
# --------------------------------------------------------------------------
TAG = "EDGECOV"


def annotate(fapi, program, args):
    """Pre-comment at every edge target; old EDGECOV lines in the range are replaced."""
    by_dst = defaultdict(list)
    for key, info in sorted(HIT_EDGES.items()):
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

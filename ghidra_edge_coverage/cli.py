"""
Command line interface: argument parsing, logging setup and the top-level
flow (open the Ghidra project -> detect the arch -> run the stages).

The Ghidra project is opened through pyghidra, so this runs under plain
CPython, not inside a Ghidra script:

  source env.sh   # GHIDRA_PROJECT_DIR, GHIDRA_PROJECT_NAME, PROG_PATH
  python -m ghidra_edge_coverage --metadata path/to/corpus   # or a single .metadata
  python -m ghidra_edge_coverage --metadata ... --comment    # annotate in the project

How it works (see core.py and the arch modules for the details):
  1. stage "compute" walks all code and, for every instruction QEMU records
     a conditional decision for, runs `qemu-edge-coverage-<arch>` once per
     possible outcome to learn which hitmap offset that outcome produces;
     outcomes whose offset is in the recorded hitmap become "hit edges";
  2. the filter stages (--stages) may only *remove* edges, i.e. false
     positives caused by hash collisions;
  3. logs a summary and optionally (--comment) writes pre-comments with
     "EDGECOV: hit from <src>" to the edge targets.

Per-architecture knowledge (which instructions are recorded, how to decode
their condition and CPU state) lives in archs/<arch>.py; knowledge common
to the ARM architectures in archs/arm_shared.py; the filter stages in
filter.py; everything else in core.py.
"""
import argparse
import logging
import os
import time

from . import core, filter as filter_stages
from .archs import ARCHS, get_arch
from .archs import aarch64, arm  # noqa: F401  (populate the registry)


def parse_args(argv=None):
    auto_int = lambda s: int(s, 0)
    p = argparse.ArgumentParser(
        description="Replay a QEMU edge-coverage hitmap in Ghidra: mark (and "
                    "optionally comment) every conditional-decision edge whose "
                    "hitmap offset was recorded by a fuzzing run.",
        epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--program", default=os.environ.get("PROG_PATH", ""),
                   help="program path inside the Ghidra project "
                        "(default: $PROG_PATH)")
    p.add_argument("--metadata", required=True, nargs="+",
                   help="json .metadata files containing the hitmap and/or folders "
                        "whose *.metadata files are all loaded (usually a fuzzer corpus)")
    p.add_argument("--hitmap-key", default="",
                   help="json key under which the hitmap list is found (searched "
                        "recursively; any unique prefix; default: accept the first "
                        "list of >10 ints under any key)")
    p.add_argument("--arch", default="auto", choices=["auto"] + sorted(ARCHS),
                   help="target architecture (default: auto-detect from the program language)")
    p.add_argument("--tool", default="",
                   help="qemu-edge-coverage-<arch> binary (default: per-arch name, "
                        "resolved from $PATH)")
    p.add_argument("--edge-elem-sz", type=int, default=1,
                   help="covrec edge_elem_sz (as used in the fuzz run)")
    p.add_argument("--edge-elems", type=int, default=65536,
                   help="covrec edge_elems (as used in the fuzz run)")
    p.add_argument("--tool-cache", default="",
                   help="persist tool results to this json ('' = off); speeds up "
                        "re-runs enormously")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4,
                   help="parallel tool runs")
    p.add_argument("--start", type=auto_int, help="first address to analyze (debug)")
    p.add_argument("--end", type=auto_int, help="last address to analyze (debug)")
    p.add_argument("--limit", type=int, default=0,
                   help="max recorded instructions to analyze (debug)")
    p.add_argument("--stages", default=",".join(filter_stages.FILTER_STAGES),
                   type=lambda s: [x for x in s.split(",") if x],
                   help="filter stages to run, in order (default: %(default)s; '' = none)")
    p.add_argument("--max-walk", type=int, default=256,
                   help="max straight-line steps in filter walks")
    p.add_argument("--comment", action="store_true",
                   help="add pre-comments at hit edge targets (and save the program)")
    p.add_argument("--dry-run", action="store_true",
                   help="do not commit/save any program changes")
    p.add_argument("-v", "--verbose", action="store_true", help="debug logging")
    p.add_argument("--log-file", help="also write the log to this file")
    args = p.parse_args(argv)
    for name in args.stages:
        if name not in filter_stages.FILTER_STAGES:
            p.error("unknown stage %r, choose from %s"
                    % (name, ",".join(filter_stages.FILTER_STAGES)))
    return args


def main(argv=None):
    args = parse_args(argv)
    handlers = [logging.StreamHandler()] + ([logging.FileHandler(args.log_file, "w")] if args.log_file else [])
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, handlers=handlers,
                        format="%(asctime)s %(levelname)-7s %(message)s", datefmt="%H:%M:%S")
    log = logging.getLogger("edgecov")
    log.debug("args: %s", vars(args))
    ctx = core.Ctx()
    ctx.hitmap.update(core.load_hitmap(args.metadata, args.hitmap_key, args.edge_elems))
    core.load_tool_cache(ctx, args)

    import pyghidra
    pyghidra.start()
    from ghidra.program.flatapi import FlatProgramAPI

    with pyghidra.open_project(os.environ["GHIDRA_PROJECT_DIR"], os.environ["GHIDRA_PROJECT_NAME"]) as project:
        with pyghidra.program_context(project, args.program) as program:
            fapi = FlatProgramAPI(program)
            ctx.arch = get_arch(args.arch, program)
            ctx.arch.bind(program)
            if not args.tool:
                args.tool = ctx.arch.TOOL
            log.info("program %s (%s), arch %s, tool %s, range %s..%s",
                     program.getName(), program.getLanguageID(), ctx.arch.NAME, args.tool,
                     hex(args.start) if args.start is not None else "min",
                     hex(args.end) if args.end is not None else "max")
            t0 = time.time()
            core.stage_compute(ctx, fapi, args)
            counts = [("compute", len(ctx.hit_edges))]
            log.info("stage compute: %d candidate edges, %d hit (%.1fs)",
                     len(ctx.candidates), len(ctx.hit_edges), time.time() - t0)
            for name in args.stages:
                t0, before = time.time(), len(ctx.hit_edges)
                filter_stages.FILTER_STAGES[name](ctx, fapi, args)
                counts.append((name, len(ctx.hit_edges)))
                log.info("stage %s: %d -> %d hit edges (%.1fs)",
                         name, before, len(ctx.hit_edges), time.time() - t0)
            core.summary(ctx, counts)
            if args.comment:
                core.annotate(ctx, fapi, program, args)

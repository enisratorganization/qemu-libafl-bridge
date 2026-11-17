# AGENTS.md (repo root): session log + how to rebuild/re-verify

Repo: a fork of qemu-libafl-bridge (the QEMU used by LibAFL). It **is** a
git repo: use `git diff`/`git status` to check and revert your work; no
manual backups needed.

User scope: improve and refactor
- the **firmware re-hosting** code: `hw/fastproto/**`, `include/libafl/
  instrument.h` + `libafl/instrument.c` (PC hooks), the hook insertion in
  `accel/tcg/translator.c`, and `hw/ufs/**` where the fork's re-hosting
  mods live;
- the **edge-coverage replay tooling**: `system/edge-coverage.c`, its
  `meson.build` rule, and the `ghidra_edge_coverage/` package.

Don't touch the other LibAFL modifications (coverage, snapshots, ...), but
report any bugs you find in them (§5).

User rules:
- **Propose changes first**, concise and well outlined, and **wait for
  approval**. Apply them in batches; the user may pick items.
- The code is experimental and hacky on purpose: no unit tests, and QEMU
  style is not required. Aim for readable, concise comments and good
  examples.
- Prefer a **comment** over a code change for cosmetic findings. Small
  fixes and asserts are fine.
- Run the relevant e2e test (§2) before and after every change.
- For re-hosting knowledge (workflow, APIs, pitfalls) the **user-facing
  doc is `hw/fastproto/AGENTS.md`**; this file is the session/maintenance
  log - keep re-hosting how-to out of it.

## 1. Environment facts

- Session scratch dirs under `/tmp` can get wiped without warning (it
  happened twice and took baselines along). Keep scratch files, baselines
  and logs in a persistent directory outside `/tmp`.
- Build dirs (created by the user; re-create with the configure lines
  below if missing):
  - `buildexec/` - aarch64-softmmu: `qemu-system-aarch64` and the
    `qemu-edge-coverage-aarch64` tool;
  - `build_edgecov_arm/` - arm-softmmu: the `qemu-edge-coverage-arm` tool;
  - other `build*/` dirs belong to the user, don't touch them.
- Configure line (only `--target-list` differs between the build dirs):
  ```
  mkdir buildexec && cd buildexec && ../configure --target-list=aarch64-softmmu \
    --enable-debug --disable-werror --enable-gcrypt --disable-tools \
    '--extra-cflags=-DDUMMY_TIMERS' --disable-docs
  ninja qemu-system-aarch64
  ```
  `DUMMY_TIMERS` makes the guest clock deterministic (details in
  `hw/fastproto/AGENTS.md` §2). UFS I/O is still asynchronous, so
  timestamps and poll counts vary slightly between runs.
- A full build takes about 50 s with 16 cores; incremental builds are
  seconds. `ninja qemu-system-aarch64 2>&1 | grep -E "error:|FAILED"`.
- The `qemu-edge-coverage-<target>` tools are built by the normal `ninja`
  for aarch64 and arm (see the "EDGE COVERAGE" rule in `meson.build`); the
  rule is skipped when building as a lib for the Rust bridge.
- Firmware images: `images/` (in-tree) for redfin. The mt6768 images
  (bl31/tee/lk, preloaded SRAM, config area, ...) are **not in-tree** -
  copy them into `images/`; the command lines in the machine file headers
  expect them there.
- Warning baseline: **51 warnings, 0 errors**, none of them in
  `hw/fastproto/**`, `libafl/instrument.c`, `hw/ufs/**` or the
  edge-coverage files. The only warning in a file we touch is
  `translator.c: label 'post_translate_insn' defined but not used`
  (pre-existing, belongs to LibAFL's commented-out backdoor block), plus
  one `arm_tcg_gen_rec_edge` pointer-type warning from our `trans_CBZ`
  call (same known class as §5, aarch64 build only). The
  rest is listed in §5.
- Syntax-check the templates (never compiled by meson): take the
  `compile_commands.json` entry for `arm/redfin/redfin.c`, swap the file
  for `templates/X.c`, drop `-o/-MQ/-MF/-MD`, add `-fsyntax-only`. Same
  trick with `-DAS_LIB=1` for the `#ifdef AS_LIB` block in `instr_sbl1.c`.

## 2. E2E tests: expected result

The command lines live in the header comments of the machine files
(`hw/fastproto/arm/{redfin,mt6768}/*.c`); both expect `-L images` and the
images in `images/`.

### 2.1 redfin (re-hosting)

- The run reaches its end state in about 20 s (use a 60-90 s timeout). The
  serial output (about 8.3 KB) ends with:
  ```
  QseeLoadServiceImageSyscall Failed
  ASSERT_EFI_ERROR (Status = Not Found)
  ERROR: C90000002:V03000007 I0 6925FAD3-E243-4B69-AD61-3F978C8ABC60
  ASSERT QseeComDxe.c +203: !EFI_ERROR (Status)
  ```
  This is a known open problem (`hw/fastproto/arm/redfin/TODO.md`), not a
  regression.
- Pass criteria:
  1. the normalized serial output is identical to the baseline;
  2. the set of MMIO (device, rw, offset, value) tuples is identical;
  3. the per-PC hook-hit counts is identical - all excluding the noise.
- Known noise, which also appears between two *unmodified* runs:
  - timestamps in the serial output and in QUP TX words (`qcom_qup` off
    0x700);
  - `qcom_mpm2_sleepctr` values;
  - `qcom_spmi_*` off 0x92c;
  - the hit count of "UFS: clear NON_BLOCKING flag" (0x14864298), which
    varies by a factor of 3 between runs, and of `qcom_timer1`.
- The log is only written with `-d fastproto,fastproto_mmio`. UFS
  transactions are QEMU trace events: add `-trace 'ufs_*'`.

### 2.2 mt6768 (re-hosting)

- The guest boots ATF -> LK -> TEE and ends in an **endless fuzz loop**
  (5 MB of serial in 45 s - always use a timeout and stop at a milestone,
  not at wall time).
- Everything up to the first `campaign-1 END` line (805 lines) is fully
  deterministic, **timestamps included**. After the milestone the loop
  body is the same set of distinct lines; compare the *set* of lines, not
  the byte count (the loop rate follows host speed).
- `-d int` changes the guest output (ATF prints 2 lines more) - never mix
  runs with and without it.

### 2.3 edge-coverage replay (Ghidra)

Inputs: a Ghidra project with the target program analyzed, the
`qemu-edge-coverage-<arch>` tool (§1), and one or more fuzzer `.metadata`
files (a single hitmap file or a whole corpus directory; see
`ghidra_edge_coverage.py --help`).  The program is opened via pyghidra
under plain CPython: `GHIDRA_PROJECT_DIR`/`GHIDRA_PROJECT_NAME` env vars
(or `pyghidra.open_project`), program path via `--program` or `$PROG_PATH`.

Expected results (identical numbers = pass; also diff the timing-stripped
`FINAL ...` debug lines against a previous log):
- **aarch64** (e.g. an aarch64 U-Boot binary + one crash hitmap):
  28531 candidate edges, 402 -> 116 -> **54** final edges;
- **arm** (e.g. a 32-bit ARM trustlet binary + one corpus hitmap):
  72299 candidate edges, 1167 -> 629 -> **388** final edges, only 12 of
  the 546 hitmap offsets unexplained;
- **arm, whole corpus** (thousands of `.metadata` files, most with empty
  maps - small non-empty maps would be skipped by the >10-ints rule):
  15377 -> 11024 -> **7476** final edges, ~2% unexplained.

Ghidra notes:
- Ghidra projects are **exclusively locked**: stop any open GUI / ghidra-rpc
  daemon before running the script against the same project.
- The pyghidra version must match the project's version (a project created
  by a newer Ghidra fails with "data created with newer version").
- Use `ghidra-rpc` (start a daemon on the `.gpr`, `load <binary>`,
  `disassemble`/`decompile`/... ) to set up and inspect projects.
  After a daemon restart no program is open: `load` on the same file
  re-opens the already-analyzed program from the project.
- Verify `--comment` output through the decompiler: the `EDGECOV` pre-
  comments show up in the decompiled C.

## 3. Edge-coverage replay tooling (design facts)

- `system/edge-coverage.c`: standalone tool that executes **one**
  instruction and prints the hitmap offsets its control-flow decisions
  produce (`-a` pc, `-i` insn hex bytes, `-f` NZCV flags, `-m` flags mask,
  `-r` register values, `-o` covrec options).  Per-target because register
  numbering and `get_pc` differ; the arm branch writes CPSR (NZCV + T +
  IT bits) via `arm_cpu_gdb_write_register`.
- The **arm-softmmu tool is valid for AArch32 hitmaps recorded by an
  aarch64-softmmu fuzzer**: the hitmap hash is only (pc, edge_id) and the
  condition evaluation (`arm_test_cc`) is shared QEMU code.  Verified
  empirically (~98% of hitmap offsets explained).
- `mt6768_cpu.c` registers its CPU subtype only `#ifdef TARGET_AARCH64`
  (the `cortex-a55` parent does not exist in arm-softmmu).
- `ghidra_edge_coverage/` package (root `ghidra_edge_coverage.py` is a
  thin shim):
  | File | Content |
  |---|---|
  | `cli.py` | argparse docs + orchestration |
  | `core.py` | `Ctx` (arch, hitmap, candidates, hit_edges, tool_cache), hitmap loading, tool replay, compute stage, annotate/summary |
  | `filter.py` | filter stages (successor/predecessor) + their straight-line walks; may only *remove* hit edges |
  | `archs/__init__.py` | arch registry: `detect`/`bind`/`plan_runs`/`tool_flags` |
  | `archs/aarch64.py`, `archs/arm.py` | per-arch: which insns QEMU records and how to compute their outcomes |
  | `archs/arm_shared.py` | condition-code knowledge common to both ARM arches |
- Tool cache: json keyed `"pc|insn|nzcv|regs#mode" -> [offsets]`, with a
  `cache_version` field; caches with another version or other covrec
  options are ignored (warning).  A cache is per target program + tool.
- `--comment` writes `EDGECOV: hit from <src>` pre-comments at every edge
  target (old tags in the range are replaced) and saves the program;
  `--dry-run` rolls the transaction back.

## 4. Session history (compressed)

1. **Re-hosting refactor.** Instrumentation core rewritten (hash table,
   RCU removal, TB flush, per-cpu + wildcard hooks); new `hw/fastproto/`
   tree (table-driven `fp-stub`, templates, per-machine dirs); log masks
   `LOG_FASTPROTO`/`LOG_FASTPROTO_MMIO` in `include/qemu/log.h` +
   `util/log.c`; many stub/hook bug fixes.  Verified: serial/MMIO/hook
   sets identical, plus a throwaway self-test machine covering the paths
   the e2e cannot reach (per-vCPU hooks, runtime add/remove, all
   `fp-stub` modes).
2. **Verification + UFS.** `fp-stub`/template reset fixed; RCU locking in
   the instrument update paths; guest-triggerable NULL deref in `hw/ufs`
   fixed; the REDFIN-specific UFS behaviour moved behind device
   properties (`permissive-uic`, `config-desc`, `boot-lun`), set by
   `redfin.c`.
3. **mt6768 bring-up.** Boots via a machine-local CPU subtype
   (`mt6768_cpu.c`: board MIDR + implementation-defined DSU sysregs,
   `target/arm` untouched) and by *not* porting a reference hook that was
   a no-op there (a `void`-declared callback whose `return true` was
   dropped; see `instr_atf.c`).  Serial output byte-identical to the
   reference up to the fuzz-loop milestone.
4. **Edge-coverage replay.** `ghidra_edge_coverage.py` modularized into
   the `ghidra_edge_coverage/` package (argparse docs, arch registry);
   arm (AArch32 + Thumb/IT-state) support added, including the arm-softmmu
   tool port of `system/edge-coverage.c`.  All e2e numbers in §2.3 were
   reproduced byte-identically after each refactor round.
5. **Thumb cbz/cbnz edge coverage.** `trans_CBZ` (arm32) now records the
   decision like aarch64 (edge id = (Rn != 0), no coverage guard, per
   user request); `arm.py` plans the two cbz runs (T bit set, Rn from
   the T1 encoding) and gained a bugfix: outside-IT Thumb b.cond was
   replayed without the T bit, so the tool decoded it in ARM state -
   correct only for the eq/ne pair, empty for T32 (km e2e: 11
   "expected exactly one offset" warnings gone).  tbb/tbh turned out to
   be recorded all along (store_reg -> target as edge id, indirect like
   bx) - docstring fixed, no code change.  Verified with the arm tool
   (cbz/cbnz taken/fall, IT-gated cbz = 2 offsets) and the e2e: FINAL
   lines byte-identical, candidates 72293 -> 72299 (+6 = the 3 Thumb
   cbz insns), hit edges unchanged; aarch64 e2e unchanged.

## 5. Open ends / TODO

- **GIC wiring bug (user said: NOT done, leave it).** For smp>1 the
  per-CPU timer/PMU/maintenance lines land on SPIs.  Kept via
  `FpGicConfig.legacy_redfin_layout = true` in `redfin.c`.  Fixing it
  changes guest behaviour, so re-baseline the e2e test.
- **Known firmware blocker:** QseeLoadServiceImageSyscall fails (when
  loading the 32-bit cmnlib) -> UEFI assert.  See
  `hw/fastproto/arm/redfin/TODO.md`.
- **Only one hook per PC.** LibAFL's `libafl_qemu_set_breakpoint()` uses
  `add_instrument(pc, -1)` too, so it replaces a fastproto hook at the same
  PC (and `libafl_qemu_remove_breakpoint()` only deactivates it, so the
  fastproto hook is gone for good).  Possible future work: chaining.
- **Not snapshotted:** hook-side state in C statics (`FpHash` contexts,
  counters) is outside the vmstate.
- **Not exercised by the re-hosting e2e tests:** the per-vCPU hook path,
  runtime hook adds, and AArch32/Thumb `fp_return`/`fp_skip_insn`; use a
  self-test machine as in §4.1.
- **mt6768 hook that is deliberately absent** (`instr_atf.c`): adding the
  "keep UART enabled" skip would produce 2 extra ATF lines - re-baseline
  the e2e if you do.
- **mt6768 has no device model beyond the stubs:** anything past the
  current fuzz loop (real RPMB/eMMC, more MMIO) is still the catch-all
  stub.
- Optional follow-ups: make `fp-stub` configurable from the command line;
  a callback mode for `fp-stub`; an `fp_ram_find_replace` common helper
  (currently local in `instr_teei.c`); report all attached LUs in the UFS
  Configuration Descriptor; table-drive the aarch64 `cbz/tbz` planning;
  more filter stages (collision/ambiguity) - see `filter.py`.
- **Bugs outside the scope** (reported, not fixed) - these are the 50
  remaining build warnings:
  - `libafl/syx-snapshot/syx-snapshot.c` uses `size_t` as `void *` for QHT
    keys (4 `-Wint-conversion` warnings) and `g_aligned_alloc` is
    deprecated; `qht_cmp_true`, `ramcopy_alloc`,
    `restore_pages_and_mark_notdirty` have no prototypes; two unused
    `PAGESZ` variables.
  - `target/arm/tcg/translate{,-a64}.c` and `accel/tcg/coverage.c` pass
    incompatible pointer types to `arm_tcg_gen_rec_edge` /
    `tcg_gen_addi_i32` (10 warnings, incl. our `trans_CBZ` call).  Only
    `--disable-werror` keeps this building.
  - `vmstate_ufs_hc` saves `UfsParams` as a raw buffer although it
    contains `char *serial` (a host pointer).  Harmless for in-process
    LibAFL snapshots, wrong for migration.
  - `libafl/cpu.c` has an unused `err`; `libafl/exit.c` and
    `libafl/blkdev.c` export functions without prototypes.

## 6. Lessons learned / dos & don'ts

- **Baselines:** run the baseline **twice** before judging a diff, so you
  know the noise.  Keep baselines outside `/tmp` (§1).  When comparing
  against a reference tree, stop both runs at a guest milestone, not at
  wall time; `timeout` can leave QEMU grandchildren running - kill them
  afterwards.
- **Hook callbacks must be `bool (CPUState *, vaddr, void *)`.** A `void`
  one compiles (warning only) and silently loses `return true`, i.e.
  every skip/return hook becomes a no-op - this exact bug made a reference
  tree behave differently and cost a day of diffing.
- **Includes:** common fastproto code includes `libafl/instrument.h`, so
  it must be compiled per target: it goes into `hw_arch[...]`, never
  `system_ss`.  This QEMU needs `qobject/qlist.h` (not `qapi/qmp/qlist.h`),
  `exec/cputlb.h` for `tlb_flush`, `exec/tb-flush.h` for `tb_flush`,
  `system/hostmem.h` for memory backends; `g_free_rcu` needs
  `struct rcu_head` at offset 0.
- **Name clash:** `OBJECT_DECLARE_SIMPLE_TYPE(X, FP_STUB)` would define an
  `FP_STUB()` cast macro that clashes with the convenience macro - hence
  `FP_STUB_DEV`.
- **Default `-smp maxcpus=8`** means cpus=8 (`machine-smp.c`), but only
  CPU 0 ever runs in the redfin e2e.
- **Efficiency:** after adding/removing files run
  `touch hw/fastproto/meson.build`; use 60-90 s timeouts and
  `-monitor none -serial none </dev/null` for headless runs;
  `-nographic` muxes the monitor onto stdin and quits QEMU on EOF.
- **Editing:** for many-file mechanical edits, python replace scripts with
  `assert old in s` work well.  After replacing a block, re-read the file:
  a stray `}` slipped through twice and only the compiler caught it.
- **Refactoring the replay package:** keep it behaviour-preserving and
  prove it - diff the timing-stripped `FINAL` log lines of a full run
  before/after (§2.3).

# AGENTS.md (repo root): session log + how to rebuild/re-verify

Repo: `/home/oc/qemu-libafl-bridge`, a fork of qemu-libafl-bridge (the QEMU
used by LibAFL). It is not a git repo, so there is no git diff: back up files
before editing them (see §3).

User scope: improve and refactor the **firmware re-hosting** code only:
- `hw/fastproto/**` (machines, stubs, hooks)
- `include/libafl/instrument.h` and `libafl/instrument.c` (PC hooks)
- the hook insertion in `accel/tcg/translator.c`
- `hw/ufs/**` only where the fork's re-hosting mods live (see §5)

Don't touch the other LibAFL modifications (coverage, snapshots, ...), but
report any bugs you find in them (§6).

User rules:
- **Propose changes first**, concise and well outlined, and **wait for
  approval**. Apply them in batches; the user may pick items.
- The code is experimental and hacky on purpose: no unit tests, and QEMU
  style is not required. Aim for readable, concise comments and good
  examples.
- Prefer a **comment** over a code change for cosmetic findings. Small fixes
  and asserts are fine.
- The end-to-end (e2e) test is the redfin example
  (`build_redfin/run_example.sh`). Run it before and after every change.
- For re-hosting knowledge (workflow, APIs, pitfalls), the **user-facing doc
  is `hw/fastproto/AGENTS.md`**. This file is the session/maintenance log:
  keep re-hosting how-to out of it.

## 1. Environment facts

- The user's shell is `oc`. `/tmp/opencode` is NOT writable, and
  **`/tmp/oc` gets wiped without warning** - it happened mid-session and took
  the baselines with it. Keep scratch files, baselines and backups in
  **`/home/oc/oc_verify/`** (persistent, see §3).
- Two build dirs, both working and both verified against the e2e test:
  - `build_fp/` - the working build.
  - `build_redfin/` - reconfigured from scratch in session 2 (it pointed at
    `/home/f/git/...` and `ninja` died with "Permission denied").
    `run_example.sh` lives here and expects `./qemu-system-aarch64` plus
    `../images/`.
- Configure line (`-DREDFIN` was obsolete and has been dropped):
  ```
  mkdir build_fp && cd build_fp && ../configure --target-list=aarch64-softmmu \
    --enable-debug --disable-werror --enable-gcrypt --disable-tools \
    '--extra-cflags=-DDUMMY_TIMERS' --disable-docs
  ninja qemu-system-aarch64
  ```
- Firmware images: `images/` (`pbl_sdm865.bin`, `bootlun.bin`, `sda`, `sde`)
  for redfin, `/tmp/mtk/` (read-only, owned by another user: `atf`, `tee`,
  `lk`, `atags`, `atf_arg_t`, `mtk_bl_param_t`, `sram2`, `CFG`) for mt6768.
- The mt6768 reference tree is `~/qemu-libafl-bridge-mtk` (QEMU 10.0.3, flat
  `hw/fastproto/`, `./run.sh` needs `./qemu-system-aarch64` in its root - that
  binary is a copy of `build/qemu-system-aarch64`).
- `DUMMY_TIMERS` makes the guest clock deterministic (see
  `util/qemu-timer.c`, `target/arm/helper.c`, and WFI in
  `target/arm/tcg/op_helper.c`). UFS I/O is still asynchronous, so
  timestamps and poll counts vary slightly between runs.
- A full build takes about 50 s with 16 cores; incremental builds are
  seconds. `ninja qemu-system-aarch64 2>&1 | grep -E "error:|FAILED"`.
  In session 3 neither build dir existed: `build_fp/` was reconfigured from
  scratch with the line above and re-verified against both e2e tests;
  `build_redfin/` was not recreated.
- Warning baseline after session 2: **50 warnings, 0 errors**, none of them
  in `hw/fastproto/**`, `libafl/instrument.c`, `hw/ufs/**` or `util/log.c`.
  The only warning in a file we touch is `translator.c: label
  'post_translate_insn' defined but not used` (pre-existing, belongs to
  LibAFL's commented-out backdoor block). The rest is listed in §6.
- Syntax-check the templates (never compiled by meson): take the
  `compile_commands.json` entry for `arm/redfin/redfin.c`, swap the file for
  `templates/X.c`, drop `-o/-MQ/-MF/-MD`, add `-fsyntax-only`. Same trick
  with `-DAS_LIB=1` for the `#ifdef AS_LIB` block in `instr_sbl1.c`.

## 2. E2E tests: expected result

### 2.1 redfin

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
  3. the per-PC hook-hit counts are identical - both excluding the noise.
- Known noise, which also appears between two *unmodified* runs:
  - timestamps in the serial output and in QUP TX words (`qcom_qup` off
    0x700);
  - `qcom_mpm2_sleepctr` values;
  - `qcom_spmi_*` off 0x92c;
  - the hit count of "UFS: clear NON_BLOCKING flag" (0x14864298), which
    varies by a factor of 3 between runs, and of `qcom_timer1`.
- The log is only written with `-d fastproto,fastproto_mmio`. UFS
  transactions are QEMU trace events: add `-trace 'ufs_*'`.

### 2.2 mt6768 (added in session 3)

- `./mt6768_cmp.sh` runs both trees (reference and this one) with the
  reference command line and compares. The guest boots ATF -> LK -> TEE and
  ends in an **endless fuzz loop** (5 MB of serial in 45 s - always use a
  timeout and stop at a milestone, not at wall time).
- Everything up to the first `campaign-1 END` line (805 lines) is fully
  deterministic here, **timestamps included**, and identical to the
  reference: md5 `480120255a388712c112bb20de1e0903` of that prefix, both
  trees (`24d4473f...` with the `(0)[<time>]` timestamps stripped).
- After the milestone the loop body is the same 14 distinct lines; compare
  the *set* of lines, not the byte count (the loop rate follows host speed).
- `-d int` changes the guest output (ATF prints 2 lines more) - never mix
  runs with and without it.

## 3. Verification setup (`/home/oc/oc_verify/`, persistent)

| Path | What |
|---|---|
| `run.sh <builddir> <tag> [secs]` | runs the redfin example, writes `logs/<tag>.{serial,log,stdout}`; set `EXTRA="-d fastproto,fastproto_mmio"` |
| `norm.sh <serial>` | strips timings (`B - <n> -`, `Timer Delta`, `Throughput`) |
| `cmp_mmio.py A.log B.log` | set diff of MMIO tuples, noise filtered; parses the old (`qcom_X_mmio1_read: off A sz S val V`) and the current (`name: read off 0xA sz S val 0xV`) format |
| `hooks.sh <log>` | per-PC hook hit counts |
| `logs/ref1`, `logs/ref2` | baseline: tree state *before* session 2 (two runs, so the noise is visible) |
| `logs/fin1`, `logs/fin2` | final session-2 state, from `build_redfin` and `build_fp` |
| `pre2/`, `post2/` | copies of the 15 files session 2 touched, before and after |
| `fptest.c` | throwaway self-test machine, see §5 |
| `redfin_smoke.sh <tag> [secs]` | minimal redfin run (the session-1/2 `run.sh` and its `-d`/MMIO baselines were gone; this only checks the serial end state) |
| `mt6768_run_ref.sh` / `mt6768_run_fp.sh` <tag> [secs] | run one tree; `UNTIL=<grep pattern>` stops at a milestone instead of at wall time |
| `mt6768_norm.sh <serial>` | strip the `(N)[<time>]` timestamps, cut at the 1st `campaign-1 END` |
| `mt6768_cmp.sh [fp tag] [ref tag] [cap]` | the whole mt6768 e2e: run both, diff boot log (with and without timestamps) + the loop line set |
| `pre_mtk_port/` | copies of `hw/fastproto/arm/mt6768/**` and `target/arm/helper.c` from before session 3 |

Recreate anything missing there; do not put it in `/tmp`. Typical use:
```sh
cd /home/oc/oc_verify
EXTRA="-d fastproto,fastproto_mmio" ./run.sh build_fp ref1      # baseline
EXTRA="-d fastproto,fastproto_mmio" ./run.sh build_fp ref2      # noise
# ... change code, rebuild ...
EXTRA="-d fastproto,fastproto_mmio" ./run.sh build_fp new
diff <(./norm.sh logs/new.serial) <(./norm.sh logs/ref1.serial)
python3 cmp_mmio.py logs/new.log logs/ref1.log
diff <(./hooks.sh logs/new.log) <(./hooks.sh logs/ref1.log)
```

## 4. Session 1: refactor of the re-hosting code

Re-verified in session 2 (rebuild + e2e + code review) and correct; the list
is kept short on purpose.

- **Instrumentation core** (`include/libafl/instrument.h`,
  `libafl/instrument.c`, `accel/tcg/translator.c`): rewritten. Fixed:
  translate-time check used `cpu_index` although TBs are shared between
  vCPUs; nondeterministic wildcard/exact precedence; LibAFL instruction and
  breakpoint hooks were emitted twice for a hooked PC (the old mid-TB `break`
  happened after `insn_start`); `add_instrument()` leaked and silently kept
  the old callback; `remove_instrument()` never freed; missing TB flush, so
  hooks added at runtime never fired; missing return in
  `libafl_qemu_remove_instrument`. Now: `qemu_xxhash2`, exact-cpu-then-
  wildcard lookup, `INSTRUMENT_ALL_CPUS`, RCU-safe removal, `DISAS_TOO_MANY`.
- **Log masks** `LOG_FASTPROTO` (1<<24) / `LOG_FASTPROTO_MMIO` (1<<25) in
  `include/qemu/log.h` + `util/log.c`, replacing `LOG_TRACE` (which is always
  on with the "log" trace backend).
- **`#ifdef REDFIN` removed** from `target/arm/cpu.c`; redfin now sets the
  `mp-affinity` CPU property in its `fp_create_cpus` setup callback.
- **New tree `hw/fastproto/`** (`meson.build` globs `common/**` + `<hw_arch
  key>/**`, skips `_*` and `templates/`): `fastproto.h`, `common/fp_common.c`,
  `common/fp_stub.c` (the table driven `fp-stub` device replacing 19
  copy-pasted stub devices), `arm/fp_arm.{h,c}`, `arm/redfin/**`,
  `arm/mt6768/**`, `templates/**`, `README.md`, `AGENTS.md`.
- Device/skeleton bug fixes: endless loops in the hash hooks and in
  `CoreImageLoad`, out-of-bounds accesses (tcsr_wonce, pimem_ramblur), an
  uninitialized `ret` in qcom_rng, an unbounded QUP TX length, properties set
  after realize, a missing NULL terminator in `sysbus_create_varargs`, a
  non-advancing memmem loop in `instr_teei.c`, `-D none` in
  `run_example.sh`.
- Intended log deltas vs. the pre-refactor code: `qcom_prng` fully quiet,
  `qcom_rng` off 4 reads appear, every table hook logs its hit (62 hooked PCs
  instead of 32 - a strict superset).

## 5. Session 2: verification, fixes, UFS properties

**Verification done:** clean full rebuild (0 errors, 50 warnings), all 3
templates and the `-DAS_LIB` path syntax-checked, e2e serial/MMIO/hook sets
identical to the session-1 baseline from *both* build dirs, and a throwaway
self-test machine (`/home/oc/oc_verify/fptest.c`, `-M fptest`) covering what
the e2e cannot reach: per-vCPU hook precedence, cpu filtering, runtime
add/remove, deactivate/reactivate, `fp_skip_insn`, `fp_read/write*`,
`fp_read_str`, all `fp-stub` modes. It found one real bug (reset, below);
after the fixes it is **29/29 with `-smp 1` and `-smp 2`**. To run it again,
copy it to `hw/fastproto/arm/fptest/`, `touch hw/fastproto/meson.build`,
build, run, then delete it.

**Fixed:**
- **`fp-stub` and the device template never reset** (`dc->legacy_reset = fn`
  is not a registration, see §7): RAM-mode reset values were silently never
  applied. `qcom_pimem_ramblur.c` set its reset values in `realize` (lost on
  system reset) and now has a reset handler too.
- **`build_redfin` could not be rebuilt.** Reconfigured from scratch,
  `run_example.sh` preserved and re-verified.
- RCU: `remove/deactivate/reactivate_instrument()` and the update path of
  `add_instrument()` now hold the RCU read lock around the QHT lookup (they
  are called from the main thread too).
- `fp-stub` RAM mode asserts that `off + size` is inside the store;
  `fp_hash_update()` no longer hashes garbage after a failed guest read.
- Comments added: `add_instrument()` from inside a callback should `return
  true` (the TB flush can be synchronous); `instr_invalidate()` uses
  `first_cpu`, so the flush may be deferred; `FpGicConfig.timer_intids` needs
  all 5 entries; ramblur's MMIO window is larger than its register file.

**`hw/ufs`** (diffed against stock QEMU 10.0.3). The REDFIN-specific
behaviour is now behind device properties, all defaulting to the upstream
behaviour, and `redfin.c` sets them explicitly. With them off the firmware
produces no serial output at all, i.e. the gating is real:
- `permissive-uic` - emulate `DME_GET` (TX_FSM_State) / `DME_SET`
  (PA_PWRMode) and never fail a UIC command (was unconditional);
- `config-desc` - answer the Configuration Descriptor query; the blob is now
  built from named offsets and reports LU 0 + the boot LU;
- `boot-lun=<n>` - LU `<n>` is the boot LU (was hard-coded `lun == 7` in
  `hw/ufs/lu.c`).

Also in `hw/ufs`:
- Fixed a **guest-triggerable NULL dereference**: a request to the BOOT well
  known LUN with no boot LU attached crashed QEMU (`u->boot_wlu` was a
  pointer only set by `ufs_add_lu()`). The upstream emulated WLU is back as
  the fallback, plus a `lu == NULL` guard.
- The sysbus port had left the interrupt dead (`pci_irq_*` commented out,
  `u->irq` never initialized). Now `sysbus_init_irq()` + `qemu_set_irq()`,
  a no-op unless a machine wires it (redfin polls).
- 4 unconditional `qemu_log()` debug prints became trace events
  (`ufs_exec_query_attr`, `ufs_read_desc`); use `-trace 'ufs_*'`.
- Dead code removed (`ufs_init_pci()`, commented PCI class bits, a redundant
  `qr.opcode` assignment); the commented-out `pci_dma_*` calls became
  comments explaining the sysbus port; `ufs_exit()` documented as "teardown
  before snapshot load" and no longer frees the qdev-owned IRQ.
- Unchanged on purpose (fork behaviour that is not REDFIN-specific): the
  PCI -> SysBus port itself, `address_space_memory` DMA, the fixed `ufs-bus`
  name, and `vmstate_ufs_hc` (snapshot support).

**Docs:** `hw/fastproto/AGENTS.md` got the device-reset pitfall, the "prefer
a property over a hack in an upstream device" rule, a UFS boot-media recipe
with the three properties, a new symptom->fix row for boot media, and a
rewritten regression-check section (persistent baselines, noise list, what
the e2e does not cover). `hw/fastproto/README.md` lists the UFS knobs.

**Files touched in session 2** (backups in `/home/oc/oc_verify/{pre2,post2}`):
`AGENTS.md`, `hw/fastproto/{AGENTS.md,README.md}`,
`hw/fastproto/common/{fp_stub.c,fp_common.c}`,
`hw/fastproto/templates/sysbus_device_skeleton.c`,
`hw/fastproto/arm/fp_arm.h`,
`hw/fastproto/arm/redfin/{redfin.c,qcom_pimem_ramblur.c}`,
`include/libafl/instrument.h`, `libafl/instrument.c`,
`hw/ufs/{ufs.c,ufs.h,lu.c,trace-events}`.

## 5b. Session 3: mt6768 now runs (second e2e test)

`hw/fastproto/arm/mt6768/` did not boot at all: ATF took a fatal
"Unhandled Exception in EL3" right after `bl31_plat_arch_setup()`. Two
causes, both found by diffing the two trees and by hooking the same PCs in
both (identical ATF control flow, so the bug was not in the hooks):

- **Missing CPU model bits.** The reference branch hacked `target/arm/
  tcg/cpu64.c`: `cortex-a55` got `midr = 0x410FD0F0` plus 16 DSU
  "CLUSTER*" sysregs (`S3_0_c15_c3_0..7`, `S3_0_c15_c4_0..7`, constants,
  `CLUSTERPWRSTAT_EL1 = 0xf<<4`). None of that is in this tree, so ATF's
  `mrs` of `CLUSTERPWRCTLR_EL1` was an undefined instruction. Fixed with a
  **machine-local CPU subtype** `mt6768-a55` (new `mt6768_cpu.c`, parent
  `cortex-a55-arm-cpu`, sets `cpu->midr` + `define_arm_cp_regs()`, needs
  `target/arm/cpregs.h`), used as `default_cpu_type`/`valid_cpu_types`.
  `target/arm` is untouched on purpose: the reference branch changed the
  shared a53/a55/a76 models and redfin runs on a53. Of the two, only the
  sysregs are needed for the e2e output (the boot log is the same with the
  upstream a55 MIDR); the MIDR is kept to stay byte-identical with the
  reference for anything that runs later.
- **One hook that must stay broken for the baseline.** The reference
  `instr_atf.c` hooks `str xzr,[x3,#0x40]` @0x4CE18B84 ("keep UART enabled")
  with a callback declared `void`, so its `return true` is discarded and the
  hook is a no-op: ATF does clear its `uart_base` and stops logging. The
  refactored `FP_SKIP` really skipped it -> 2 extra ATF lines
  (`[rpmb_init] invoked from lk`, `unknown kernel SMC(0x82000103)`). The
  hook is therefore **removed** (comment in `instr_atf.c` explains, one line
  brings the intent back).

Also: `mtk_mcucfg`/`mtk_trng` from the reference branch are covered by two
`FP_STUB_CONST`s (verified: no MMIO difference), and
`hw/fastproto/arm/mt6768/run_example.sh` is the reproducible command line.

**Result:** serial output identical to the reference, byte for byte
including the guest timestamps, up to the first `campaign-1 END`; the
endless loop after it has the same line set. Build baseline unchanged
(50 warnings, 0 errors, none in the files touched).

**Files touched in session 3:** `hw/fastproto/arm/mt6768/{mt6768.c,mt6768.h,
instr_atf.c}`, new `hw/fastproto/arm/mt6768/{mt6768_cpu.c,run_example.sh}`,
`hw/fastproto/{AGENTS.md,README.md}`, `AGENTS.md`. Backups of the before
state: `/home/oc/oc_verify/pre_mtk_port/`. The reference tree was only
touched to build it (`build/`, a copy of the binary for `run.sh`) and is
otherwise unmodified.

## 6. Open ends / TODO

- **A6: GIC wiring bug (user said: NOT done, leave it).** For smp>1, the
  per-CPU timer/PMU/maintenance lines land on SPIs. Kept via
  `FpGicConfig.legacy_redfin_layout = true` in `redfin.c`. Fixing it changes
  guest behaviour, so re-baseline the e2e test.
- **Known firmware blocker:** QseeLoadServiceImageSyscall fails (when
  loading the 32-bit cmnlib) -> UEFI assert. See `arm/redfin/TODO.md`.
- **Only one hook per PC.** LibAFL's `libafl_qemu_set_breakpoint()` uses
  `add_instrument(pc, -1)` too, so it replaces a fastproto hook at the same
  PC (and `libafl_qemu_remove_breakpoint()` only deactivates it, so the
  fastproto hook is gone for good). Possible future work: chaining.
- **Not snapshotted:** hook-side state in C statics (`FpHash` contexts, the
  `halt_after_8` counter) is outside the vmstate.
- **Not exercised by the e2e tests:** the per-vCPU hook path, runtime hook
  adds, and AArch32/Thumb `fp_return`/`fp_skip_insn` (mt6768 does hit
  `fp_skip_insn`-style hooks only through `FP_SET_REG`/`FP_HOOK`; the
  AArch32 path is untested). Use the `fptest` machine from §5.
- **mt6768 hook that is deliberately a no-op** (`instr_atf.c`, §5b): if the
  ATF log goes quiet and you want it back, add
  `FP_SKIP(0x4CE18B84, "keep UART enabled")` - but then re-baseline the
  mt6768 e2e (2 extra ATF lines).
- **mt6768 has no device model beyond the stubs:** the reference branch's
  `mtk_mcucfg`/`mtk_trng` became `FP_STUB_CONST`s; anything past the current
  fuzz loop (real RPMB/eMMC, more MMIO) is still the catch-all stub.
- **R3b (optional, not done):** make `fp-stub` configurable from the command
  line (a regs string property + `-device`, needs dynamic sysbus support).
- Possible follow-ups: a callback mode for `fp-stub` (to replace the
  remaining small custom devices); an `fp_ram_find_replace` common helper
  (currently local in `instr_teei.c`); report all attached LUs in the UFS
  Configuration Descriptor instead of just LU 0 + boot LU.
- **Bugs noticed outside the scope** (reported, not fixed) - these are the
  50 remaining build warnings:
  - `libafl/syx-snapshot/syx-snapshot.c` uses `size_t` as `void *` for QHT
    keys (4 `-Wint-conversion` warnings) and `g_aligned_alloc` is deprecated;
    `qht_cmp_true`, `ramcopy_alloc`, `restore_pages_and_mark_notdirty` have
    no prototypes; two unused `PAGESZ` variables.
  - `target/arm/tcg/translate{,-a64}.c` and `accel/tcg/coverage.c` pass
    incompatible pointer types to `arm_tcg_gen_rec_edge` / `tcg_gen_addi_i32`
    (9 warnings). Only `--disable-werror` keeps this building.
  - `vmstate_ufs_hc` saves `UfsParams` as a raw buffer although it contains
    `char *serial` (a host pointer). Harmless for in-process LibAFL
    snapshots, wrong for migration.
  - `libafl/cpu.c:172` has an unused `err`; `libafl/exit.c` and
    `libafl/blkdev.c` export functions without prototypes.
- **Bugs in the mt6768 reference tree** (`~/qemu-libafl-bridge-mtk`, found
  while porting, *not* copied over):
  - every `hw/fastproto/instr_atf.c` callback is declared `void` while
    `add_instrument()` wants `bool (...)`: the `return true` of the
    "keep UART enabled" hook is dropped, so it never skips its instruction
    (compiles with `-Wincompatible-pointer-types`, see §5b).
  - `target/arm/helper.c`, `dummy_clock_inc_arm()`: the statement
    `void gt_recalc_timer(cpu, i);` is a *declaration*, not a call, so the
    generic timers are never recalculated after a dummy-clock bump. This
    tree has the call; verified that the mt6768 e2e output is the same
    either way (the guest never has a armed generic timer at that point).
  - its `meson.build` compiles every `*.c` in `hw/fastproto/` (both the
    redfin and the mt6768 files) into the arm target - fine, but the
    `get_source_files.sh` + `find` indirection is what the meson glob in
    this tree replaced.

## 7. Lessons learned / dos & don'ts

- **Checking behaviour without git:** copy the originals to
  `/home/oc/oc_verify/<tag>/` first (NOT `/tmp`). Build a baseline binary and
  run it **twice**, so you know the noise before judging a diff.
- **`device_class_set_legacy_reset(dc, fn)`**, never `dc->legacy_reset = fn`:
  the latter compiles but the handler is never called. Don't set reset
  values in `realize` either - they are lost on a system reset.
- **Name clash:** `OBJECT_DECLARE_SIMPLE_TYPE(X, FP_STUB)` would define an
  `FP_STUB()` cast macro that clashes with the convenience macro. Hence
  `FP_STUB_DEV`.
- **Includes:** fastproto headers are included as `"hw/fastproto/..."`; this
  works because of `-iquote <srcroot>`. Common code includes
  `libafl/instrument.h`, so it must be compiled per target: it goes into
  `hw_arch[...]`, never `system_ss`.
- **Headers this QEMU needs:** `qobject/qlist.h` (not `qapi/qmp/qlist.h`),
  `exec/cputlb.h` for `tlb_flush`, `exec/tb-flush.h` for `tb_flush`,
  `system/hostmem.h` for memory backends; `g_free_rcu` needs
  `struct rcu_head` at offset 0.
- **`cpu_memory_rw_debug`** (used by `fp_write`) goes through
  `address_space_rw`. RAM writes invalidate TBs, but ROM writes are DROPPED
  (the ROM *loader* still works: `load_image_targphys` uses the rom blob
  mechanism, which writes with `address_space_write_rom`).
- **Memory priority:** at equal priority the subregion added LAST wins.
  `create_unimplemented_device` uses -1000.
- **Default `-smp maxcpus=8`** means cpus=8 (`machine-smp.c`), but only CPU 0
  ever runs in the redfin e2e (all hook hits are `cpu 0`).
- **Trace events are off by default**; `-d ...` masks are not. Don't use bare
  `qemu_log()` for debug output in a device.
- **Gate firmware-specific behaviour in an upstream device behind a
  property** that defaults to the upstream behaviour, and set it in the
  machine file - see the `ufs` knobs in §5. For a *CPU model* there is no
  property: register a machine-local subtype (parent = the upstream
  `...-arm-cpu` type, override `cpu->midr`, `define_arm_cp_regs()`), see
  `mt6768_cpu.c` and §5b.
- **Hook callbacks must be `bool (CPUState *, vaddr, void *)`.** A `void`
  one compiles (warning only) and silently loses `return true`, i.e. every
  skip/return hook becomes a no-op. Diffing the two trees' ATF control flow
  with identical hooks in both was what proved the port was correct and the
  *reference* was the one behaving oddly (§5b).
- **`-nographic` muxes the monitor onto stdin and quits QEMU on EOF**, which
  cuts a `timeout`ed run short. Keep stdin open, or use `-monitor none`.
- **Comparing two trees:** stop at a guest milestone (`UNTIL=`), not at wall
  time - the loop rate follows the host. `timeout` kills the wrapper script
  and leaves the QEMU grandchild running, so `pkill -f '[q]emu-system-...'`
  after it (7 leaked 8-vCPU VMs once already ruined a whole comparison).
- **Efficiency:** after adding/removing files run
  `touch hw/fastproto/meson.build`; use 60-90 s timeouts and
  `-monitor none -serial none </dev/null` for headless runs.
- **Editing:** for many-file mechanical edits, python replace scripts with
  `assert old in s` work well. After replacing a block, re-read the file:
  stray `}` slipped through twice (session 1 and 2) and only the compiler
  caught them.

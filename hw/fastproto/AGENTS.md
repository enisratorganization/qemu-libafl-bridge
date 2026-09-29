# fastproto: re-hosting firmware on this QEMU (guide for AI agents)

Goal: run a firmware blob for unknown/custom HW in a new machine
(`-M <proj>`), deterministic, later fuzzed with LibAFL. Method: iterative
trial & error; stub/hook just enough HW for the next boot step. Hacky code
is OK (no QEMU style, no unit tests); every hack gets a one-line reason
comment. `fastproto.h`, `common/`, `arm/`, `templates/` are in
`hw/fastproto/`; other paths are relative to the repo root.

## 1. Code map

| Path | Content |
|---|---|
| `fastproto.h` | common API: `FP_LOG`, `FpHook`/`FP_ADD_HOOKS`, `fp_read*/fp_write*/fp_read_str`, `fp_create_cpus`, `FP_ADD_MEMORY`, `fp_load_firmware`, `fp_map_memdev`, `FP_STUB`, `FP_VMSTATE_TAIL`, `FpHash` |
| `arm/fp_arm.h` | ARM: `fp_arg/fp_set_arg/fp_get_reg/fp_set_reg`, `fp_return[_void]`, `fp_skip_insn`, `FP_RET/FP_SET_REG/FP_SKIP`, `fp_arm_cpu_start`, `fp_create_gicv3` |
| `common/fp_stub.c` | `fp-stub`: table-driven MMIO register stub |
| `templates/` | `machine_skeleton.c`, `instr_skeleton.c` (hooks), `sysbus_device_skeleton.c`; compile-ready, never built in place |
| `include/libafl/instrument.h` | PC hook core (`add_instrument`); header comment = contract |
| `arm/redfin/` | example: Pixel 5 (Qualcomm SDM865) boot ROM → XBL → TZ → UEFI, boot media = UFS |
| `arm/mt6768/` | example: MediaTek ATF (BL31) + TEE started from preloaded images |

Where to copy from:
- `redfin/redfin.c`: memory map, ~20 `FP_STUB`s, GIC with custom timer
  INTIDs, UFS, MPIDR via the `fp_create_cpus` setup callback. Hooks: one
  `instr_<stage>.c` per stage. `instr_brom.c`: `FpHash`; `instr_sbl1.c`,
  `instr_tz.c`: skip HW init, fill structs (`ICB_Get_Memmap`);
  `instr_xbl_uefi.c`: force sync UART, MMU state; `qsee_interface.c`: edit a
  boot-image table. Devices: `qcom_qup.c` (UART), `qcom_mpm2_sleepctr.c`
  (counter per read), `qcom_pimem_ramblur.c` (state + reset).
- `mt6768/mt6768.c`: start mid-chain (raw images, x0/x1 boot args,
  `fp_map_memdev`). `instr_atf.c`: SPSR of next stage, keep UART on;
  `instr_teei.c`: patch a lib loaded at runtime (host-RAM find/replace +
  `tb_flush`), `DUMMY_TIMERS` ticks, debug hooks.

## 2. Build

- Every `*.c` under `common/` and `<hw_arch key>/` (`arm` = arm+aarch64) is
  built automatically. Paths starting with `_` are skipped (`_foo.c` =
  disabled).
- Files added/removed: `touch hw/fastproto/meson.build`, then `ninja
  qemu-system-aarch64` (incremental: seconds).
- Configure: `--target-list=aarch64-softmmu --disable-werror
  --extra-cflags=-DDUMMY_TIMERS` (+ `--enable-debug`). `DUMMY_TIMERS`: guest
  clock advances only with guest activity, WFI skips ahead.
- Includes: `"hw/fastproto/arm/fp_arm.h"` (pulls in `fastproto.h`). Put
  prototypes shared by the project files in `arm/<proj>/<proj>.h`.
- Non-ARM firmware: add `<arch>/fp_<arch>.{h,c}` mirroring `fp_arm.h`
  (register get/set, return, skip insn, `FP_RET`-style macros, CPU start,
  IRQ controller). `common/` works unchanged.

## 3. Hooks: contract (must know)

`add_instrument(pc, cpu, cb, opaque)`; `FP_ADD_HOOKS(table)` wraps it with
`cpu = INSTRUMENT_ALL_CPUS`.
- The callback runs **before** the insn at `pc`, on the vCPU thread,
  **without BQL**. Registers/memory are valid and writable.
- `return false`: continue at `pc` (only regs/mem changed).
  `return true`: PC was changed (`fp_return`, `fp_skip_insn`, ...). `true`
  without a PC change = endless re-entry (only to park a CPU on purpose).
- One hook per (pc, cpu); a second one replaces the first (warning). Combine
  actions in one callback. LibAFL breakpoints use the same table: never
  put both on one PC.
- `pc` = guest **virtual** address as executed (after MMU); Thumb: without
  bit 0.
- Table callbacks get the entry as `opaque` (`const FpHook *`:
  `h->name`, `fp_hook_arg(opaque)`, `h->reg`), so tables must be `static`.
  Each hit logs `hook <name> @0x<pc> cpu <n>` (`-d fastproto`).
- Hooks added at runtime flush the TBs themselves, but fire only after the
  vCPU leaves its current TB; from inside a callback, `return true`.
- `fp_write*` invalidates TBs of RAM; writes to ROM regions are **dropped**.
  Patched code via a host pointer → `tb_flush(cs)` yourself.
- Touching devices/timers/IRQs from a hook: `BQL_LOCK_GUARD()` first.
- `fp_read/fp_write` use the current MMU of `cs`. Physical:
  `address_space_*`, `ldq_le_phys(&address_space_memory, pa)`.
- `fp_arg`/`fp_return` follow the vCPU's *current* state (X regs in
  AArch64, R0-R3/LR in AArch32; `fp_return` handles the Thumb bit). Raw
  `env->xregs[]` is AArch64 only.

## 4. Workflow

```
recon → skeleton machine → build → run (timeout, logs) → find blocker
  ↑                                                          ↓
  └──── diff vs last good run ← rebuild ← cheapest fix (§4.6) ┘
```

### 4.1 Recon (static, before code)
Collect, with sources:
- **SoC/vendor:** `strings` (chip names, build paths, version tags). Then
  get its Linux DTS (`arch/arm64/boot/dts/<vendor>/`), TF-A/U-Boot/LK/
  coreboot ports: memory map, UART, GIC, timer, often register semantics.
- **CPU:** core (`cortex-a53`, ...), AArch64/AArch32/Thumb, EL at entry
  (boot ROM: EL3). ID-register reads (`mrs midr_el1`) and
  `mpidr` checks hint at required CPU properties.
- **Load address + entry:** ELF/MBN/FIT headers, vector table (AArch64:
  16 entries every 0x80, 2 KiB aligned), absolute pointers in literal pools
  that point into the image. Disassemble raw blobs at the right base
  (Ghidra/IDA, or `objdump -D -b binary -m aarch64 --adjust-vma=<base>`).
- **Memory map:** SRAM/DRAM/ROM from DTS, linker-like constants, stack
  pointer setup, MMU page tables built by the firmware.
- **Output path:** UART base + TX/status regs, or a printf/log function
  (xrefs to format strings). Output early = biggest debugging win.
- **Boot chain:** who loads what from where (block device, shared-memory
  tables, preloaded by an earlier stage you can skip).
- Name functions as you go (panic, printf, HW init, loader): hook targets.

### 4.2 Skeleton machine
`cp templates/machine_skeleton.c arm/<proj>/<proj>.c`, rename
`machinexyz`, `touch hw/fastproto/meson.build`. Fill in:
- `mc->default_cpu_type`, `fp_create_cpus(ms, setup_cb, NULL)` (CPU 0 runs,
  others off until PSCI). Create CPUs first: the GIC wires them.
- `FpMemRegion` table + `FP_ADD_MEMORY`; DRAM = `machine->ram` (`-m`).
- `fp_load_firmware(machine->firmware /* -bios */, addr, max)`; more images
  by name (searched in `-L` dirs), or ad hoc: `-device
  loader,file=x.bin,addr=0x...,force-raw=on`.
- `fp_arm_cpu_start(qemu_get_cpu(0), 3, entry)` (it resets the CPU), then
  `fp_set_reg` for boot args the previous stage would pass.
- Catch-all `create_unimplemented_device("mmio_catchall", 0, size)`:
  priority -1000, logs unknown MMIO with `-d unimp`; based at 0 its offset =
  physical address.
- `mc->ignore_memory_transaction_failures = true` (unmapped reads 0, no
  abort). Temporarily off to find bad accesses.
- `fp_create_gicv3` once the firmware touches the GIC or timers. New
  machines: never `legacy_redfin_layout`.
- Hooks: one `instr_<stage>.c` per stage from `templates/instr_skeleton.c`,
  registered from machine init.

### 4.3 Run headless
```sh
timeout 60 ./qemu-system-aarch64 -M <proj> -bios fw.bin -smp 1 -m 1G \
  -display none -monitor none -serial file:serial.txt </dev/null \
  -d fastproto,fastproto_mmio,unimp,guest_errors,invalid_mem,int -D fp.log
```
Add as needed:
- `-no-reboot`: stop on guest reset instead of looping;
  `-d cpu_reset`: CPU state at reset.
- `-d exec,nochain -dfilter 0xA..0xB`: executed TBs in a range (heavy;
  keep the range small). `-d in_asm`: translated code.
- Live state: `-monitor unix:mon.sock,server=on,wait=off`, then
  `echo 'info registers' | socat - UNIX-CONNECT:mon.sock`. Useful HMP:
  `info registers`, `info mtree` (effective memory map), `x/16i $pc`,
  `xp /8wx <pa>`.
- gdb: `-s -S`, then `gdb-multiarch -batch -ex 'target remote :1234'
  -ex 'hb *0x...' -ex c -ex 'info reg' -ex 'x/8i $pc'`.
- Stage writes to its own chardev (`qemu_chr_find("id")` in the machine):
  `-chardev file,id=<id>,path=out.txt`.
- UFS: `-trace 'ufs_*'` (trace events, not `-d`).

### 4.4 Find the blocker
1. Last serial line → `grep -abo '<string>' fw.bin` (file offset) → xrefs
   → code location.
2. `tail -n 100 fp.log`:
   - same MMIO read repeating → poll loop: read the disassembly around it,
     stub the expected bit;
   - `Taking exception` + `ESR`/`FAR`/`ELR` repeating → fault loop: FAR =
     bad address, ELR = faulting insn;
   - nothing new → get PC (monitor/gdb), disassemble, identify the loop
     (WFI? spin on a RAM flag set by another core/IRQ?).
3. Bisect "did we get here?" with `FP_TRACE` hooks.
4. Hook panic/assert/error functions: log args + LR (caller) → direct
   pointer to the failing check.

### 4.5 Symptom → fix

| Symptom | Fix |
|---|---|
| Spins reading one MMIO reg (ready/lock/done) | `{off, val}` in an `FP_STUB` (`FP_STUB_QUIET` if hot) |
| Needs a reg to change between reads (counter, toggle) | small device (`qcom_mpm2_sleepctr.c`) |
| Writes a reg, reads it back | `FP_STUB_RAM` |
| Data abort, FAR unmapped | add RAM/ROM/stub; check region size |
| Undef insn / unexpected exception at entry | CPU type, start EL, AArch32 vs 64, CPU props (`cntfrq`, `mp-affinity`) |
| Stuck in `wfi` / waiting for a tick | GIC + timer INTIDs, `DUMMY_TIMERS`, or hook the wait |
| PLL/DDR training/PMIC/clock init never ends | `FP_RET(pc, <success>, "name")` on the function |
| Signature/hash check fails | `FpHash` offload (`instr_brom.c`) or `FP_RET` on verify |
| No output | UART device (`pl011`, `qcom_qup.c`, `mtk_uart.c`) + stub its status reg, or hook printf → `FP_LOG` |
| Reads structs a skipped stage/HW would fill (DDR size, fuses, boot params) | hook the producer, `fp_write` the struct, `fp_return` |
| Hook never fires in a later stage | wrong VA (MMU on/relocated); take PC from `info registers`/gdb |
| Next stage can't be read from flash/eMMC/UFS | reuse upstream device (`ufs`, `sdhci`, `pflash`, ...) + add a property for the missing detail (§5), or hook the loader and `fp_write` the image |
| Next stage runs in the wrong mode/EL | `FP_SET_REG` on the SPSR/ELR value before `eret` (`instr_atf.c`) |
| Firmware disables the UART/console | `FP_SKIP` the write or hook the console setup |

### 4.6 Cheapest stable fix first
1. **`FP_STUB`**: register values only; independent of firmware build and
   code paths.
2. **Hook** (`FP_RET`/`FP_SET_REG`/`FP_SKIP`/`FP_HOOK`): replace a function
   by its contract; tied to one build; always a descriptive name.
3. **Device** (`templates/sysbus_device_skeleton.c`): state, side effects
   (TX, DMA, IRQ), reuse across builds. Search `hw/` for an upstream model
   first.

### 4.7 Iterate
- One change per run; keep the last good `serial.txt`/`fp.log`, diff.
- Comment every stub value/hook (register meaning, why skipped).
- Open blockers → `arm/<proj>/TODO.md`.
- Done = reaches the target state (e.g. the fuzz target function) with
  identical output on repeated runs.

## 5. Recipes

```c
/* CONST: listed regs read val, rest 0 (-d unimp), writes ignored */
FP_STUB("clk", 0x17800000, 0x2000, FP_STUB_CONST, { 0x0, 0x10000000 });
/* RAM: writes stick, list = reset values; .fixed = constant */
FP_STUB("prng", 0x791000, 0x1000, FP_STUB_RAM | FP_STUB_QUIET,
        { 0x0, 0x12345678, .fixed = true });
/* non-4-byte accesses: | FP_STUB_ANY_SIZE (else fail, -d guest_errors) */

static const FpHook stage_hooks[] = {
    FP_RET(0x148243E8, 0, "ddr_training: skip, ret 0"),
    FP_SET_REG(0x14850E30, 3, 0, "pmic_status (x3) = 0"),
    FP_SKIP(0x4CE18B84, "keep UART enabled"),
    FP_TRACE(0x14860000, "reached X"),
    FP_HOOK_ARG(0xFFF007354, set_flag, 0x200, "debug -> UART"),
    FP_HOOK(0x1482C4B8, get_cfg, "get_cfg"),
};
FP_ADD_HOOKS(stage_hooks);                  /* from machine init */

/* Replace a function: fill out-param, return 0 */
static bool get_cfg(CPUState *cs, vaddr pc, void *opaque) {
    fp_write_u32(cs, fp_arg(cs, 1), 0x1234);
    return fp_return(cs, 0);                /* x0/r0 = 0, PC = LR */
}

/* Guest printf -> log (no UART needed) */
static bool log_puts(CPUState *cs, vaddr pc, void *opaque) {
    g_autofree char *s = fp_read_str(cs, fp_arg(cs, 0), 256);
    FP_LOG("guest: %s\n", s);
    return false;                           /* or fp_return_void(cs) */
}

/* Panic/assert: who called? */
static bool on_panic(CPUState *cs, vaddr pc, void *opaque) {
    FP_LOG("panic x0=0x%" PRIx64 " lr=0x%" PRIx64 "\n",
           fp_arg(cs, 0), fp_get_reg(cs, is_a64(cpu_env(cs)) ? 30 : 14));
    return false;
}

/* Per-vCPU hook / plain opaque */
add_instrument(0x80001000, 0 /* cpu 0 */, my_cb, my_opaque);

/* Devices (props BEFORE realize, IRQ = GIC GPIO n = SPI n): machine_skeleton.c */

/* RAM/dump from the command line: -object memory-backend-file,id=x,size=..,mem-path=.. */
fp_map_memdev("x", 0x200000);

/* UFS boot media. Fork properties (hw/ufs/ufs.h), default = upstream:
 *   permissive-uic: accept all UIC/DME cmds (TX_FSM_State, PA_PWRMode)
 *   config-desc:    answer the Configuration Descriptor query
 *   boot-lun=<n>:   LU n is the boot LU (serves the BOOT well known LUN)
 * LUs: -drive file=boot.bin,if=none,id=d7,readonly=on
 *      -device ufs-lu,drive=d7,bus=ufs-bus,lun=7                        */
dev = qdev_new("ufs");
qdev_prop_set_bit(dev, "permissive-uic", true);
qdev_prop_set_bit(dev, "config-desc", true);
qdev_prop_set_int32(dev, "boot-lun", 7);
sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x1d84000);   /* IRQ optional */
```

## 6. Pitfalls

- **Device reset:** `device_class_set_legacy_reset(dc, fn)`.
  `dc->legacy_reset = fn` compiles but is never called. Reset values set in
  `realize` are lost on system reset.
- **Snapshots (LibAFL):** all mutable device state in the vmstate
  (`FP_VMSTATE_TAIL(State, first_member)`, no pointers after it). Hook
  state in C statics (`FpHash`, counters) is not snapshotted.
- **Determinism:** no host time/randomness in stubs/hooks; `DUMMY_TIMERS`.
  Async block I/O (UFS) still shifts timestamps/poll counts slightly.
- **Overlapping MMIO:** at equal priority the region added last wins; check
  `info mtree`.
- **Upstream devices:** firmware-specific behaviour behind a property
  defaulting to upstream, set in the machine file (see `ufs`). No
  unconditional hacks in `hw/<upstream>/`.
- **Log volume:** `-d fastproto_mmio` can reach GBs: `FP_STUB_QUIET` for
  polled regs, `timeout`, `tail`.
- **`-smp`:** secondaries need PSCI/TZ to start; use `-smp 1` for bring-up
  unless the firmware checks the core count.
- **Logging in devices:** `FP_LOG`/`fp_log_mmio` (`-d` gated), never bare
  `qemu_log()`.

## 7. Regression check

For your machine: two baseline runs (outside `/tmp`) to learn the noise,
then diff after each change: timing-stripped serial output, sorted `hook
...` lines, set of MMIO `(dev, rw, off, val)` tuples.


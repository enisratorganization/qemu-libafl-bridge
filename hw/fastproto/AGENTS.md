# AGENTS.md: firmware re-hosting with fastproto

This file is for AI agents (and humans) who use this QEMU fork
(qemu-libafl-bridge + fastproto) to re-host firmware. That means getting
bare-metal firmware or boot chains to run without the real hardware, so they
can later be instrumented and fuzzed with LibAFL.

Everything here is experimental on purpose. Speed of iteration matters more
than clean QEMU code. What matters is that it works, is deterministic, and is
understandable.

---

## 1. Map of the code

| Path | What it is |
|---|---|
| `hw/fastproto/fastproto.h` | Common API: `FP_LOG`, `FpHook` tables, `fp_read/fp_write*`, `fp_create_cpus`, `FP_ADD_MEMORY`, `fp_load_firmware`, `fp_map_memdev`, `FP_STUB`, `FpHash` |
| `hw/fastproto/arm/fp_arm.h` | ARM: `fp_arg/fp_set_reg`, `fp_return`, `fp_skip_insn`, `FP_RET/FP_SET_REG/FP_SKIP`, `fp_arm_cpu_start`, `fp_create_gicv3` |
| `hw/fastproto/common/fp_stub.c` | `fp-stub`, the generic table-driven MMIO register stub |
| `hw/fastproto/arm/redfin/` | Full example: Qualcomm Pixel 5 boot chain (PBL → XBL → TZ → UEFI) |
| `hw/fastproto/arm/mt6768/` | Second example: MediaTek ATF + TEE started from preloaded images |
| `hw/fastproto/templates/` | Skeletons for a machine, a sysbus device and a hook file (not compiled) |
| `include/libafl/instrument.h` | The PC hook mechanism. **Read its header comment.** |
| `build_redfin/run_example.sh` | End-to-end reference run |

Build facts:
- Every `*.c` under `common/` and `<arch>/` is compiled automatically. The
  folder name is a `hw_arch` key: `arm` covers both arm and aarch64.
- Paths starting with `_` are skipped. `templates/` is never compiled.
- After adding or removing files, run `touch hw/fastproto/meson.build` and
  then `ninja`.
- Configure with `--disable-werror`. Add `--extra-cflags=-DDUMMY_TIMERS` for
  deterministic guest time: the virtual clock advances only with guest
  activity, and WFI jumps ahead in time.
- Include headers as `"hw/fastproto/fastproto.h"` or
  `"hw/fastproto/arm/fp_arm.h"`. A project header such as
  `arm/<proj>/<proj>.h` can include those.

---

## 2. How the hooks work (must know)

`add_instrument(pc, cpu, cb, opaque)` registers a callback. The translator
then starts a new TB at `pc` and emits a helper call as the TB's first
operation. The callback runs **before** the instruction at `pc` executes. It
runs on the vCPU thread, **without the BQL** (the big QEMU lock). The guest
registers in `env` are valid at that point.

- `return false`: only registers or memory changed. Execution continues at
  `pc`.
- `return true`: control flow changed (you set a new PC). The vCPU leaves the
  TB via `cpu_loop_exit()`.
  - Returning `true` **without** changing PC re-runs the hook forever. Only
    do that on purpose, e.g. to park a CPU.
- **One hook per (pc, cpu).** A second hook at the same PC replaces the first
  (`fp_add_hooks` prints a warning).
  - LibAFL breakpoints (`libafl_qemu_set_breakpoint`) use the same mechanism,
    so don't place a fastproto hook on a LibAFL breakpoint PC.
- `pc` is a guest **virtual** address: the PC value as it runs, with the MMU
  applied. On Thumb, use the address without bit 0.
- Hooks added while the guest is running flush the TB cache automatically.
  If you patch guest code through a host pointer, call `tb_flush(cs)`
  yourself. `fp_write()` invalidates TBs by itself, but writes to ROM
  regions are dropped.
- Take `BQL_LOCK_GUARD()` before touching devices, timers or IRQs from a
  hook.

With `FpHook` tables, the callback's `opaque` is the table entry itself
(`const FpHook *`). Use `fp_hook_arg(opaque)` or `h->name` to read it. Every
hit is logged as `hook <name> @0x<pc> cpu <n>` when `-d fastproto` is on.

```c
static const FpHook sbl_hooks[] = {
    FP_RET(0x148243E8, 0, "do_ddr_training"),         /* skip, return 0 */
    FP_SET_REG(0x14850E30, 3, 0, "pmic_status = 0"),  /* x3 = 0, continue */
    FP_SKIP(0x4CE18B84, "don't disable UART"),        /* skip one insn */
    FP_TRACE(0x14860000, "reached X"),                /* log only */
    FP_HOOK(0x1482C4B8, ddr_initialize_info, "boot_ddr_initialize_device"),
};
FP_ADD_HOOKS(sbl_hooks);                               /* from machine init */
```

---

## 3. The re-hosting loop (trial & error)

```
 recon ─► minimal machine ─► run+log ─► find blocker ─► stub/hook/device ─┐
                                ▲                                         │
                                └──────────── rebuild, compare ◄──────────┘
```

### 3.1 Recon (before writing code)
Collect:
- **CPU:** core type (e.g. `cortex-a53`), AArch64 or AArch32, and the
  exception level at reset (usually EL3 for a boot ROM).
- **Images:** load addresses and entry points. Sources: ELF/MBN headers,
  vector tables, and absolute addresses in the first few instructions.
- **Memory map:** SRAM/DRAM/ROM. Sources: Linux DTS of the SoC, vendor
  headers, strings, and MMIO constants seen in the disassembly (Ghidra/IDA).
- **UART:** base address and TX register. Getting serial output early is the
  single most useful debugging aid.
- **Boot chain:** which stage loads which image, and how it gets it (block
  device, shared memory tables).

### 3.2 Minimal machine
Copy `templates/machine_skeleton.c` to `arm/<proj>/<proj>.c` and fill in:
- CPU type and count: `fp_create_cpus`. Only CPU 0 runs; the others wait for
  PSCI.
- `FP_ADD_MEMORY` with RAM/ROM regions, and `machine->ram` as DRAM.
- `fp_load_firmware(file, addr, max)` for the image(s).
- `fp_arm_cpu_start(cpu0, 3, entry)`, plus registers the loader would set
  (`fp_set_reg`).
- A low-priority catch-all `create_unimplemented_device("mmio_catchall", 0,
  4G)`, so unknown MMIO is visible (`-d unimp`) instead of silent.
- `mc->ignore_memory_transaction_failures = true`, so unmapped accesses
  read 0 instead of aborting the guest. Turn it off temporarily if you need
  to find bad accesses.
- An interrupt controller (`fp_create_gicv3`) as soon as the firmware
  touches the GIC or uses timers.

### 3.3 Run and observe
```
qemu-system-aarch64 -M <proj> -bios fw.bin -nographic -smp 1 \
  -d fastproto,fastproto_mmio,unimp,guest_errors,int -D /tmp/fp.log \
  [-s -S]                  # gdbstub on :1234, wait for debugger
  [-d in_asm,exec,cpu -dfilter 0x1000..0x2000]   # heavy: narrow it down
```
- Monitor (`-monitor stdio` or Ctrl-A C): `info registers`, `info mtree`
  (the memory map as QEMU sees it), `x/16i $pc`, `xp /8wx 0xADDR`.
- gdb: `gdb-multiarch -ex 'target remote :1234'`, then `add-symbol-file`,
  hardware breakpoints (`hb`), and `display/i $pc`.
- Always compare the serial output and the tail of the log between runs.

### 3.4 Diagnose the blocker (symptom → typical fix)

| Symptom | Likely cause | Fix |
|---|---|---|
| PC spins in a loop reading one MMIO register | Waiting for a ready/lock/done bit | Add `{off, val}` to an `FP_STUB`. The value is usually a single bit; read the disassembly around the poll. |
| Same register expected to change between reads | Status toggles or counts | Small custom device (see `qcom_mpm2_sleepctr.c`, `qcom_pimem_ramblur.c`) |
| Firmware writes a register and reads it back | Scratch/config register | `FP_STUB_RAM` |
| `-d int` shows Data Abort, with FAR in an unmapped area | Missing RAM/ROM or device | Add a memory region or stub. Check the size. |
| Undefined instruction / exception at EL3 | Wrong CPU model, EL or features | Change the CPU type or start EL |
| Stuck in `wfi` | Waiting for an IRQ/timer | Wire the GIC and timers; use `-DDUMMY_TIMERS`; or hook the wait loop |
| Deep HW init (PLL, DDR training, PMIC, clocks) never finishes | Too much HW to model | `FP_RET(pc, success_value, "name")` on the whole function |
| Signature/hash verification fails | Crypto HW, or modified images | `FpHash` offload (`instr_brom.c`), or `FP_RET` on verify |
| No output at all | No UART | Stub the UART's status register + a TX device (`qcom_qup.c`, `mtk_uart.c`), or hook the print function and `FP_LOG` its string |
| Firmware reads config structs filled by earlier HW (DDR size, fuses) | Data normally provided by skipped code | Hook the producer and `fp_write` a struct (`ddr_initialize_info`) |
| Next boot stage has a different MMU/VA layout | Hooks at the wrong addresses | Use the VA the code runs at (check with `info registers`/gdb) |

### 3.5 Pick the least effort that is still stable
1. **MMIO stub** (`FP_STUB`). Use it when the firmware only needs plausible
   register values. It stays correct for all code paths and needs no
   firmware addresses.
2. **Hook** (`FP_RET`/`FP_SET_REG`/`FP_HOOK`). Use it when a whole function
   can be replaced by its contract. It's fast, but tied to one firmware
   build. Always give it a descriptive name.
3. **Custom device** (`templates/sysbus_device_skeleton.c`). Use it when you
   need state, side effects (UART output, DMA, IRQs) or reuse across
   firmware versions. Existing QEMU devices (`pl011`, `ufs`, ...) are often
   good enough. Search `hw/` before writing one.

### 3.6 Iterate
- Make one change per iteration, rebuild, and rerun. Keep the log from the
  last good run and diff against it.
- Write down what you learned (register meaning, why a function is skipped)
  in comments next to the stub entry or hook.
- Track open blockers in `arm/<proj>/TODO.md`.

---

## 4. Recipes

```c
/* Constant registers (unknown offsets read 0, logged with -d unimp) */
FP_STUB("clk", 0x17800000, 0x2000, FP_STUB_CONST, { 0x0, 0x10000000 });

/* RAM-like registers, some fixed (always read the value, ignore writes) */
FP_STUB("prng", 0x791000, 0x1000, FP_STUB_RAM | FP_STUB_QUIET,
        { 0x0, 0x12345678, .fixed = true });

/* Replace a function: fill the out-parameter, return 0 */
static bool get_cfg(CPUState *cs, vaddr pc, void *opaque) {
    fp_write_u32(cs, fp_arg(cs, 1), 0x1234);
    return fp_return(cs, 0);          /* x0/r0 = 0, PC = LR */
}

/* Trace a print function instead of modelling the UART */
static bool log_puts(CPUState *cs, vaddr pc, void *opaque) {
    g_autofree char *s = fp_read_str(cs, fp_arg(cs, 0), 256);
    FP_LOG("guest: %s\n", s);
    return false;                     /* or fp_return_void(cs) to skip it */
}

/* Per-vCPU / plain opaque: use the raw API */
add_instrument(0x80001000, 0 /* cpu 0 only */, my_cb, my_opaque);

/* Park a CPU at a PC for debugging (endless re-entry on purpose) */
static bool park(CPUState *cs, vaddr pc, void *opaque) { return true; }

/* Device with properties: set them BEFORE realize */
DeviceState *d = qdev_new("qcom_qup");
qdev_prop_set_chr(d, "chardev", qemu_chr_find("qup"));
sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_fatal);
sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, 0x888000);
sysbus_connect_irq(SYS_BUS_DEVICE(d), 0, qdev_get_gpio_in(gic, SPI_NR));

/* Extra RAM from the command line: -object memory-backend-file,id=x,... */
fp_map_memdev("x", 0x200000);
```

---

## 5. Pitfalls

- **One hook per PC.** Combine several actions in one callback.
- **`FpHook` tables must be `static`**, because their entries are the
  callbacks' opaque pointer.
- **AArch32 vs AArch64:** `fp_arg`/`fp_set_reg`/`fp_return` follow the
  vCPU's *current* state. Raw `env->xregs[]` is only valid in AArch64.
- **`fp_read`/`fp_write` use the current MMU context of `cs`.** Use
  `address_space_*`/`ldq_le_phys` for physical addresses.
- **`fp-stub` accepts 4-byte accesses only by default.** Other sizes fail
  silently (they read 0 because failures are ignored). Use
  `FP_STUB_ANY_SIZE` and check `-d guest_errors`.
- **Overlapping MMIO at equal priority:** the region added last wins. Put
  catch-alls at low priority (`create_unimplemented_device` uses -1000).
  Check with `info mtree`.
- **Order in machine init:** create CPUs before the GIC (it wires the
  CPUs), and set device properties before realize.
- **Determinism** is required for fuzzing and for diffing runs. Don't use
  host time or randomness in stubs or hooks; use `-DDUMMY_TIMERS`.
  Asynchronous backends (UFS/block I/O) can still shift timestamps in the
  output.
- **Snapshots:** LibAFL restores devices through their vmstate. All mutable
  device state must be in the vmstate: `FP_VMSTATE_TAIL(State, first_member)`
  for plain members, no pointers. Hook state kept in C statics (e.g.
  `FpHash`) is *not* snapshotted.
- **Log volume:** `-d fastproto_mmio` can produce gigabytes. Use
  `FP_STUB_QUIET` for polled registers, or narrow the logging down.
- **`-smp`:** secondary CPUs start powered off and need PSCI/TZ to wake them.
  Use `-smp 1` for early bring-up.
- **Known redfin quirk:** its GIC uses `legacy_redfin_layout`. For smp>1 the
  CPU timer and PMU PPIs are wired to SPIs; this is only kept so the
  reference run doesn't change. New machines must not use it (see
  `FpGicConfig`).

---

## 6. Regression check (before/after refactors)

1. `ninja -C <build> qemu-system-aarch64`.
2. Run `build_redfin/run_example.sh`. It stops at the known QseeComDxe
   assert after about 20 s; stop it with a timeout.
3. Compare `/tmp/qup_serial_out.txt` with a known good copy, after stripping
   the timing numbers (`B - <n> -`, `Delta`, `Throughput`).
4. Optionally diff the sorted sets of `hook ...` and MMIO lines in the log.
   Sleep-counter values and UART timestamp bytes vary between runs.

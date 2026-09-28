# fastproto: fast prototyping of machines for firmware re-hosting

This folder is for hacking together machines (boards), MMIO device stubs and
firmware hooks quickly, without following all of QEMU's coding rules. The
result can be fuzzed with LibAFL (system mode) later.

The re-hosting workflow (trial & error) is described in [AGENTS.md](AGENTS.md).

## Layout

```
fastproto.h            common API: logging, hook tables, guest memory,
                       machine setup, fp-stub, hash offloading
common/                arch independent code (fp-stub device, helpers)
arm/fp_arm.{h,c}       ARM helpers: registers/return, GICv3, CPU start
arm/redfin/            example: Pixel 5 (Qualcomm) boot ROM -> UEFI
arm/mt6768/            example: MediaTek ATF + TEE
templates/             copy&paste skeletons (never compiled)
```

## Build

- Every `*.c` below `common/` and `<arch>/` (arch = key of `hw_arch` in
  meson, e.g. `arm`, `riscv`) is compiled automatically for that target.
- Paths starting with `_` are skipped: rename `foo.c` -> `_foo.c` to disable.
- New/removed files: `touch hw/fastproto/meson.build` (meson re-globs).
- Configure with `--disable-werror` (prototype code is allowed to be sloppy).
  `--extra-cflags=-DDUMMY_TIMERS` makes guest time deterministic.

## Building blocks (see `fastproto.h` / `arm/fp_arm.h`)

| Need                                   | Use                                   |
|----------------------------------------|---------------------------------------|
| register block returning constants     | `FP_STUB(name, base, size, FP_STUB_CONST, {off, val}, ...)` |
| RAM-like registers                     | `FP_STUB(..., FP_STUB_RAM, ...)`      |
| device with behaviour (UART, counters) | `templates/sysbus_device_skeleton.c`  |
| skip a firmware function               | `FP_RET(pc, retval, "name")`          |
| patch a register before an insn        | `FP_SET_REG(pc, reg, val, "name")`    |
| arbitrary C at a PC                    | `FP_HOOK(pc, callback, "name")`       |
| RAM/ROM, images, CPUs, GIC             | `FP_ADD_MEMORY`, `fp_load_firmware`, `fp_create_cpus`, `fp_create_gicv3` |

Hooks are built on `include/libafl/instrument.h` (read its header comment).

## Logging

```
-d fastproto          hook hits ("hook <name> @pc cpu n") + FP_LOG()
-d fastproto_mmio     every stub/device register access
-d unimp,guest_errors unlisted stub registers, unimplemented devices,
                      invalid accesses
-D file.log           write the log to a file
```

## Example

`build_redfin/run_example.sh` boots the redfin firmware up to the UEFI
QseeComDxe assert (see `arm/redfin/TODO.md`). Use it as end-to-end test.

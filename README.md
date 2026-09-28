# MODS

- Includes `hw/fastproto` for fast and efficient prototyping of HW ("firmware re-hosting")
- Incudes "TCG inline edge coverage recording" 
  - `./include/exec/coverage.h` etc.
  - inline mods inside translator, e.g. in `target/arm/tcg/translate-a64.c`

# QEMU LibAFL Bridge

This is a patched version of [QEMU](https://gitlab.com/qemu-project/qemu) that exposes an interface for LibAFL-based fuzzers.

This raw interface is used in `libafl_qemu` that expose a more Rusty API.

To use `libafl_qemu`, refer to the [LibAFL](https://github.com/AFLplusplus/LibAFL) repository, especially the [qemu](https://github.com/AFLplusplus/LibAFL/tree/main/fuzzers/qemu) fuzzer example.

#### License

<sup>
This project extends the QEMU emulator, and our contributions to previously existing files adopt those files' respective licenses; the files that we have added are made available under the terms of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later version.
</sup>

<br>

# RUN mt6768

Example cmd:
```
./qemu-system-aarch64 -machine mt6768 -smp maxcpus=8 -object memory-backend-file,id=config_area,size=4096B,share=off,rom=off,readonly=on,mem-path=CFG  -object memory-backend-file,id=sram2,size=458752B,share=off,rom=off,readonly=on,mem-path=YOUR_PATH/sram2 -chardev file,id=uart0,path=/tmp/qemulog -nographic -covrec edge_elem_sz=1,edge_elems=8192,edge_enable -L ~/git/mtk-microtrust-fuzzer/fuzzer/files/
```
```
ls ~/git/mtk-microtrust-fuzzer/fuzzer/files/
atags  atf  atf_arg_t  CFG  el1.elf  fiasco_withsyms.elf  lk  mtk_bl_param_t  sram2  tee
```

# QEMU LibAFL Bridge

This is a patched version of [QEMU](https://gitlab.com/qemu-project/qemu) that exposes an interface for LibAFL-based fuzzers.

This raw interface is used in `libafl_qemu` that expose a more Rusty API.

To use `libafl_qemu`, refer to the [LibAFL](https://github.com/AFLplusplus/LibAFL) repository, especially the [qemu](https://github.com/AFLplusplus/LibAFL/tree/main/fuzzers/qemu) fuzzer example.

#### License

<sup>
This project extends the QEMU emulator, and our contributions to previously existing files adopt those files' respective licenses; the files that we have added are made available under the terms of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later version.
</sup>

<br>

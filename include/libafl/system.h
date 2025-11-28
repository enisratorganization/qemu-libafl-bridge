#pragma once

#include "hw/core/cpu.h"
#include "system/block-backend.h"

int libafl_qemu_set_hw_breakpoint(vaddr addr);
int libafl_qemu_remove_hw_breakpoint(vaddr addr);

void libafl_qemu_init(int argc, char** argv);

/**
 * @brief Make qemu virtual clock go super slooow 
 * (no more timer interrupts which hamper coverage stability)
 */
void libafl_warp_clock_reset();
void libafl_warp_clock_set_inc(int64_t val);

/** Write to a block device with aio API
 * The same way the guest would, 
 * thus this writes to the Syx COW cache (if it is initialized)
 */
int libafl_blk_write(BlockBackend *blk, void *buf, int64_t offset, int64_t sz);
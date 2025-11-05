#pragma once

#include "qemu/osdep.h"
#include "exec/cpu-defs.h"
#include "qemu/qht.h"
#include "exec/vaddr.h"
#include "hw/core/cpu.h"
#include "stdbool.h"

/**
 * Return true iff cpu_loop_exit_...(...) should be called.
 * This means, if you only change GP registers or flags or write Memory
 * without changing Control Flow, you MUST return false.
 * The current TCG TB will continue.
 * 
 * If you change Control Flow (e.g. ealry return from subroutine or exceptions), 
 * then you MUST return true.
*/
typedef bool (*InstrumentCallback) (CPUState *cs, vaddr pc, void *opaque);

void libafl_qemu_handle_instrument(CPUArchState *env);

/*
 * Is the PC for a given vCPU really instrumented?
 * Quick check to make while translating
 */
bool check_instrument(vaddr pc, int cpu_index);
bool call_instrument_cb(CPUState *cs, vaddr pc);

/* Add a instrumentation "breakpoint" to be compiled into intermediate TCG
 * cpu_index = -1 matches all vCPUs
 */
bool add_instrument(vaddr pc, int cpu_index, InstrumentCallback cb, void *opaque);

bool deactivate_instrument(vaddr pc, int cpu_index);
bool reactivate_instrument(vaddr pc, int cpu_index);

bool remove_instrument(vaddr pc, int cpu_index);

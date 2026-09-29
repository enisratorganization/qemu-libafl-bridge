/*
 * PC-based instrumentation ("instrument breakpoints")
 *
 * Register a C callback for a guest PC. While translating, a TB is split so
 * that every instrumented PC starts its own TB, and a helper call is emitted
 * as the very first op of that TB. At runtime the helper looks up and calls
 * your callback *before* the guest instruction at @pc executes.
 *
 * Inside the callback you may freely read/modify vCPU registers and guest
 * memory, exactly like in a TCG helper (env->pc / env->regs[15] is valid).
 *
 * Callback contract:
 *  - return false: registers/memory may have changed, but NOT control flow.
 *    Execution continues with the instruction at @pc.
 *  - return true:  control flow changed (PC set, exception raised, ...).
 *    The vCPU leaves the TB via cpu_loop_exit() and continues at the new PC.
 *    NOTE: returning true without changing PC re-enters the same hook
 *    immediately -> endless loop (only useful as a deliberate "halt here").
 *
 * Caveats:
 *  - Callbacks run on the vCPU thread WITHOUT the BQL held. Take BQL
 *    (BQL_LOCK_GUARD()) before touching devices, timers, IRQs, ...
 *    Locking between vCPUs sharing state is up to the user.
 *  - At most one callback per (pc, cpu_index). A vCPU-specific entry wins
 *    over an INSTRUMENT_ALL_CPUS entry for the same PC.
 *  - PCs are guest *virtual* addresses (as seen in the PC register).
 */

#pragma once

#include "qemu/osdep.h"
#include "exec/cpu-defs.h"
#include "exec/vaddr.h"
#include "hw/core/cpu.h"

/* cpu_index wildcard: instrument the PC on every vCPU */
#define INSTRUMENT_ALL_CPUS (-1)

/* See contract above. @opaque is the pointer given to add_instrument(). */
typedef bool (*InstrumentCallback)(CPUState *cs, vaddr pc, void *opaque);

/*
 * Register (or replace the callback of) an instrument for @pc.
 * Can be called at any time, also from within a callback. If code at @pc may
 * already be translated, the TB cache is flushed so the hook takes effect.
 * Returns true if newly added, false if an existing entry was updated.
 *
 * When adding from within a callback, prefer `return true` from that callback
 */
bool add_instrument(vaddr pc, int cpu_index, InstrumentCallback cb,
                    void *opaque);

/* Remove an instrument. Returns false if it did not exist. */
bool remove_instrument(vaddr pc, int cpu_index);

/*
 * Cheaply toggle an instrument without removing it (no TB flush involved).
 * Returns false if it did not exist.
 */
bool deactivate_instrument(vaddr pc, int cpu_index);
bool reactivate_instrument(vaddr pc, int cpu_index);

/* ---- Internal: used by the translator and the TCG helper ---- */

/* Translation time: is there any instrument (any vCPU, any state) at @pc? */
bool check_instrument(vaddr pc);

/* Runtime: call the matching enabled callback; returns its result. */
bool call_instrument_cb(CPUState *cs, vaddr pc);

/* Body of the TCG helper emitted at the start of an instrumented TB. */
void libafl_qemu_handle_instrument(CPUArchState *env);

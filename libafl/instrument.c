/*
 * PC-based instrumentation ("instrument breakpoints").
 * See include/libafl/instrument.h for the API contract.
 *
 * Entries live in a QHT keyed by (pc, cpu_index). Lookups happen under the
 * RCU read lock held by cpu_exec(), so removed entries are freed via RCU.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/qht.h"
#include "qemu/rcu.h"
#include "qemu/xxhash.h"

#include "cpu.h"
#include "libafl/instrument.h"
#include "libafl/cpu.h"

typedef struct InstrBreakpoint {
    struct rcu_head rcu;        /* must be first for g_free_rcu() */
    vaddr pc;
    int cpu_index;              /* INSTRUMENT_ALL_CPUS matches every vCPU */
    InstrumentCallback cb;
    void *opaque;
    bool enabled;
} InstrBreakpoint;

static struct qht instr_htable;

/* Set as soon as the translator queried us: from then on TBs may exist. */
static bool instr_translation_started;

static inline uint32_t instr_hash(vaddr pc)
{
    return qemu_xxhash2(pc);
}

/* qht compare functions: @obj is the table entry, @userp the search key */
static bool instr_match_exact(const void *obj, const void *userp)
{
    const InstrBreakpoint *a = obj;
    const InstrBreakpoint *b = userp;

    return a->pc == b->pc && a->cpu_index == b->cpu_index;
}

static bool instr_match_pc(const void *obj, const void *userp)
{
    const InstrBreakpoint *a = obj;
    const InstrBreakpoint *b = userp;

    return a->pc == b->pc;
}

static InstrBreakpoint *instr_lookup(vaddr pc, int cpu_index)
{
    InstrBreakpoint key = { .pc = pc, .cpu_index = cpu_index };

    return qht_lookup(&instr_htable, &key, instr_hash(pc));
}

/* Make sure already translated code at @pc picks up a new instrument. */
static void instr_invalidate(vaddr pc)
{
    if (qatomic_read(&instr_translation_started) && first_cpu) {
        /* @TODO called from first cpu */
        libafl_breakpoint_invalidate(first_cpu, pc);
    }
}

/* ---- Translation time / runtime ---- */

bool check_instrument(vaddr pc)
{
    /*
     * Deliberately ignores cpu_index and the enabled state: TBs are shared
     * between vCPUs and (de)activation must work without re-translation.
     * Filtering happens at runtime in call_instrument_cb().
     */
    InstrBreakpoint key = { .pc = pc };

    if (unlikely(!qatomic_read(&instr_translation_started))) {
        qatomic_set(&instr_translation_started, true);
    }
    return qht_lookup_custom(&instr_htable, &key, instr_hash(pc),
                             instr_match_pc) != NULL;
}

bool call_instrument_cb(CPUState *cs, vaddr pc)
{
    /* vCPU-specific entry first, then the wildcard */
    InstrBreakpoint *b = instr_lookup(pc, cs->cpu_index);

    if (b == NULL) {
        b = instr_lookup(pc, INSTRUMENT_ALL_CPUS);
    }
    if (b == NULL || !qatomic_read(&b->enabled)) {
        return false;
    }
    smp_rmb(); /* pairs with smp_wmb() in add_instrument() */
    return qatomic_read(&b->cb)(cs, pc, qatomic_read(&b->opaque));
}

void libafl_qemu_handle_instrument(CPUArchState *env)
{
    CPUState *cpu = env_cpu(env);

    /*
     * The helper call is always the first op of its TB, so the PC in env is
     * up to date and no state restore from the TB is required.
     */
    vaddr pc = cpu->cc->get_pc(cpu);

    if (unlikely(call_instrument_cb(cpu, pc))) {
        /* Callback changed control flow: leave the TB */
        cpu_loop_exit(cpu);
    }
    /* Otherwise continue with the TB, just like after any helper call */
}

/* ---- Public API ---- */

bool add_instrument(vaddr pc, int cpu_index, InstrumentCallback cb,
                    void *opaque)
{
    InstrBreakpoint *b = g_new0(InstrBreakpoint, 1);
    InstrBreakpoint *existing = NULL;

    b->pc = pc;
    b->cpu_index = cpu_index;
    b->cb = cb;
    b->opaque = opaque;
    b->enabled = true;

    WITH_RCU_READ_LOCK_GUARD() {
        if (!qht_insert(&instr_htable, b, instr_hash(pc),
                        (void **)&existing)) {
            /* Already registered: update in place (TBs already call us) */
            g_free(b);
            qatomic_set(&existing->enabled, false);
            smp_wmb();
            qatomic_set(&existing->cb, cb);
            qatomic_set(&existing->opaque, opaque);
            smp_wmb();
            qatomic_set(&existing->enabled, true);
            return false;
        }
    }

    instr_invalidate(pc);
    return true;
}



bool remove_instrument(vaddr pc, int cpu_index)
{
    WITH_RCU_READ_LOCK_GUARD() {
        InstrBreakpoint *b = instr_lookup(pc, cpu_index);

        /*
         * No TB flush needed: stale helper calls simply find no entry.
         * Free via RCU since a vCPU may be looking at the entry right now.
         */
        if (b != NULL && qht_remove(&instr_htable, b, instr_hash(pc))) {
            g_free_rcu(b, rcu);
            return true;
        }
    }
    return false;
}

static bool instr_set_enabled(vaddr pc, int cpu_index, bool enabled)
{
    WITH_RCU_READ_LOCK_GUARD() {
        InstrBreakpoint *b = instr_lookup(pc, cpu_index);

        if (b == NULL) {
            return false;
        }
        qatomic_set(&b->enabled, enabled);
    }
    return true;
}

bool deactivate_instrument(vaddr pc, int cpu_index)
{
    return instr_set_enabled(pc, cpu_index, false);
}

bool reactivate_instrument(vaddr pc, int cpu_index)
{
    return instr_set_enabled(pc, cpu_index, true);
}



/* Runs before main(), so instruments can be added from any init code */
static void __attribute__((constructor)) instrument_init(void)
{
    qht_init(&instr_htable, instr_match_exact, 1 << 11, QHT_MODE_AUTO_RESIZE);
}

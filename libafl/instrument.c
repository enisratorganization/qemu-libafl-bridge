#include "libafl/instrument.h"
#include "tcg/tcg.h"
#include "tcg/tcg-op.h"
#include "tcg/tcg-temp-internal.h"
#include "sysemu/runstate.h"

#include "cpu.h"
#include "libafl/cpu.h"

#ifdef CONFIG_USER_ONLY
#define THREAD_MODIFIER __thread
#else
#define THREAD_MODIFIER
#endif

typedef struct  {
	vaddr pc;
	int cpu_index;	/* -1 = matches all vCPU IDs */
	InstrumentCallback cb;
	void *opaque;
} InstrBreakpoint;

struct qht htable = {0};

#define QHT_PC_HASH(pc) ((uint32_t)(pc>>1))

int libafl_qemu_set_instrument(target_ulong pc) { return 1; }

int libafl_qemu_remove_instrument(target_ulong pc)
{
    remove_instrument(pc, -1);
}

void libafl_qemu_handle_instrument(CPUArchState *env) {
	CPUState* cpu = env_cpu(env);
	
	/** When generating the call to libafl_qemu_handle_instrument_helper, 
	 * we took care of the following: The call will always be first in a TB. 
	 * Thus the PC value should be good, and we do not need to restore the CPU state from TB.
	 */
	vaddr pc = cpu->cc->get_pc(cpu);

	if( cpu->last_instrumented_pc_addr != pc ){
		if( unlikely(call_instrument_cb(cpu, pc)) ) {
			/* Instrument hook indicates state has changed */
			cpu_loop_exit(cpu);
		}
	} else {
        cpu->last_instrumented_pc_addr = -1;
    }
}

static bool pc_is_instrumented(const void *p, const void *d) {
	const InstrBreakpoint *a = p;
    const InstrBreakpoint *b = d;

	if( a->pc == b->pc ){
		if( a->cpu_index == -1 ||  a->cpu_index == b->cpu_index )
			return true;
	}
	return false;
};

static bool cmp(const void *ap, const void *bp)
{
	const InstrBreakpoint *a = ap;
    const InstrBreakpoint *b = bp;

	if( a->pc == b->pc && a->cpu_index == b->cpu_index){
		return true;
	}
	return false;
};

bool check_instrument(vaddr pc, int cpu_index) {
	if(htable.map != NULL){
		InstrBreakpoint desc;
		desc.pc = pc;
		desc.cpu_index = cpu_index;

		InstrBreakpoint *b = qht_lookup_custom(&htable, &desc, QHT_PC_HASH(pc), pc_is_instrumented);
		return b != NULL;
	} else{
		return false;
	}
};

bool call_instrument_cb(CPUState *cs, vaddr pc) {
	if(htable.map != NULL){
		InstrBreakpoint desc;
		desc.pc = pc;
		desc.cpu_index = cs->cpu_index;

		InstrBreakpoint* b =
			qht_lookup_custom(&htable, &desc, QHT_PC_HASH(pc), pc_is_instrumented);

		if (b != NULL) {
			cs->last_instrumented_pc_addr = pc;
			return b->cb(cs, pc, b->opaque);
		} else {
			return false;
		}
	} else{
		return false;
	};
};

/** Add a instrumentation "breakpoint" to be compiled into intermediate TCG
 * 	cpu_index = -1 matches all vCPUs
 */
bool add_instrument(vaddr pc, int cpu_index, InstrumentCallback cb, void *opaque) {
	InstrBreakpoint *b = malloc(sizeof(InstrBreakpoint));

	b->pc = pc;
	b->cpu_index = cpu_index;
	b->cb = cb;
	b->opaque = opaque;

	void *existing = NULL;
	qht_insert(&htable, (void *) b, QHT_PC_HASH(pc), &existing);

	return existing == NULL;
};

bool remove_instrument(vaddr pc, int cpu_index) {
	InstrBreakpoint b;

	b.pc = pc;
	b.cpu_index = cpu_index;

	void *ht_elem = qht_lookup(&htable, &b, QHT_PC_HASH(pc));
	if( ht_elem != NULL ){
		qht_remove(&htable, ht_elem, QHT_PC_HASH(pc));
		return true;
	}
	return false;
};

__attribute__ ((constructor)) 
void init_instrument_htable(void) {
	qht_init(&htable, &cmp, 1<<11, QHT_MODE_AUTO_RESIZE);
};





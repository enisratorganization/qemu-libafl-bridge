
#include <unistd.h>
#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "exec/cpu-common.h"
#include "exec/coverage.h"
#include "tcg/coverage-tcg.h"

#define HELPER_H  "tcg/coverage-tcg-helper.h"
#include "exec/helper-info.c.inc"
#undef  HELPER_H

#if defined(__x86_64__) || defined(__i386__)
 #include <x86intrin.h>
#endif

__thread void *current_disasctx=0; 

static inline uint32_t crc32_i64(uint32_t total, uint64_t in) {
#if (defined(__x86_64__) || defined(__i386__))
 	return (uint32_t)_mm_crc32_u64(total, in);
#else
	#error "TODO: Define CRC32 when no _mm_crc32_u64 instruction (x86) is available"
#endif
}

/**
 * @brief Get index for the CMP Coverage hitmap from the CPU state and the JIT-compiletime pc_diff.
 * 
 */
static inline uint32_t get_index_i64(CPUState *cpu, uint64_t pc_diff, uint32_t edgeid) {
	return crc32_i64( edgeid, cpu->cc->get_pc(cpu) + pc_diff ) & cpu->neg.coverage_rec.comp_rec.mask;
}


/** chop size to generate pseudo-edges for
 *  1=classical "compcov", 2=nibbles (could be advantageous), 4=2-bit groups (probably useless)
*/
#define CMP_CHOPS_PER_BYTE 2
#define CMP_CHOPS_SHIFT (8/CMP_CHOPS_PER_BYTE)
#define CMP_CHOPS_MASK ((1<<CMP_CHOPS_SHIFT) -1)

/**
 * @brief Generate pseudo "edges" for each matching position of a CMP insn.
 * Using all bit positions is too fine grained, using byte (8-bit) groups is not much advantegeous.
 * Thus use nibble-grained pseudo-edges (4-bit group).
 */
static inline void gen_cmp_edges(CPUState *cpu, uint64_t pc_diff, int how_many_bytes, uint64_t a1, uint64_t a2) {
	uint32_t cmp_idx = get_index_i64(cpu, pc_diff, 0);

	uint32_t ctr = 0;
	uint32_t trailing_zeroes = 0;	
	for (int i = 0; i < how_many_bytes*CMP_CHOPS_PER_BYTE; i++)
	{
		uint32_t eq = ((a1 & CMP_CHOPS_MASK) == (a2 & CMP_CHOPS_MASK));
		ctr += eq;
		trailing_zeroes += eq;
		trailing_zeroes &= ~((((a1 & CMP_CHOPS_MASK) == 0) && eq) - 1);
		a1 >>= CMP_CHOPS_SHIFT;
		a2 >>= CMP_CHOPS_SHIFT;
	}

	((uint8_t*)cpu->neg.coverage_rec.comp_rec.rec_buf_hitmap)[ cmp_idx ] += ctr;

	// generate more edge hits for the scheduler (LibAFL) to prefer testcases.
	// trailing zeroes are too common in numerical compares. Do not pollute edge map with them
	for (uint32_t i = 1; i < ctr - trailing_zeroes; i++) {
		((uint8_t *)cpu->neg.coverage_rec.comp_rec.rec_buf_hitmap)[get_index_i64(cpu, pc_diff, i << 27)] += ctr - trailing_zeroes;
	};
}

/**
 * @brief Helper to record equal byte values of up to 64-bit "compare" (or SUB/SUBS) operations.
 * It then uses the hashed PC as an index into a hasmap and increments that entry.
 * 
 */
void HELPER(record_cmp_i64_u8_mo8)(CPUArchState *env, uint64_t pc_diff, uint64_t a1, uint64_t a2)
{
    CPUState *cpu = env_cpu(env);

	gen_cmp_edges(cpu, pc_diff, 1, a1, a2);
}

void HELPER(record_cmp_i64_u8_mo16)(CPUArchState *env, uint64_t pc_diff, uint64_t a1, uint64_t a2)
{
	CPUState *cpu = env_cpu(env);

	gen_cmp_edges(cpu, pc_diff, 2, a1, a2);
}


void HELPER(record_cmp_i64_u8_mo32)(CPUArchState *env, uint64_t pc_diff, uint64_t a1, uint64_t a2)
{
	CPUState *cpu = env_cpu(env);

	gen_cmp_edges(cpu, pc_diff, 4, a1, a2);
}

void HELPER(record_cmp_i64_u8_mo64)(CPUArchState *env, uint64_t pc_diff, uint64_t a1, uint64_t a2)
{
	CPUState *cpu = env_cpu(env);

	gen_cmp_edges(cpu, pc_diff, 8, a1, a2);
}

#ifndef LIBAFL_LIBAFL_COVERAGE_RECORDING_H_INCLUDED
#define LIBAFL_LIBAFL_COVERAGE_RECORDING_H_INCLUDED

#include "exec/coverage.h"

size_t libafl_get_edge_coverage_record_elem_size(void);
size_t libafl_get_comp_coverage_record_elem_size(void);

size_t libafl_get_edge_coverage_record_elems(void);
size_t libafl_get_comp_coverage_record_elems(void);

void *libafl_get_edge_coverage_map(CPUState *cpu);
void *libafl_get_comp_coverage_map(CPUState *cpu);

/**
 * @brief Set the whitelist of physical addresses for coverage recording
 * Should be a static buffer. The layout of this buffer is pairs of "uint64_t"
 * Each pair uint64_t[2]{a,b} is thus a range of guest physical addresses [a, b)
 */
void libafl_set_coverage_whitelist_pa_ranges(void *loc, size_t num);

#endif
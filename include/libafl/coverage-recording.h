#ifndef LIBAFL_LIBAFL_COVERAGE_RECORDING_H_INCLUDED
#define LIBAFL_LIBAFL_COVERAGE_RECORDING_H_INCLUDED

#include "exec/coverage.h"

size_t libafl_get_edge_coverage_record_elem_size(void);
size_t libafl_get_comp_coverage_record_elem_size(void);

size_t libafl_get_edge_coverage_record_elems(void);
size_t libafl_get_comp_coverage_record_elems(void);

void *libafl_get_edge_coverage_map(CPUState *cpu);
void *libafl_get_comp_coverage_map(CPUState *cpu);

#endif
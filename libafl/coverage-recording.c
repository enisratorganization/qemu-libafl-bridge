#include "qemu/osdep.h"
#include "hw/core/cpu.h"
#include "libafl/coverage-recording.h"



size_t libafl_get_edge_coverage_record_elem_size(void) {
    return edge_coverage_record_elem_size;
}
size_t libafl_get_comp_coverage_record_elem_size(void) {
    return comp_coverage_record_elem_size;
}
size_t libafl_get_edge_coverage_record_elems(void) {
    return edge_coverage_record_elems;
}
size_t libafl_get_comp_coverage_record_elems(void) {
	return comp_coverage_record_elems;
}

void * libafl_get_edge_coverage_map(CPUState *cpu) {
    if (edge_coverage_record_tcg_enabled) {
        return cpu->neg.coverage_rec.edge_rec.rec_buf_hitmap;
    } else {
        return NULL;
    }
}

void * libafl_get_comp_coverage_map(CPUState *cpu) {
    if (comp_coverage_record_tcg_enabled) {
        return cpu->neg.coverage_rec.comp_rec.rec_buf_hitmap;
    } else {
        return NULL;
    }
}
#include "qemu/osdep.h"

#include "qemu/main-loop.h"
#include "cpu.h"

#include "exec/ramlist.h"
#include "exec/ram_addr.h"
#include "exec/exec-all.h"
#include "exec/address-spaces.h"

#include "libafl/syx-snapshot/syx-snapshot.h"
#include "libafl/syx-snapshot/device-save.h"
#include "libafl/syx-misc.h"

//#define SYX_SNAPSHOT_DEBUG

#define SYX_DPL_INIT_QHT_ELEMS 512

SyxSnapshotState syx_snapshot_state = {0};
static MemoryRegion* mr_to_enable = NULL;


bool qht_cmp_true(const void* a, const void* b) { return true; }

//set all RAM as clear (not dirty)
static void all_ram_notdirty(void) {

    RAMBlock* rb;
    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(rb)
    {
        if (rb->mr) {
            #ifdef SYX_SNAPSHOT_DEBUG
            printf("cpu_physical_memory_test_and_clear_dirty: %llx %llx\n", rb->mr->addr, rb->used_length);
            #endif
            cpu_physical_memory_test_and_clear_dirty(rb->offset, rb->used_length, DIRTY_MEMORY_MIGRATION);
        }
    }
}

// Root snapshot API
static SyxSnapshot* syx_snapshot_root_new(DeviceSnapshotKind kind,
                                              char** devices);

void syx_snapshot_init(bool cached_bdrvs)
{
    uint64_t page_size = TARGET_PAGE_SIZE;

    syx_snapshot_state.page_size = page_size;
    syx_snapshot_state.page_mask = ((uint64_t)-1) << __builtin_ctz(page_size);

    if (cached_bdrvs) {
        syx_snapshot_state.before_fuzz_cache = syx_cow_cache_new();
        syx_cow_cache_push_layer(syx_snapshot_state.before_fuzz_cache,
                                 SYX_SNAPSHOT_COW_CACHE_DEFAULT_CHUNK_SIZE,
                                 SYX_SNAPSHOT_COW_CACHE_DEFAULT_MAX_BLOCKS);
    }

    syx_snapshot_state.is_enabled = false;
}

SyxSnapshot* syx_snapshot_new(bool track, bool is_active_bdrv_cache,
                              DeviceSnapshotKind kind, char** devices)
{
    SyxSnapshot* snapshot = syx_snapshot_root_new(kind, devices);
   
    if (is_active_bdrv_cache) {
        // we have cached writes from BEFORE fuzzing starts
        snapshot->bdrvs_cow_cache = syx_snapshot_state.before_fuzz_cache;
        syx_snapshot_state.before_fuzz_cache = NULL;
    } else {
        snapshot->bdrvs_cow_cache = syx_cow_cache_new();
    }
    syx_cow_cache_push_layer(snapshot->bdrvs_cow_cache,
        SYX_SNAPSHOT_COW_CACHE_DEFAULT_CHUNK_SIZE,
        SYX_SNAPSHOT_COW_CACHE_DEFAULT_MAX_BLOCKS);
    syx_snapshot_state.active_bdrv_cache_snapshot = snapshot;

    if (track) {
        syx_snapshot_state.thesnap = snapshot;
        //make sure to catch all new writes
        //with a filled TLB there might be missed writes
        tlb_flush_all_cpus();
        all_ram_notdirty();
    }

    syx_snapshot_state.is_enabled = true;

    return snapshot;
}

void syx_snapshot_free(SyxSnapshot* snapshot)
{
 
}

static SyxSnapshot* syx_snapshot_root_new(DeviceSnapshotKind kind,
                                              char** devices)
{
    SyxSnapshot* rootsnap = g_new0(SyxSnapshot, 1);
    RAMBlock* rb;

    BQL_LOCK_GUARD();

    DeviceSaveState* dss = device_save_kind(kind, devices);
    rootsnap->dss[0] = *dss;
    g_free(dss);
    rootsnap->inc = 0;

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(rb)
    {
        SyxSnapshotRAMBlock* srb = g_new0(SyxSnapshotRAMBlock, 1);
        srb->used_length = rb->used_length;
        rb->syx = srb;
        SyxSnapshotInc* root = &srb->incs[0];
        root->seqnum = 1;
        root->saved_pages =
            g_aligned_alloc(1, rb->max_length, qemu_real_host_page_size());
        memcpy(root->saved_pages, rb->host, rb->used_length);

        qht_init(&root->dpl, qht_cmp_true, SYX_DPL_INIT_QHT_ELEMS, QHT_MODE_AUTO_RESIZE);
    } 

    return rootsnap;
}

static void save_pages(void* p, uint32_t h, void* up) {
    SyxSnapshotInc* sinc = ((void**)up)[0];
    RAMBlock* rb = ((void**)up)[1];
    ram_addr_t *min  = &((void**)up)[2];
    ram_addr_t *max  = &((void**)up)[3];
    uintptr_t *any  = &((void**)up)[4];
    *any = 1;

    ram_addr_t offset = ((ram_addr_t)h) << TARGET_PAGE_BITS;
    size_t seq = p;
    memcpy(sinc->saved_pages+ (seq-1)*TARGET_PAGE_SIZE, rb->host + offset, TARGET_PAGE_SIZE);

    if(offset < *min) *min = offset;
    if(offset > *max) *max = offset;
}
void syx_snapshot_increment_push(SyxSnapshot* snapshot, DeviceSnapshotKind kind,
                                 char** devices)
{
    const size_t PAGESZ = TARGET_PAGE_SIZE;
    RAMBlock* rb;

    BQL_LOCK_GUARD();

    size_t inc = ++snapshot->inc;

    assert(inc >= 1);
    assert(inc < SYX_SNAPSHOT_MAX_INCREMENTAL_DEPTH);

    DeviceSaveState *dss = device_save_kind(kind, devices);
    snapshot->dss[inc] = *dss;
    g_free(dss);

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(rb)
    {
        SyxSnapshotRAMBlock* srb = rb->syx;
        SyxSnapshotInc *sinc = &srb->incs[inc];

        size_t numdirty = srb->incs[inc - 1].seqnum-1; //-1 important as we start seqnum at 1!
        sinc->saved_pages = g_aligned_alloc(PAGESZ, numdirty, qemu_real_host_page_size());

        // Copy pages changed since last snapshot
        void* arg[5] = {sinc, rb, -1, 0, 0};
        qht_iter(&srb->incs[inc-1].dpl, save_pages, arg);
       
        // reset QEMU dirty page tracking
        // trying to reduce the range as this is quite costly
        uintptr_t *any  = &arg[4];
        if(*any) {
            ram_addr_t min  = *(ram_addr_t*)&arg[2];
            ram_addr_t max  = *(ram_addr_t*)&arg[3];           
            cpu_physical_memory_test_and_clear_dirty(rb->offset + min, (max-min+1), DIRTY_MEMORY_MIGRATION); 
            #ifdef SYX_SNAPSHOT_DEBUG
            printf("cpu_physical_memory_test_and_clear_dirty: @%llx , %llx-%llx\n", rb->mr->addr, min, max);
            printf("clean: %d\n", cpu_physical_memory_get_dirty(rb->offset, rb->used_length, DIRTY_MEMORY_MIGRATION));
            #endif
        }

        if(!sinc->dpl.map) 
            qht_init(&sinc->dpl, qht_cmp_true, SYX_DPL_INIT_QHT_ELEMS, QHT_MODE_AUTO_RESIZE);

        sinc->seqnum = 1;
    }

    tlb_flush_all_cpus();
}


static void restore_pages(void* p, uint32_t h, void* up) {
    RAMBlock* rb = ((void**)up)[0];
    SyxSnapshot* snap = ((void**)up)[1];
    size_t inc  = ((void**)up)[2];
    ram_addr_t *min  = &((void**)up)[3];
    ram_addr_t *max  = &((void**)up)[4];
    uintptr_t *any  = &((void**)up)[5];
    *any = 1;

    SyxSnapshotRAMBlock* srb = rb->syx;

    ram_addr_t offset = ((ram_addr_t)h) << TARGET_PAGE_BITS;
    size_t seq = p;

    if(offset < *min) *min = offset;
    if(offset > *max) *max = offset;

    // search for hit in DPLs top to bottom
    while(inc > 0) {
        size_t oldseq = qht_lookup(&srb->incs[inc - 1].dpl, NULL, h);
        if(oldseq) {
            memcpy(rb->host + offset, srb->incs[inc].saved_pages+  TARGET_PAGE_SIZE*(oldseq-1), TARGET_PAGE_SIZE);
            goto tb_inv;
        }
        inc--;
    }
    //if we have come here, restore from root snapshot(0), which contains all RAM
    memcpy(rb->host + offset, srb->incs[0].saved_pages + offset, TARGET_PAGE_SIZE);

    tb_inv:
        // Invalidate TBs
        tb_invalidate_phys_range(rb->offset + offset,
        rb->offset + offset + TARGET_PAGE_SIZE - 1);
}


void restore_pages_and_mark_notdirty(RAMBlock *rb, SyxSnapshot *snap, size_t inc)
{
    SyxSnapshotRAMBlock* srb = rb->syx;
    SyxSnapshotInc *sinc = &srb->incs[inc];

    // Copy pages back to hostmem
    void* arg[6] = {rb, snap, inc, -1, 0, 0};
    qht_iter(&sinc->dpl, restore_pages, arg);

    qht_reset(&sinc->dpl);
    sinc->seqnum = 1;

    // reset QEMU dirty page tracking
    // trying to reduce the range as this is quite costly
    uintptr_t *any  = &arg[5];
    if(*any) {
        ram_addr_t min  = *(ram_addr_t*)&arg[3];
        ram_addr_t max  = *(ram_addr_t*)&arg[4];            
        cpu_physical_memory_test_and_clear_dirty(rb->offset + min, (max-min+TARGET_PAGE_SIZE-1), DIRTY_MEMORY_MIGRATION); 
        #ifdef SYX_SNAPSHOT_DEBUG
        printf("cpu_physical_memory_test_and_clear_dirty: @%llx , %llx-%llx\n", rb->mr->addr, min, max);
        printf("clean: %d\n", cpu_physical_memory_get_dirty(rb->offset, rb->used_length, DIRTY_MEMORY_MIGRATION));
        #endif
    }
}


void syx_snapshot_increment_restore_last(SyxSnapshot* snapshot)
{
    const size_t PAGESZ = TARGET_PAGE_SIZE;
    RAMBlock* rb;
    size_t inc = snapshot->inc;

    BQL_LOCK_GUARD();

    device_restore_all(&snapshot->dss[inc]);

    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(rb)
    {
        restore_pages_and_mark_notdirty(rb, snapshot, inc);
    }

    tlb_flush_all_cpus(); // no way to optimize probably?

}

void syx_snapshot_increment_pop(SyxSnapshot* snapshot)
{
    if(snapshot->inc > 0) {
        syx_snapshot_increment_restore_last(snapshot);

        RAMBlock* rb;
        RCU_READ_LOCK_GUARD();
        RAMBLOCK_FOREACH(rb)
        {
            SyxSnapshotRAMBlock* srb = rb->syx;
            g_free(srb->incs[snapshot->inc].saved_pages);
        }

        snapshot->inc--;
    }
}

static inline void syx_snapshot_dirty_list_add_internal(RAMBlock* rb,
                                                        ram_addr_t offset)
{
    assert((offset & TARGET_PAGE_MASK) == offset); // offsets should always be page-aligned.
    const size_t PAGESZ = syx_snapshot_state.page_size;

    SyxSnapshot* snapshot = syx_snapshot_state.thesnap;
    SyxSnapshotRAMBlock *srb = (SyxSnapshotRAMBlock*)rb->syx;

    size_t inc = snapshot->inc;
    size_t seq = srb->incs[inc].seqnum;
    int ret;

    ret = qht_insert(&srb->incs[inc].dpl, seq, (uint32_t) (offset>>TARGET_PAGE_BITS), NULL);

    srb->incs[inc].seqnum += ret;

    #ifdef SYX_SNAPSHOT_DEBUG
    SYX_PRINTF("[%s] [inc %d] Marking offset 0x%lx as dirty\n", rb->idstr, snapshot->inc, offset);
    #endif

}

bool syx_snapshot_is_enabled(void) { return syx_snapshot_state.is_enabled; }


// host_addr should be page-aligned.
void syx_snapshot_dirty_list_add_hostaddr(void* host_addr)
{
    // early check to know whether we should log the page access or not
    if (!syx_snapshot_is_enabled()) {
        return;
    }

    ram_addr_t offset;
    RAMBlock* rb = qemu_ram_block_from_host((void*)host_addr, true, &offset);

/*#ifdef SYX_SNAPSHOT_DEBUG
    SYX_PRINTF("Should mark offset 0x%lx as dirty\n", offset);
#endif*/

    if (!rb) { return;}

    syx_snapshot_dirty_list_add_internal(rb, offset);
}

#define TARGET_NEXT_PAGE_ADDR(p)                                               \
    ((typeof(p))(((uintptr_t)p + TARGET_PAGE_SIZE) & TARGET_PAGE_MASK))
void syx_snapshot_dirty_list_add_hostaddr_range(void* host_addr, uint64_t len)
{
    // early check to know whether we should log the page access or not
    if (!syx_snapshot_is_enabled()) {
        return;
    }

    assert(len < INT64_MAX);
    int64_t len_signed = (int64_t)len;

    syx_snapshot_dirty_list_add_hostaddr(
        QEMU_ALIGN_PTR_DOWN(host_addr, syx_snapshot_state.page_size));
    void* next_page_addr = TARGET_NEXT_PAGE_ADDR(host_addr);
    assert(next_page_addr > host_addr);
    assert(QEMU_PTR_IS_ALIGNED(next_page_addr, TARGET_PAGE_SIZE));

    int64_t len_to_next_page = next_page_addr - host_addr;

    host_addr += len_to_next_page;
    len_signed -= len_to_next_page;

    while (len_signed > 0) {
        assert(QEMU_PTR_IS_ALIGNED(host_addr, TARGET_PAGE_SIZE));

        syx_snapshot_dirty_list_add_hostaddr(host_addr);
        len_signed -= TARGET_PAGE_SIZE;
    }
}


static void copy_ht(void* p, uint32_t h, void* up) {
    struct qht* tgt = ((void**)up)[0];
    qht_insert(tgt, p, h, NULL);
}
void syx_snapshot_root_restore(SyxSnapshot* snapshot)
{
    BQL_LOCK_GUARD();

    // In case, we first restore devices if there is a modification of memory
    // layout
    device_restore_all(&snapshot->dss[0]);

    //collect all dirty pages from increments
    RAMBlock* rb;
    RCU_READ_LOCK_GUARD();
    RAMBLOCK_FOREACH(rb)
    {
        SyxSnapshotRAMBlock* srb = rb->syx;
        SyxSnapshotInc *root = &srb->incs[0];
        size_t inc = snapshot->inc;

        // collect all dirty pages from above increments
        while(inc > 0) {
            SyxSnapshotInc* sinc = &srb->incs[inc];
            void* args[1] = {&root->dpl};
            qht_iter(&sinc->dpl, copy_ht, args);
            qht_reset(&sinc->dpl);
            g_free(sinc->saved_pages);
            sinc->seqnum = 1;
            inc--;
        }

        //now batch-restore them
        restore_pages_and_mark_notdirty(rb, snapshot, 0);
    }

    snapshot->inc = 0;

    tlb_flush_all_cpus();

    syx_cow_cache_flush_highest_layer(snapshot->bdrvs_cow_cache);

    if (mr_to_enable) {
        memory_region_set_enabled(mr_to_enable, true);
        mr_to_enable = NULL;
    }

}

bool syx_snapshot_cow_cache_read_entry(BlockBackend* blk, int64_t offset,
                                       int64_t bytes, QEMUIOVector* qiov,
                                       size_t qiov_offset,
                                       BdrvRequestFlags flags)
{
    if (!syx_snapshot_state.active_bdrv_cache_snapshot) {
        if (syx_snapshot_state.before_fuzz_cache) {
            syx_cow_cache_read_entry(syx_snapshot_state.before_fuzz_cache, blk,
                                     offset, bytes, qiov, qiov_offset, flags);
            return true;
        }

        return false;
    } else {
        syx_cow_cache_read_entry(
            syx_snapshot_state.active_bdrv_cache_snapshot->bdrvs_cow_cache, blk,
            offset, bytes, qiov, qiov_offset, flags);
        return true;
    }
}

bool syx_snapshot_cow_cache_write_entry(BlockBackend* blk, int64_t offset,
                                        int64_t bytes, QEMUIOVector* qiov,
                                        size_t qiov_offset,
                                        BdrvRequestFlags flags)
{
    if (!syx_snapshot_state.active_bdrv_cache_snapshot) {
        if (syx_snapshot_state.before_fuzz_cache) {
            assert(syx_cow_cache_write_entry(
                syx_snapshot_state.before_fuzz_cache, blk, offset, bytes, qiov,
                qiov_offset, flags));
            return true;
        }

        return false;
    } else {
        assert(syx_cow_cache_write_entry(
            syx_snapshot_state.active_bdrv_cache_snapshot->bdrvs_cow_cache, blk,
            offset, bytes, qiov, qiov_offset, flags));
        return true;
    }
}

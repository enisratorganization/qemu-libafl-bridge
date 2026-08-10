/*
 * SYX Snapshot
 *
 * A speed-oriented snapshot mechanism.
 *
 * TODO: complete documentation.
 */

#pragma once

#include "qemu/osdep.h"
#include "qemu/qht.h"

#include "device-save.h"
#include "syx-cow-cache.h"
#include "libafl/syx-misc.h"

#define SYX_SNAPSHOT_COW_CACHE_DEFAULT_CHUNK_SIZE 64
#define SYX_SNAPSHOT_COW_CACHE_DEFAULT_MAX_BLOCKS (1024 * 1024)
#define SYX_SNAPSHOT_MAX_INCREMENTAL_DEPTH 8


typedef struct SyxSnapshotInc {
    size_t seqnum;        // next (incremental) sequence num for dirty page not recorded yet (starting with 1!)
    struct qht dpl;       // Collect new dirty pages since this increment (while this increment is active)
    uint8_t *saved_pages; //Pages different compared to snapshot before
} SyxSnapshotInc;
/**
 * Saved ramblock
 */
typedef struct SyxSnapshotRAMBlock {
    //uint8_t* ram;         // Copy of RAM block @root snapshot
    uint64_t used_length; // Length of the ram block at initial full copy
    // Incremental dirty page lists. We allow at most 8
    SyxSnapshotInc incs[SYX_SNAPSHOT_MAX_INCREMENTAL_DEPTH];
} SyxSnapshotRAMBlock;
/**
 * A snapshot. It is the main object used in this API to
 * handle snapshotting.
 */
typedef struct SyxSnapshot {
    SyxCowCache* bdrv_cow_cache;
    size_t inc; // how many incremental snapshots above root do we have?
    DeviceSaveState dss[SYX_SNAPSHOT_MAX_INCREMENTAL_DEPTH];
} SyxSnapshot;

/*
//No use, there is only ONE snapshot (with incrementals)
typedef struct SyxSnapshotTracker {
    SyxSnapshot** tracked_snapshots;
    uint64_t length;
    uint64_t capacity;
} SyxSnapshotTracker;
*/

typedef struct SyxSnapshotState {
    bool is_enabled;

    uint64_t page_size;
    uint64_t page_mask;

    SyxSnapshot *thesnap; //@TODO: there is only one...
    SyxCowCache* bdrv_cow_cache;

    // Root
} SyxSnapshotState;

typedef struct SyxSnapshotCheckResult {
    uint64_t nb_inconsistencies;
} SyxSnapshotCheckResult;

void syx_snapshot_init(bool cached_bdrvs);

//
// Snapshot API
//

SyxSnapshot* syx_snapshot_new(bool track, bool is_active_bdrv_cache,
                              DeviceSnapshotKind kind, char** devices);

void syx_snapshot_free(SyxSnapshot* snapshot);

void syx_snapshot_root_restore(SyxSnapshot* snapshot);

SyxSnapshotCheckResult syx_snapshot_check(SyxSnapshot* ref_snapshot);

// Push the current RAM state and saves it
void syx_snapshot_increment_push(SyxSnapshot* snapshot, DeviceSnapshotKind kind,
                                 char** devices);

// Restores the last push. Restores the root snapshot if no incremental snapshot
// is present.
void syx_snapshot_increment_pop(SyxSnapshot* snapshot);

void syx_snapshot_increment_restore_last(SyxSnapshot* snapshot);

//simplified argument-less versions of the above using singleton snapshot
void syx_the_snapshot_root_restore(void);
void syx_the_snapshot_increment_push(void);
void syx_the_snapshot_increment_pop(void);
void syx_the_snapshot_increment_restore_last(void);
uint64_t syx_snapshot_get_num_dirty(void);

//
// Misc functions
//

bool syx_snapshot_is_enabled(void);

//
// Dirty list API
//

void syx_snapshot_dirty_list_add_hostaddr(void* host_addr);

void syx_snapshot_dirty_list_add_hostaddr_range(void* host_addr, uint64_t len);

/**
 * @brief Same as syx_snapshot_dirty_list_add. The difference
 * being that it has been specially compiled for full context
 * saving so that it can be called from anywhere, even in
 * extreme environments where SystemV ABI is not respected.
 * It was created with tcg-target.inc.c environment in
 * mind.
 *
 * @param dummy A dummy argument. it is to comply with
 *              tcg-target.inc.c specific environment.
 * @param host_addr The host address where the dirty page is located.
 */
void syx_snapshot_dirty_list_add_tcg_target(uint64_t dummy, void* host_addr);

bool syx_snapshot_cow_cache_read_entry(BlockBackend* blk, int64_t offset,
                                       int64_t bytes, QEMUIOVector* qiov,
                                       size_t qiov_offset,
                                       BdrvRequestFlags flags);

bool syx_snapshot_cow_cache_write_entry(BlockBackend* blk, int64_t offset,
                                        int64_t bytes, QEMUIOVector* qiov,
                                        size_t qiov_offset,
                                        BdrvRequestFlags flags);

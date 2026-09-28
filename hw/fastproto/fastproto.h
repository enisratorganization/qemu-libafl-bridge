/*
 * fastproto: helpers for fast (and hacky) firmware re-hosting.
 *
 * Arch independent part. Include as "hw/fastproto/fastproto.h".
 * Arch specific helpers (registers, calling convention, IC): <arch>/fp_<arch>.h
 *
 *  - Logging:        FP_LOG()/FP_LOG_MMIO()  (-d fastproto,fastproto_mmio)
 *  - Hook tables:    FpHook + fp_add_hooks() on top of libafl/instrument.h
 *  - Guest memory:   fp_read()/fp_write()/fp_read_u32()/...
 *  - Machine setup:  fp_create_cpus(), fp_add_memory(), fp_load_firmware()
 *  - MMIO stubs:     fp_stub_create()/FP_STUB()   (common/fp_stub.c)
 *  - Offloading:     FpHash (emulate firmware hash functions on the host)
 */

#pragma once

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/boards.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "crypto/hash.h"
#include "libafl/instrument.h"

/* ------------------------------------------------------------------------
 * Logging (enable with -d fastproto,fastproto_mmio -D logfile)
 */

/* Hook hits and all other prototype messages */
#define FP_LOG(fmt, ...) \
    qemu_log_mask(LOG_FASTPROTO, fmt, ## __VA_ARGS__)

/* MMIO accesses of stub/prototype devices (very verbose) */
#define FP_LOG_MMIO(fmt, ...) \
    qemu_log_mask(LOG_FASTPROTO_MMIO, fmt, ## __VA_ARGS__)

/* Uniform MMIO access trace line, used by fp-stub and custom devices */
static inline void fp_log_mmio(const char *dev, bool is_write, hwaddr off,
                               unsigned size, uint64_t val)
{
    FP_LOG_MMIO("%s: %s off 0x%" HWADDR_PRIx " sz %u val 0x%" PRIx64 "\n",
                dev, is_write ? "write" : "read ", off, size, val);
}

/* ------------------------------------------------------------------------
 * Hook tables
 *
 * Declare hooks as static const tables and register them in one go:
 *
 *   static bool my_cb(CPUState *cs, vaddr pc, void *opaque) {
 *       const FpHook *h = opaque;           // opaque is the table entry!
 *       ...
 *       return false;                       // see libafl/instrument.h
 *   }
 *   static const FpHook hooks[] = {
 *       FP_RET(0x1000, 0, "pll_init"),      // arch helpers: fp_<arch>.h
 *       FP_HOOK(0x2000, my_cb, "my_function"),
 *   };
 *   FP_ADD_HOOKS(hooks);
 *
 * Every hit is logged with its name (-d fastproto). Tables must have static
 * storage duration because the entries are passed as opaque.
 * For per-vCPU hooks or a plain opaque use add_instrument() directly.
 */
typedef struct FpHook {
    vaddr pc;               /* guest virtual PC to hook */
    InstrumentCallback cb;  /* gets (const FpHook *) as opaque */
    const char *name;       /* for logging; also documents the hook */
    uint64_t arg;           /* generic argument (e.g. return value) */
    int reg;                /* generic register number argument */
} FpHook;

#define FP_HOOK(pc_, cb_, name_) \
    { .pc = (pc_), .cb = (cb_), .name = (name_) }
#define FP_HOOK_ARG(pc_, cb_, arg_, name_) \
    { .pc = (pc_), .cb = (cb_), .arg = (uint64_t)(arg_), .name = (name_) }
/* Only log the hit (tracing / "did we get here?") */
#define FP_TRACE(pc_, name_) FP_HOOK(pc_, fp_cb_nop, name_)

void fp_add_hooks(const FpHook *hooks, size_t n);
#define FP_ADD_HOOKS(table) fp_add_hooks(table, ARRAY_SIZE(table))

/* Argument of the FpHook entry given as opaque */
static inline uint64_t fp_hook_arg(void *opaque)
{
    return ((const FpHook *)opaque)->arg;
}

/* Generic callback: does nothing (the hit is logged by the table glue) */
bool fp_cb_nop(CPUState *cs, vaddr pc, void *opaque);

/* ------------------------------------------------------------------------
 * Guest memory access from hooks (guest *virtual* addresses of @cs, i.e.
 * through the current MMU config; ignores page permissions). Writes to RAM
 * invalidate affected TBs; writes to ROM regions are dropped.
 * Return false on translation/access failure.
 */
static inline bool fp_read(CPUState *cs, vaddr addr, void *buf, size_t len)
{
    return cpu_memory_rw_debug(cs, addr, buf, len, false) == 0;
}

static inline bool fp_write(CPUState *cs, vaddr addr, const void *buf,
                            size_t len)
{
    return cpu_memory_rw_debug(cs, addr, (void *)buf, len, true) == 0;
}

/* Little-endian scalar accessors (0 on failure) */
static inline uint32_t fp_read_u32(CPUState *cs, vaddr addr)
{
    uint8_t b[4] = { 0 };
    fp_read(cs, addr, b, sizeof(b));
    return ldl_le_p(b);
}

static inline uint64_t fp_read_u64(CPUState *cs, vaddr addr)
{
    uint8_t b[8] = { 0 };
    fp_read(cs, addr, b, sizeof(b));
    return ldq_le_p(b);
}

static inline bool fp_write_u8(CPUState *cs, vaddr addr, uint8_t val)
{
    return fp_write(cs, addr, &val, 1);
}

static inline bool fp_write_u32(CPUState *cs, vaddr addr, uint32_t val)
{
    uint8_t b[4];
    stl_le_p(b, val);
    return fp_write(cs, addr, b, sizeof(b));
}

static inline bool fp_write_u64(CPUState *cs, vaddr addr, uint64_t val)
{
    uint8_t b[8];
    stq_le_p(b, val);
    return fp_write(cs, addr, b, sizeof(b));
}

/* Read a NUL terminated string (at most @max chars). Caller g_free()s. */
char *fp_read_str(CPUState *cs, vaddr addr, size_t max);

/* ------------------------------------------------------------------------
 * Machine setup helpers
 */

/*
 * Create ms->smp.cpus CPUs of ms->cpu_type; all but CPU 0 start powered off
 * (to be woken via PSCI etc.). @setup (optional) may set properties on each
 * CPU object before it is realized.
 */
void fp_create_cpus(MachineState *ms,
                    void (*setup)(Object *cpu, int index, void *opaque),
                    void *opaque);

typedef enum FpMemType {
    FP_RAM,     /* zeroed RAM */
    FP_ROM,     /* read-only for the guest; loaders may write it */
} FpMemType;

typedef struct FpMemRegion {
    const char *name;
    hwaddr base;
    uint64_t size;
    FpMemType type;
} FpMemRegion;

/* Add RAM/ROM regions to the system address space */
void fp_add_memory(const FpMemRegion *regions, size_t n);
#define FP_ADD_MEMORY(table) fp_add_memory(table, ARRAY_SIZE(table))

/*
 * Map a -object memory-backend-*,id=@id at @base (exits with an error
 * message if the backend is missing).
 */
void fp_map_memdev(const char *id, hwaddr base);

/*
 * Load a raw image @file (searched like -bios, i.e. also in -L dirs) to
 * guest physical @addr. Exits on error. Returns the image size.
 */
int64_t fp_load_firmware(const char *file, hwaddr addr, uint64_t max_size);

/* ------------------------------------------------------------------------
 * fp-stub: generic, table driven MMIO register stub (common/fp_stub.c)
 *
 *   FP_STUB("qcom_ufsphy", 0x1D87000, 0x1000, FP_STUB_CONST,
 *           { 0xd60, 1 }, { 0xd80, 1 });
 *
 * FP_STUB_CONST: listed registers read their value, everything else reads 0,
 *                writes are ignored (logged).
 * FP_STUB_RAM:   registers behave like RAM (writes stick); listed registers
 *                are reset values, or constants if .fixed is set.
 * FP_STUB_QUIET: don't log accesses (for registers polled in hot loops).
 * FP_STUB_ANY_SIZE: allow 1..8 byte accesses (default: 4 byte only; other
 *                sizes fail and are reported with -d guest_errors).
 * Unlisted CONST accesses are also logged with -d unimp.
 */
typedef struct FpReg {
    hwaddr off;
    uint64_t val;
    bool fixed;     /* RAM mode: always read @val, ignore writes */
} FpReg;

enum {
    FP_STUB_CONST = 0,
    FP_STUB_RAM   = 1 << 0,
    FP_STUB_QUIET = 1 << 1,
    FP_STUB_ANY_SIZE = 1 << 2,
};

#define TYPE_FP_STUB "fp-stub"

DeviceState *fp_stub_create(const char *name, hwaddr base, uint64_t size,
                            unsigned flags, const FpReg *regs, size_t nregs);

/* Inline variant: registers are given as trailing { off, val } pairs */
#define FP_STUB(name, base, size, flags, ...) ({                        \
        static const FpReg fp_regs_[] = { __VA_ARGS__ };                \
        fp_stub_create(name, base, size, flags, fp_regs_,               \
                       ARRAY_SIZE(fp_regs_));                           \
    })

/*
 * Low level: set the register table of a not yet realized fp-stub (created
 * with qdev_new(TYPE_FP_STUB), e.g. to also set "access-min"/"access-max").
 */
void fp_stub_set_regs(DeviceState *dev, const FpReg *regs, size_t nregs);

/* ------------------------------------------------------------------------
 * VMState helper for prototype devices: save all struct members from
 * @first to the end of @State as one raw buffer (no pointers there!).
 */
#define FP_VMSTATE_TAIL(State, first) {                                 \
        .name = "tail", .version_id = 1,                                \
        .size = sizeof(State) - offsetof(State, first),                 \
        .info = &vmstate_info_buffer, .flags = VMS_BUFFER,              \
        .offset = offsetof(State, first),                               \
    }

/* ------------------------------------------------------------------------
 * FpHash: offload firmware hash functions (init/update/final) to the host.
 * Much faster than emulating the (often HW accelerated) implementation.
 */
typedef struct FpHash {
    QCryptoHashAlgo alg;
    GByteArray *data;       /* collected message */
    bool active;
} FpHash;

void fp_hash_init(FpHash *h, QCryptoHashAlgo alg);
/* Append @len bytes at guest vaddr @src; false if too large/not active */
bool fp_hash_update(FpHash *h, CPUState *cs, vaddr src, uint64_t len);
/* Write the digest to guest vaddr @dst; returns digest size, 0 on error */
size_t fp_hash_final(FpHash *h, CPUState *cs, vaddr dst);

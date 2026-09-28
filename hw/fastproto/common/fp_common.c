/*
 * fastproto: arch independent helpers (see hw/fastproto/fastproto.h)
 */

#include "hw/fastproto/fastproto.h"
#include "hw/loader.h"
#include "qemu/datadir.h"
#include "system/hostmem.h"

/* ---- Hook tables ---- */

/* Glue between instrument.c and FpHook entries: log, then call */
static bool fp_hook_trampoline(CPUState *cs, vaddr pc, void *opaque)
{
    const FpHook *h = opaque;

    FP_LOG("hook %s @0x%" VADDR_PRIx " cpu %d\n",
           h->name ? h->name : "?", pc, cs->cpu_index);
    return h->cb(cs, pc, opaque);
}

void fp_add_hooks(const FpHook *hooks, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!add_instrument(hooks[i].pc, INSTRUMENT_ALL_CPUS,
                            fp_hook_trampoline, (void *)&hooks[i])) {
            warn_report("fastproto: hook '%s' @0x%" VADDR_PRIx
                        " replaced an existing hook",
                        hooks[i].name ? hooks[i].name : "?", hooks[i].pc);
        }
    }
}

bool fp_cb_nop(CPUState *cs, vaddr pc, void *opaque)
{
    return false;
}

/* ---- Guest memory ---- */

char *fp_read_str(CPUState *cs, vaddr addr, size_t max)
{
    char *s = g_malloc0(max + 1);

    for (size_t i = 0; i < max; i++) {
        if (!fp_read(cs, addr + i, &s[i], 1) || s[i] == '\0') {
            s[i] = '\0';
            break;
        }
    }
    return s;
}

/* ---- Machine setup ---- */

void fp_create_cpus(MachineState *ms,
                    void (*setup)(Object *cpu, int index, void *opaque),
                    void *opaque)
{
    for (int n = 0; n < ms->smp.cpus; n++) {
        Object *cpuobj = object_new(ms->cpu_type);

        object_property_add_child(OBJECT(ms), "cpu[*]", cpuobj);
        qdev_prop_set_bit(DEVICE(cpuobj), "start-powered-off", n > 0);
        if (setup) {
            setup(cpuobj, n, opaque);
        }
        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        object_unref(cpuobj);
    }
}

void fp_add_memory(const FpMemRegion *regions, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const FpMemRegion *r = &regions[i];
        MemoryRegion *mr = g_new(MemoryRegion, 1);

        if (r->type == FP_ROM) {
            memory_region_init_rom(mr, NULL, r->name, r->size, &error_fatal);
        } else {
            memory_region_init_ram(mr, NULL, r->name, r->size, &error_fatal);
        }
        memory_region_add_subregion(get_system_memory(), r->base, mr);
    }
}

void fp_map_memdev(const char *id, hwaddr base)
{
    Object *o = object_resolve_path_component(object_get_objects_root(), id);
    HostMemoryBackend *be;

    if (o == NULL || !object_dynamic_cast(o, TYPE_MEMORY_BACKEND)) {
        error_report("fastproto: memory backend '%s' missing, add e.g. "
                     "-object memory-backend-file,id=%s,size=...,mem-path=...",
                     id, id);
        exit(1);
    }
    be = MEMORY_BACKEND(o);
    memory_region_add_subregion(get_system_memory(), base,
                                host_memory_backend_get_memory(be));
    host_memory_backend_set_mapped(be, true);
}

int64_t fp_load_firmware(const char *file, hwaddr addr, uint64_t max_size)
{
    g_autofree char *fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, file);
    int64_t size;

    if (fn == NULL) {
        error_report("fastproto: unable to find '%s'", file);
        exit(1);
    }
    size = load_image_targphys(fn, addr, max_size);
    if (size < 0) {
        error_report("fastproto: unable to load '%s' @0x%" HWADDR_PRIx
                     " (max 0x%" PRIx64 ")", fn, addr, max_size);
        exit(1);
    }
    return size;
}

/* ---- Hash offloading ---- */

#define FP_HASH_MAX (256 * MiB)     /* guard against garbage lengths */

void fp_hash_init(FpHash *h, QCryptoHashAlgo alg)
{
    if (h->data == NULL) {
        h->data = g_byte_array_new();
    }
    g_byte_array_set_size(h->data, 0);
    h->alg = alg;
    h->active = true;
}

bool fp_hash_update(FpHash *h, CPUState *cs, vaddr src, uint64_t len)
{
    size_t used;

    if (!h->active) {
        return false;
    }
    used = h->data->len;
    if (len > FP_HASH_MAX - used) {
        qemu_log_mask(LOG_GUEST_ERROR, "fp_hash_update: too large (0x%"
                      PRIx64 ")\n", len);
        return false;
    }
    g_byte_array_set_size(h->data, used + len);
    fp_read(cs, src, h->data->data + used, len);
    return true;
}

size_t fp_hash_final(FpHash *h, CPUState *cs, vaddr dst)
{
    g_autofree uint8_t *digest = NULL;
    size_t sz = 0;

    if (!h->active) {
        return 0;
    }
    qcrypto_hash_bytes(h->alg, (const char *)h->data->data, h->data->len,
                       &digest, &sz, &error_fatal);
    fp_write(cs, dst, digest, sz);
    /* ready for the next message with the same algorithm */
    g_byte_array_set_size(h->data, 0);
    return sz;
}

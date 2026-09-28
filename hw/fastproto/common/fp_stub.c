/*
 * fp-stub: generic, table driven MMIO register stub.
 * See "MMIO stubs" in hw/fastproto/fastproto.h for usage.
 *
 * Typical trial & error loop: map an empty FP_STUB_CONST stub over an
 * unknown peripheral, run with -d fastproto_mmio,unimp, look at what the
 * firmware polls/expects and add { offset, value } entries until it is happy.
 */

#include "hw/fastproto/fastproto.h"
#include "qemu/bswap.h"

/* QOM cast macro is FP_STUB_DEV(); FP_STUB() is the convenience macro */
#define TYPE_FP_STUB_DEV TYPE_FP_STUB
OBJECT_DECLARE_SIMPLE_TYPE(FpStubState, FP_STUB_DEV)

struct FpStubState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegionOps ops;    /* per instance: access sizes are properties */

    /* configuration (properties + fp_stub_set_regs()) */
    char *name;
    uint64_t size;
    bool ram_mode;
    bool quiet;
    uint8_t access_min;
    uint8_t access_max;
    const FpReg *regs;
    size_t nregs;

    /* RAM mode backing store (little endian), saved in the vmstate */
    uint32_t ram_size;
    uint8_t *ram;
};

static const FpReg *fp_stub_find(FpStubState *s, hwaddr off)
{
    for (size_t i = 0; i < s->nregs; i++) {
        if (s->regs[i].off == off) {
            return &s->regs[i];
        }
    }
    return NULL;
}

static uint64_t fp_stub_read(void *opaque, hwaddr off, unsigned size)
{
    FpStubState *s = opaque;
    const FpReg *r = fp_stub_find(s, off);
    uint64_t val = 0;

    if (s->ram_mode) {
        val = (r && r->fixed) ? r->val : ldn_le_p(s->ram + off, size);
    } else if (r) {
        val = r->val;
    } else {
        qemu_log_mask(LOG_UNIMP, "%s: unlisted read off 0x%" HWADDR_PRIx
                      " sz %u\n", s->name, off, size);
    }
    if (!s->quiet) {
        fp_log_mmio(s->name, false, off, size, val);
    }
    return val;
}

static void fp_stub_write(void *opaque, hwaddr off, uint64_t val,
                          unsigned size)
{
    FpStubState *s = opaque;
    const FpReg *r = fp_stub_find(s, off);

    if (!s->quiet) {
        fp_log_mmio(s->name, true, off, size, val);
    }
    if (s->ram_mode && !(r && r->fixed)) {
        stn_le_p(s->ram + off, size, val);
    }
}

static void fp_stub_reset(DeviceState *dev)
{
    FpStubState *s = FP_STUB_DEV(dev);

    if (!s->ram_mode) {
        return;
    }
    memset(s->ram, 0, s->ram_size);
    for (size_t i = 0; i < s->nregs; i++) {
        const FpReg *r = &s->regs[i];
        if (r->val > UINT32_MAX && r->off + 8 <= s->ram_size) {
            stq_le_p(s->ram + r->off, r->val);
        } else if (r->off + 4 <= s->ram_size) {
            stl_le_p(s->ram + r->off, r->val);
        }
    }
}

static void fp_stub_realize(DeviceState *dev, Error **errp)
{
    FpStubState *s = FP_STUB_DEV(dev);

    if (s->name == NULL) {
        s->name = g_strdup("fp-stub");
    }
    if (s->size == 0 || s->size > UINT32_MAX) {
        error_setg(errp, "%s: invalid size 0x%" PRIx64, s->name, s->size);
        return;
    }
    for (size_t i = 0; i < s->nregs; i++) {
        if (s->regs[i].off >= s->size) {
            error_setg(errp, "%s: register 0x%" HWADDR_PRIx
                       " outside of size 0x%" PRIx64,
                       s->name, s->regs[i].off, s->size);
            return;
        }
    }
    if (s->ram_mode) {
        s->ram_size = s->size;
        s->ram = g_malloc0(s->ram_size);
    }

    s->ops = (MemoryRegionOps) {
        .read = fp_stub_read,
        .write = fp_stub_write,
        .endianness = DEVICE_LITTLE_ENDIAN,
        .valid = {
            .min_access_size = s->access_min,
            .max_access_size = s->access_max,
        },
    };
    memory_region_init_io(&s->mmio, OBJECT(s), &s->ops, s, s->name, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static const VMStateDescription vmstate_fp_stub = {
    .name = TYPE_FP_STUB,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_VBUFFER_UINT32(ram, FpStubState, 1, NULL, ram_size),
        VMSTATE_END_OF_LIST()
    }
};

static const Property fp_stub_properties[] = {
    DEFINE_PROP_STRING("name", FpStubState, name),
    DEFINE_PROP_UINT64("size", FpStubState, size, 0x1000),
    DEFINE_PROP_BOOL("ram", FpStubState, ram_mode, false),
    DEFINE_PROP_BOOL("quiet", FpStubState, quiet, false),
    DEFINE_PROP_UINT8("access-min", FpStubState, access_min, 4),
    DEFINE_PROP_UINT8("access-max", FpStubState, access_max, 4),
};

static void fp_stub_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = fp_stub_realize;
    dc->legacy_reset = fp_stub_reset;
    dc->vmsd = &vmstate_fp_stub;
    device_class_set_props(dc, fp_stub_properties);
}

static const TypeInfo fp_stub_info = {
    .name          = TYPE_FP_STUB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(FpStubState),
    .class_init    = fp_stub_class_init,
};

static void fp_stub_register_types(void)
{
    type_register_static(&fp_stub_info);
}

type_init(fp_stub_register_types)

/* ---- C API ---- */

void fp_stub_set_regs(DeviceState *dev, const FpReg *regs, size_t nregs)
{
    FpStubState *s = FP_STUB_DEV(dev);

    assert(!dev->realized);
    s->regs = regs;
    s->nregs = nregs;
}

DeviceState *fp_stub_create(const char *name, hwaddr base, uint64_t size,
                            unsigned flags, const FpReg *regs, size_t nregs)
{
    DeviceState *dev = qdev_new(TYPE_FP_STUB);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint64(dev, "size", size);
    qdev_prop_set_bit(dev, "ram", flags & FP_STUB_RAM);
    qdev_prop_set_bit(dev, "quiet", flags & FP_STUB_QUIET);
    if (flags & FP_STUB_ANY_SIZE) {
        qdev_prop_set_uint8(dev, "access-min", 1);
        qdev_prop_set_uint8(dev, "access-max", 8);
    }
    fp_stub_set_regs(dev, regs, nregs);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, base);
    return dev;
}

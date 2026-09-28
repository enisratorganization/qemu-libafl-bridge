/*
 * MT6768: MediaTek (16550 like) UART, TX only. THR @0x0, LSR @0x14.
 */

#include "hw/fastproto/fastproto.h"
#include "hw/qdev-properties-system.h"
#include "chardev/char-fe.h"

#define TYPE_MTK_UART "mtk_uart"
OBJECT_DECLARE_SIMPLE_TYPE(MtkUartState, MTK_UART)

#define UART_THR 0x00
#define UART_LSR 0x14
#define UART_LSR_THRE (1 << 5)
#define UART_LSR_TEMT (1 << 6)

struct MtkUartState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    CharBackend chr;
};

static uint64_t mtk_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    /* TX always empty; not logged: polled for every character */
    return addr == UART_LSR ? UART_LSR_THRE | UART_LSR_TEMT : 0;
}

static void mtk_uart_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    MtkUartState *s = opaque;

    if (addr == UART_THR) {
        uint8_t ch = val;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      TYPE_MTK_UART, addr);
    }
}

static const MemoryRegionOps mtk_uart_ops = {
    .read = mtk_uart_read,
    .write = mtk_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const Property mtk_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", MtkUartState, chr),
};

static void mtk_uart_init(Object *obj)
{
    MtkUartState *s = MTK_UART(obj);

    memory_region_init_io(&s->mmio, obj, &mtk_uart_ops, s, TYPE_MTK_UART,
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void mtk_uart_class_init(ObjectClass *klass, void *data)
{
    device_class_set_props(DEVICE_CLASS(klass), mtk_uart_properties);
}

static const TypeInfo mtk_uart_info = {
    .name          = TYPE_MTK_UART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MtkUartState),
    .instance_init = mtk_uart_init,
    .class_init    = mtk_uart_class_init,
};

static void mtk_uart_register_types(void)
{
    type_register_static(&mtk_uart_info);
}

type_init(mtk_uart_register_types)

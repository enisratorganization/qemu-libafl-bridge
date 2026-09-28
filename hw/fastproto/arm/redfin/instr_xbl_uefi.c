/*
 * Redfin: XBL UEFI (DXE) hooks
 */

#include "redfin.h"
#include "uefi_guids.h"
#include "exec/cputlb.h"

/* Force synchronous UART output (no buffering in RAM) */
static bool serial_write_buffered(CPUState *cs, vaddr pc, void *opaque)
{
    fp_write_u8(cs, 0x9FC38B44, 1);     /* SyncIO_enabled */
    return false;
}

/* The MMU is disabled while TTBR0 is switched */
static bool disable_mmu_before_ttbr0_set(CPUState *cs, vaddr pc, void *opaque)
{
    CPUARMState *env = cpu_env(cs);

    env->cp15.sctlr_ns &= ~SCTLR_M;
    tlb_flush(cs);
    arm_rebuild_hflags(env);
    return false;
}

static bool set_rpmh_is_standalone(CPUState *cs, vaddr pc, void *opaque)
{
    fp_write_u8(cs, 0xA505B5F8, 1);
    return false;
}

static const char *uefi_guid_lookup(const uint8_t guid[16])
{
    char hex[33];

    for (int i = 0; i < 16; i++) {
        snprintf(hex + 2 * i, 3, "%02x", guid[i]);
    }
    for (size_t i = 0; i < ARRAY_SIZE(uefi_guid_names); i++) {
        if (strcmp(uefi_guid_names[i].guid_hex, hex) == 0) {
            return uefi_guid_names[i].name;
        }
    }
    return NULL;
}

/* CoreImageLoad(?, ?, EFI_DEVICE_PATH *path): log which image is loaded */
static bool CoreImageLoad(CPUState *cs, vaddr pc, void *opaque)
{
    uint8_t buffer[512];
    uint8_t guid[16] = { 0 };
    const char *name;
    size_t i = 0;

    fp_read(cs, fp_arg(cs, 2), buffer, sizeof(buffer));

    /* Walk the device path; the GUID of the last node names the file */
    while (i + 4 + sizeof(guid) <= sizeof(buffer)) {
        uint16_t length;

        if (buffer[i] == 0x7f) {    /* END_DEVICE_PATH_TYPE */
            break;
        }
        length = lduw_le_p(buffer + i + 2);
        memcpy(guid, buffer + i + 4, sizeof(guid));
        if (length < 4) {           /* malformed node: avoid endless loop */
            break;
        }
        i += length;
    }

    name = uefi_guid_lookup(guid);
    if (name) {
        FP_LOG("CoreImageLoad: %s\n", name);
    } else {
        FP_LOG("CoreImageLoad: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
               "%02x%02x%02x%02x%02x%02x\n",
               guid[0], guid[1], guid[2], guid[3], guid[4], guid[5], guid[6],
               guid[7], guid[8], guid[9], guid[10], guid[11], guid[12],
               guid[13], guid[14], guid[15]);
    }
    return false;
}

static bool CoreImageLoad_DestAddrDetermined(CPUState *cs, vaddr pc,
                                             void *opaque)
{
    FP_LOG("DestAddr: %" PRIx64 "\n", fp_arg(cs, 0));
    return false;
}

static const FpHook xbl_uefi_hooks[] = {
    FP_HOOK(0x9FC13E88, serial_write_buffered, "serial_write_buffered"),
    FP_HOOK(0x9FC17760, disable_mmu_before_ttbr0_set,
            "disable MMU before TTBR0 set"),

    FP_HOOK(0xA51ADE90, CoreImageLoad, "CoreImageLoad"),
    FP_HOOK(0xA51ADA8C, CoreImageLoad_DestAddrDetermined,
            "CoreImageLoad dest addr"),

    FP_HOOK(0xA5050734, set_rpmh_is_standalone, "set rpmh_is_standalone"),

    FP_RET(0xA50463A8, 0, "pdc_seq_handle_init"),
    FP_RET(0xA5046418, 0, "pdc_seq_enable"),
    FP_RET(0xA50127C4, 0, "Clock_InitTarget"),
    FP_RET(0xA500AF20, 0, "Clock_InitBases"),
    FP_RET(0xA46A552C, 0, "UFSSmmuConfig"),
};

void xbl_uefi_instrument(void)
{
    FP_ADD_HOOKS(xbl_uefi_hooks);
}

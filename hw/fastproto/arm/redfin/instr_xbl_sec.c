/*
 * Redfin: XBL_SEC hooks (XPU = Qualcomm memory protection units, no model)
 */

#include "redfin.h"

static bool xpu_stuff(CPUState *cs, vaddr pc, void *opaque)
{
    FP_LOG("XPU STUFF: %" PRIx64 " %" PRIx64 " %" PRIx64 "\n",
           fp_arg(cs, 0), fp_arg(cs, 1), fp_arg(cs, 2));
    return fp_return(cs, 0);
}

static const FpHook xbl_sec_hooks[] = {
    FP_RET(0x148F35B4, 0, "XPU init"),
    FP_HOOK(0x148EA520, xpu_stuff, "xpu_stuff"),
    FP_HOOK(0x148EA4AC, xpu_stuff, "xpu_stuff"),
};

void xbl_sec_instrument(void)
{
    FP_ADD_HOOKS(xbl_sec_hooks);
}

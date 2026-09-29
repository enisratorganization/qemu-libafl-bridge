/*
 * MT6768 CPU model: Cortex-A55 plus what the MediaTek firmware needs on top.
 * Use with -machine mt6768 (default) or -cpu mt6768-a55.
 */

#include "mt6768.h"
#include "target/arm/cpregs.h"

#define MT6768_MIDR 0x410FD0F0

/* op0:3 op1:0 crn:15 crm:3 and crm:4, op2 0..7; ARM_CP_CONST: the
 * guest reads resetvalue and writes are ignored (no state, no vmstate) */
static const ARMCPRegInfo mt6768_dsu_reginfo[] = {
    { .name = "CLUSTERCFR_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 3, .opc2 = 0,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERIDR_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 3, .opc2 = 1,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERREVIDR_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 3, .opc2 = 2,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERACTLR_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 3, .opc2 = 3,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERECTLR_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 3, .opc2 = 4,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERPWRCTLR_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 3, .opc2 = 5,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERPWRDN_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 3, .opc2 = 6,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERPWRSTAT_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 3, .opc2 = 7,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = (0xf << 4) },
    { .name = "CLUSTERTHREADSID_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 4, .opc2 = 0,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERACPSID_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 4, .opc2 = 1,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERSTASHSID_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 4, .opc2 = 2,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERPARTCR_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 4, .opc2 = 3,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERBUSQOS_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 4, .opc2 = 4,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERL3HIT_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 4, .opc2 = 5,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERL3MISS_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 4, .opc2 = 6,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
    { .name = "CLUSTERTHREADSIDOVR_EL1", .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 0, .crn = 15, .crm = 4, .opc2 = 7,
      .access = PL1_RW, .type = ARM_CP_CONST, .resetvalue = 0 },
};

/* Runs after the cortex-a55 instance_init, so this only adds/overrides */
static void mt6768_a55_init(Object *obj)
{
    ARMCPU *cpu = ARM_CPU(obj);

    cpu->midr = MT6768_MIDR;
    define_arm_cp_regs(cpu, mt6768_dsu_reginfo);
}

static const TypeInfo mt6768_a55_info = {
    .name          = MT6768_CPU_TYPE_NAME,
    .parent        = "cortex-a55-arm-cpu",
    .instance_init = mt6768_a55_init,
};

static void mt6768_cpu_register_types(void)
{
    type_register_static(&mt6768_a55_info);
}

type_init(mt6768_cpu_register_types)

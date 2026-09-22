/*
 * qemu-edge-coverage-<arch>
 *
 * Tiny standalone front-end for the "edge coverage" TCG mod.
 *
 * It sets up an absolutely minimal machine ("-machine none"), maps one guest
 * RAM page at a user supplied address, puts a single instruction into it,
 * primes the CPU registers / flags, executes *exactly* that one instruction
 * with TCG and finally reports which entry of the edge coverage hitmap
 * (rec_buf_hitmap) has been incremented.
 *
 * This is deliberately a single, self contained file that reuses qemu_init()
 * so that no changes to the rest of QEMU (besides one meson.build hunk) are
 * required.
 *
 * Example:
 *   qemu-edge-coverage-aarch64 -a 0x5fef266c -i "81 00 00 54" -f 0x40000000
 *   qemu-edge-coverage-aarch64 -a 0x5ff1b320 -i "20 00 1f d6" -r 1=0xdeadbeef
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "system/system.h"
#include "system/replay.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "exec/cpu-common.h"
#include "exec/target_page.h"
#include "exec/coverage.h"
#include "hw/core/cpu.h"

/* target specific bits (CPUARMState, cpsr_write, gdb register access, ...) */
#include "cpu.h"

#define MAX_INSN_BYTES 16
#define MAX_SET_REGS   64
#define MAX_EXTRA_ARGS 32
/* one RAM page for the instruction; 4k is fine for every supported target */
#define INSN_PAGE_SIZE 4096

typedef struct {
    int regno;
    uint64_t value;
} RegSetting;

static struct {
    uint64_t addr;
    uint8_t insn[MAX_INSN_BYTES];
    size_t insn_len;
    bool have_addr;
    bool have_insn;

    bool have_flags;
    uint64_t flags;
    uint64_t flags_mask;

    RegSetting regs[MAX_SET_REGS];
    int nregs;

    const char *cpu_model;
    const char *covrec;
    char *extra[MAX_EXTRA_ARGS];
    int nextra;
    bool verbose;
} cfg = {
    .flags_mask = 0xf0000000ULL, /* NZCV */
    .cpu_model = "max",
    .covrec = "edge_elem_sz=1,edge_elems=4096,edge_enable=on",
};

static void usage(const char *argv0)
{
    printf(
"Usage: %s -a <guest-addr> -i <insn-hex> [options]\n"
"\n"
"  -a, --addr <addr>      guest address the instruction is placed and\n"
"                         executed at (hex with 0x or decimal)\n"
"  -i, --insn <hex>       raw instruction bytes, e.g. \"81 00 00 54\"\n"
"                         (spaces/commas/0x are ignored, in memory order)\n"
"  -r, --reg <n>=<val>    set CPU register <n> before execution.\n"
"                         numbering follows the gdbstub register numbering\n"
"                         (aarch64: 0..30 = x0..x30, 31 = sp, 32 = pc,\n"
"                          33 = pstate).  May be given multiple times.\n"
"  -f, --flags <val>      set the condition flags via cpsr_write()\n"
"                         (default mask 0x%08x, i.e. NZCV)\n"
"  -m, --flags-mask <val> mask used for the cpsr_write() above\n"
"  -c, --cpu <model>      CPU model to use (default: %s)\n"
"  -o, --covrec <opts>    options passed to QEMU's -covrec\n"
"                         (default: %s)\n"
"  -X, --extra <arg>      extra argument handed to qemu_init(), may be given\n"
"                         multiple times (e.g. -X -d -X in_asm)\n"
"  -v, --verbose          chatty operation (on stderr)\n"
"  -h, --help             this text\n"
"\n"
"Prints every non-zero entry of the edge coverage hitmap as\n"
"  hitmap[<offset>] = <count>\n",
        argv0, (unsigned)cfg.flags_mask, cfg.cpu_model, cfg.covrec);
}

/*
 * Most of the argument parsing happens before qemu_init(), i.e. before the
 * monitor (and with it error_report()) is usable - so complain by hand.
 */
static void G_GNUC_PRINTF(1, 2) G_GNUC_NORETURN fatal(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "qemu-edge-coverage: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static uint64_t parse_num(const char *s, const char *what)
{
    uint64_t val;

    if (qemu_strtou64(s, NULL, 0, &val) < 0) {
        fatal("cannot parse %s: '%s'", what, s);
    }
    return val;
}

/* "81 00 00 54", "8100 0054", "0x81,0x00,..." -> bytes in memory order */
static void parse_insn_bytes(const char *s)
{
    int nibble = 0;
    unsigned cur = 0;

    while (*s) {
        char c = *s++;

        if (c == ' ' || c == ',' || c == '\t' || c == '_' || c == '\\') {
            continue;
        }
        if (c == '0' && (*s == 'x' || *s == 'X')) {
            s++;
            continue;
        }
        if (!g_ascii_isxdigit(c)) {
            fatal("bad character '%c' in instruction bytes", c);
        }
        cur = (cur << 4) | g_ascii_xdigit_value(c);
        if (++nibble == 2) {
            if (cfg.insn_len >= MAX_INSN_BYTES) {
                fatal("instruction too long (max %d bytes)",
                      MAX_INSN_BYTES);
            }
            cfg.insn[cfg.insn_len++] = cur;
            nibble = 0;
            cur = 0;
        }
    }
    if (nibble) {
        fatal("odd number of hex digits in instruction bytes");
    }
    if (!cfg.insn_len) {
        fatal("empty instruction");
    }
}

static void parse_reg_setting(const char *s)
{
    const char *eq = strchr(s, '=');
    char *num;

    if (!eq) {
        fatal("register setting needs the form <n>=<value>: '%s'", s);
    }
    if (cfg.nregs >= MAX_SET_REGS) {
        fatal("too many -r options");
    }
    num = g_strndup(s, eq - s);
    cfg.regs[cfg.nregs].regno = parse_num(num, "register number");
    g_free(num);
    cfg.regs[cfg.nregs].value = parse_num(eq + 1, "register value");
    cfg.nregs++;
}

static void parse_args(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "addr",       required_argument, NULL, 'a' },
        { "insn",       required_argument, NULL, 'i' },
        { "reg",        required_argument, NULL, 'r' },
        { "flags",      required_argument, NULL, 'f' },
        { "flags-mask", required_argument, NULL, 'm' },
        { "cpu",        required_argument, NULL, 'c' },
        { "covrec",     required_argument, NULL, 'o' },
        { "extra",      required_argument, NULL, 'X' },
        { "verbose",    no_argument,       NULL, 'v' },
        { "help",       no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    int c;

    while ((c = getopt_long(argc, argv, "a:i:r:f:m:c:o:X:vh", longopts, NULL))
           != -1) {
        switch (c) {
        case 'a':
            cfg.addr = parse_num(optarg, "address");
            cfg.have_addr = true;
            break;
        case 'i':
            parse_insn_bytes(optarg);
            cfg.have_insn = true;
            break;
        case 'r':
            parse_reg_setting(optarg);
            break;
        case 'f':
            cfg.flags = parse_num(optarg, "flags");
            cfg.have_flags = true;
            break;
        case 'm':
            cfg.flags_mask = parse_num(optarg, "flags mask");
            break;
        case 'c':
            cfg.cpu_model = optarg;
            break;
        case 'o':
            cfg.covrec = optarg;
            break;
        case 'X':
            if (cfg.nextra >= MAX_EXTRA_ARGS) {
                fatal("too many -X options");
            }
            cfg.extra[cfg.nextra++] = optarg;
            break;
        case 'v':
            cfg.verbose = true;
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        default:
            usage(argv[0]);
            exit(1);
        }
    }

    if (!cfg.have_addr || !cfg.have_insn) {
        usage(argv[0]);
        exit(1);
    }
}

/*
 * Map a single RAM page covering @addr and place the instruction into it.
 */
static void setup_guest_page(void)
{
    static MemoryRegion insn_page;
    uint64_t page = cfg.addr & ~((uint64_t)INSN_PAGE_SIZE - 1);
    uint64_t end = (cfg.addr + cfg.insn_len - 1) &
                   ~((uint64_t)INSN_PAGE_SIZE - 1);
    uint64_t size = (end - page) + INSN_PAGE_SIZE;

    memory_region_init_ram(&insn_page, NULL, "edge-coverage.insn", size,
                           &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(), page,
                                        &insn_page, 1);

    cpu_physical_memory_write(cfg.addr, cfg.insn, cfg.insn_len);

    if (cfg.verbose) {
        fprintf(stderr, "mapped %" PRIu64 " byte(s) of RAM at 0x%" PRIx64
                ", %zu instruction byte(s) at 0x%" PRIx64 "\n",
                size, page, cfg.insn_len, cfg.addr);
    }
}

/*
 * Everything below is (potentially) architecture specific.  Keep it in one
 * place so that other targets can be bolted on with another #elif branch.
 */
#if defined(TARGET_AARCH64)

#define PC_REGNO 32

static void setup_cpu_state(CPUState *cs)
{
    CPUARMState *env = cpu_env(cs);
    uint8_t buf[8];
    int i;

    for (i = 0; i < cfg.nregs; i++) {
        stq_le_p(buf, cfg.regs[i].value);
        if (aarch64_cpu_gdb_write_register(cs, buf, cfg.regs[i].regno) == 0) {
            fatal("cannot write register %d", cfg.regs[i].regno);
        }
        if (cfg.verbose) {
            fprintf(stderr, "reg[%d] = 0x%" PRIx64 "\n",
                    cfg.regs[i].regno, cfg.regs[i].value);
        }
    }

    if (cfg.have_flags) {
        cpsr_write(env, cfg.flags, cfg.flags_mask, CPSRWriteByGDBStub);
        if (cfg.verbose) {
            fprintf(stderr, "flags = 0x%" PRIx64 " (mask 0x%" PRIx64 ")\n",
                    cfg.flags, cfg.flags_mask);
        }
    }

    /* the PC always wins over anything the user may have set via -r */
    stq_le_p(buf, cfg.addr);
    aarch64_cpu_gdb_write_register(cs, buf, PC_REGNO);

    /* make sure the cached TB flags match the state we just installed */
    arm_rebuild_hflags(env);
}

static uint64_t get_pc(CPUState *cs)
{
    return cpu_env(cs)->pc;
}

#else
#error "qemu-edge-coverage has not been ported to this target yet"
#endif

static void nop_work(CPUState *cs, run_on_cpu_data data)
{
    /* only used to flush the vCPU work queue, see main() */
}

static void dump_hitmap(CPUState *cs)
{
    const void *map = cs->neg.coverage_rec.edge_rec.rec_buf_hitmap;
    size_t elems = edge_coverage_record_elems;
    size_t esz = edge_coverage_record_elem_size;
    size_t hits = 0;
    size_t i;

    if (!map) {
        fatal("no edge coverage hitmap allocated "
              "(check the -covrec options)");
    }

    for (i = 0; i < elems; i++) {
        uint64_t v;

        switch (esz) {
        case 1:
            v = ((const uint8_t *)map)[i];
            break;
        case 2:
            v = ((const uint16_t *)map)[i];
            break;
        default:
            v = ((const uint32_t *)map)[i];
            break;
        }
        if (v) {
            printf("hitmap[%zu] = %" PRIu64 "\n", i, v);
            hits++;
        }
    }
    if (!hits) {
        printf("no hits\n");
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    char *qemu_argv[16 + MAX_EXTRA_ARGS];
    int qemu_argc = 0;
    CPUState *cs;
    int i;

    parse_args(argc, argv);

    qemu_argv[qemu_argc++] = argv[0];
    qemu_argv[qemu_argc++] = (char *)"-machine";
    qemu_argv[qemu_argc++] = (char *)"none";
    qemu_argv[qemu_argc++] = (char *)"-accel";
    qemu_argv[qemu_argc++] = (char *)"tcg";
    qemu_argv[qemu_argc++] = (char *)"-cpu";
    qemu_argv[qemu_argc++] = (char *)cfg.cpu_model;
    qemu_argv[qemu_argc++] = (char *)"-display";
    qemu_argv[qemu_argc++] = (char *)"none";
    qemu_argv[qemu_argc++] = (char *)"-monitor";
    qemu_argv[qemu_argc++] = (char *)"none";
    qemu_argv[qemu_argc++] = (char *)"-serial";
    qemu_argv[qemu_argc++] = (char *)"none";
    /* never let the vCPU thread run on its own, we drive it by hand */
    qemu_argv[qemu_argc++] = (char *)"-S";
    qemu_argv[qemu_argc++] = (char *)"-covrec";
    qemu_argv[qemu_argc++] = (char *)cfg.covrec;
    for (i = 0; i < cfg.nextra; i++) {
        qemu_argv[qemu_argc++] = cfg.extra[i];
    }
    qemu_argv[qemu_argc] = NULL;

    /* brings up the machine, the CPU and the coverage recording buffers */
    qemu_init(qemu_argc, qemu_argv);
    /* qemu_init() returns with the BQL and the replay mutex held */

    cs = first_cpu;
    if (!cs) {
        fatal("no CPU was created");
    }

    /* our synthetic page is not part of any sane whitelist */
    whitelist_pa_ranges = NULL;
    num_whitelist_pa_ranges = 0;

    setup_guest_page();
    setup_cpu_state(cs);

    /*
     * Note: the hitmap is allocated, cleared and enabled by
     * init_coverage_recording() according to the -covrec options above.
     */

    if (cfg.verbose) {
        fprintf(stderr, "executing one instruction at 0x%" PRIx64 "\n",
                get_pc(cs));
    }

    /*
     * Bringing up the memory map queued asynchronous work for the vCPU
     * thread (tcg_commit_cpu(), TLB flushes, ...).  Without it the address
     * space dispatch tables of the CPU are not set up at all.  Push a dummy
     * work item through the queue and wait for it: the (otherwise parked)
     * vCPU thread drains everything in front of it while doing so.
     */
    run_on_cpu(cs, nop_work, RUN_ON_CPU_NULL);

    /* from here on we pretend to be the vCPU thread */
    current_cpu = cs;

    /*
     * The vCPU got kicked while the machine was brought up in the stopped
     * state ("-S") and by the work items above.  Clear the pending exit
     * request, otherwise the translation block bails out at its entry.
     */
    qatomic_set(&cs->exit_request, 0);
    qatomic_set(&cs->neg.icount_decr.u16.high, 0);

    /*
     * cpu_exec_step_atomic() generates a translation block limited to a
     * single instruction, runs it in an exclusive context and returns -
     * which is exactly the "execute one instruction" primitive we need
     * here.  It has to run without the BQL held.
     */
    replay_mutex_unlock();
    bql_unlock();

    cpu_exec_step_atomic(cs);

    bql_lock();

    if (cfg.verbose) {
        fprintf(stderr, "pc after execution: 0x%" PRIx64
                " (exception_index=%d)\n", get_pc(cs), cs->exception_index);
    }

    dump_hitmap(cs);

    /*
     * Do not bother with an orderly shutdown, there is nothing to flush and
     * the vCPU thread is still parked in its idle loop.
     */
    exit(0);
}

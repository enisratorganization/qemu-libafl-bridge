#include "qemu/osdep.h"
#include "libafl/gdb.h"
#include "gdbstub/internals.h"
#include "gdbstub/commands.h"

#ifndef CONFIG_USER_ONLY
#include "libafl/exit.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"
#endif

static struct libafl_custom_gdb_cmd* libafl_qemu_gdb_cmds;

#ifndef CONFIG_USER_ONLY
static bool libafl_gdb_hold = false;
static bool libafl_gdb_continue_seen = false;

bool libafl_qemu_gdb_hold_continue(void)
{
    if (libafl_gdb_hold) {
        libafl_gdb_continue_seen = true;
        return true;
    }
    return false;
}

bool libafl_qemu_gdb_wait_for_continue(void)
{
    assert(!runstate_is_running());

    // The exit request of the last executed breakpoint is stale here (it is
    // only cleared on the next vm_start); clear it so the loop below waits for
    // a real exit request (e.g. SIGINT) instead of returning immediately.
    libafl_exit_signal_vm_start();

    libafl_gdb_hold = true;
    libafl_gdb_continue_seen = false;

    while (!libafl_gdb_continue_seen && !libafl_exit_asap()) {
        main_loop_wait(false);
    }

    libafl_gdb_hold = false;

    return libafl_gdb_continue_seen;
}
#endif

void libafl_qemu_add_gdb_cmd(bool (*callback)(void*, uint8_t*, size_t),
                             void* data)
{
    struct libafl_custom_gdb_cmd* c =
        malloc(sizeof(struct libafl_custom_gdb_cmd));
    c->callback = callback;
    c->data = data;
    c->next = libafl_qemu_gdb_cmds;
    libafl_qemu_gdb_cmds = c;
}

void libafl_qemu_gdb_reply(const uint8_t* buf, size_t len)
{
    g_autoptr(GString) hex_buf = g_string_new("O");
    gdb_memtohex(hex_buf, buf, len);
    gdb_put_packet(hex_buf->str);
}

bool libafl_qemu_gdb_exec(void)
{
    struct libafl_custom_gdb_cmd** c = &libafl_qemu_gdb_cmds;
    bool recognized = false;
    while (*c) {
        recognized |= (*c)->callback((*c)->data, gdbserver_state.mem_buf->data,
                                     gdbserver_state.mem_buf->len);
        c = &(*c)->next;
    }
    return recognized;
}

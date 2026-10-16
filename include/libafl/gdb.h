#pragma once

#include "qemu/osdep.h"

struct libafl_custom_gdb_cmd {
    bool (*callback)(void*, uint8_t*, size_t);
    void* data;
    struct libafl_custom_gdb_cmd* next;
};

void libafl_qemu_add_gdb_cmd(bool (*callback)(void*, uint8_t*, size_t),
                             void* data);
void libafl_qemu_gdb_reply(const uint8_t* buf, size_t len);
bool libafl_qemu_gdb_exec(void);

/*
 * While the LibAFL caller waits in libafl_qemu_gdb_wait_for_continue(), the VM
 * must not be started by the gdbstub: swallow continue/step requests from the
 * GDB client instead (the caller resumes the VM itself afterwards).
 * Returns true if the request was swallowed.
 */
bool libafl_qemu_gdb_hold_continue(void);
/*
 * Block the caller (with the BQL held, the VM stopped) while serving the main
 * loop, hence the gdbstub, until an attached GDB client requests a
 * continue/step - or until an exit is requested (e.g. SIGINT).
 * This call starts the gdbserver (tcp::1234) first, so it must be called only
 * after the preparation sequence: the socket is not listening before, hence a
 * client cannot attach early and set breakpoints that would hit during the
 * preparation sequence. The attaching client learns that the VM is stopped
 * via the usual '?' handshake; its continue releases the hold.
 * Returns true if the client requested a continue, false if an exit was
 * requested while waiting (or the gdbserver could not be started).
 */
bool libafl_qemu_gdb_wait_for_continue(void);

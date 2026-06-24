
#include "qemu/osdep.h"

#include "qemu/error-report.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "qemu/main-loop.h"
#include "block/qdict.h"
#include "libafl/system.h"
#include "monitor/monitor.h"
#include "system/block-backend.h"



#define NOT_DONE 0x7fffffff

static void blk_rw_done(void *opaque, int ret)
{
    *(int *)opaque = ret;
}

int libafl_blk_write(BlockBackend *blk, void *buf, int64_t offset, int64_t sz)
{
	void *pattern_buf = NULL;
	QEMUIOVector qiov;
	int async_ret = NOT_DONE;

	qemu_iovec_init(&qiov, 1);
	qemu_iovec_add(&qiov, buf, sz);

	blk_aio_pwritev(blk, offset, &qiov, 0, blk_rw_done, &async_ret);
	while (async_ret == NOT_DONE) {
		main_loop_wait(false);
	}

	//printf("async_ret: %d\n", async_ret);
	//g_assert(async_ret == 0);

	g_free(pattern_buf);
	qemu_iovec_destroy(&qiov);
    return async_ret;
}

void hmp_libafl_blk_write(Monitor *mon, const QDict *qdict)
{
    const char *device = qdict_get_str(qdict, "device");
    const char *file = qdict_get_str(qdict, "file");
    BlockBackend *blk;
    char *buf;
    uint64_t size;
    GError *gerr = NULL;

    blk = blk_by_name(device);
    if (!blk) {
        error_report("Could not find block device '%s'", device);
        return;
    }

    if (!g_file_get_contents(file, &buf, (gsize *)&size, &gerr)) {
        if (gerr) {
            error_report("%s", gerr->message);
            g_error_free(gerr);
        } else {
            error_report("Failed to read file '%s'", file);
        }
        return;
    }

    int ret = libafl_blk_write(blk, buf, 0, size);
    g_free(buf);

    if (ret != 0) {
        error_report("Failed to write %lu bytes to block device '%s': %d",
                     size, device, ret);
    }
}
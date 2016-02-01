/*
 * QEMU Block driver for OpenvStorage
 *
 * Copyright (C) 2015-2016 iNuron NV
 *
 * Authors:
 *  Chrysostomos Nanakos (cnanakos@openvstorage.com)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/*
 * VM Image on OpenvStorage volume is specified like this:
 *
 * file.driver=openvstorage,file.volume=<volumename>
 *
 * or
 *
 * file=openvstorage:<volume_name>
 * file=openvstorage:<volume_name>[:snapshot-timeout=<timeout>]
 *
 * 'openvstorage' is the protocol.
 *
 * Examples:
 *
 * file.driver=openvstorage,file.volume=my_vm_volume
 * file.driver=openvstorage,file.volume=my_vm_volume, \
 * file.snapshot-timeout=120
 *
 * or
 *
 * file=openvstorage:my_vm_volume
 * file=openvstorage:my_vm_volume:snapshot-timeout=120
 *
 */
#include "block/block_int.h"
#include "qapi/qmp/qint.h"
#include "qapi/qmp/qstring.h"
#include "qapi/qmp/qjson.h"
#include "qemu/error-report.h"
#include "qemu/atomic.h"

#include <openvstorage/volumedriver.h>

#define MAX_REQUEST_SIZE        32768
#define OVS_MAX_SNAPS           100
#define OVS_DFL_SNAP_TIMEOUT    120
/* options related */
#define OVS_OPT_VOLUME          "volume"
#define OVS_OPT_SNAP_TIMEOUT    "snapshot-timeout"

typedef enum {
    OVS_OP_READ,
    OVS_OP_WRITE,
    OVS_OP_FLUSH,
} OpenvStorageCmd;

typedef struct OpenvStorageAIOCB {
    BlockAIOCB common;
    QEMUBH *bh;
    struct BDRVOpenvStorageState *s;
    QEMUIOVector *qiov;
    OpenvStorageCmd cmd;
    uint64_t size;
    ssize_t ret;
} OpenvStorageAIOCB;

typedef struct BDRVOpenvStorageState {
    OpenvStorageAIOCB *event_acb;
    ovs_ctx_t *ctx;
    char *volume_name;
    int snapshot_timeout;
} BDRVOpenvStorageState;

typedef struct OpenvStorageAIOSegregatedReq
{
    uint64_t count;
    uint64_t total;
    int ref;
    bool one_or_many_failed_after;
    bool failed;
} OpenvStorageAIOSegregatedReq;

typedef struct OpenvStorageAIORequest {
    struct ovs_aiocb *aiocbp;
    ovs_buffer_t *ovs_buffer;
    OpenvStorageAIOCB *aio_cb;
    ssize_t ret;
    uint64_t pos;
    OpenvStorageAIOSegregatedReq *seg_request;
} OpenvStorageAIORequest;

static void qemu_openvstorage_complete_aio(void *opaque);

static void openvstorage_finish_aiocb(ovs_completion_t *completion, void *arg)
{
    OpenvStorageAIORequest *aio_request = (OpenvStorageAIORequest*) arg;
    OpenvStorageAIOCB *aio_cb = aio_request->aio_cb;
    OpenvStorageAIOSegregatedReq *f = aio_request->seg_request;
    struct ovs_aiocb *aiocbp = aio_request->aiocbp;

    if (aio_cb->cmd != OVS_OP_FLUSH)
    {
        aio_request->ret = ovs_aio_return(aio_cb->s->ctx,
                                          aiocbp);
        ovs_aio_finish(aio_cb->s->ctx,
                       aiocbp);
    }
    else
    {
        aio_request->ret = ovs_aio_return_completion(completion);
    }
    ovs_aio_release_completion(completion);

    if (aio_request->ret == -1)
    {
        f->one_or_many_failed_after = true;
    }

    if (aio_cb->cmd == OVS_OP_READ && aio_request->ret != -1)
    {
        qemu_iovec_from_buf(aio_cb->qiov,
                            aio_request->pos,
                            aiocbp->aio_buf,
                            aio_request->ret);
    }

    if (aio_cb->cmd != OVS_OP_FLUSH)
    {
        ovs_deallocate(aio_cb->s->ctx,
                       aio_request->ovs_buffer);
    }

    if (aio_request->ret > 0)
    {
        atomic_add(&f->count, aio_request->ret);
    }
    if (atomic_fetch_dec(&f->ref) == 1)
    {
        if (!f->failed)
        {
            if (f->one_or_many_failed_after)
            {
                aio_cb->ret = -EIO;
            }
            else
            {
                aio_cb->ret = f->count;
                if ((aio_cb->ret < f->total) && (aio_cb->cmd == OVS_OP_READ))
                {
                    memset(aio_cb->qiov + aio_cb->ret,
                           0,
                           f->total - aio_cb->ret);
                }
                else if (aio_cb->ret != f->total)
                {
                    aio_cb->ret = -EIO;
                }
                else
                {
                    aio_cb->ret = 0;
                }
            }
            aio_cb->bh = aio_bh_new(
                    bdrv_get_aio_context(aio_cb->common.bs),
                    qemu_openvstorage_complete_aio, aio_request);
            qemu_bh_schedule(aio_cb->bh);
        }
        else
        {
            g_free(aiocbp);
            g_free(aio_request);
        }
        g_free(f);
    }
    else
    {
        g_free(aiocbp);
        g_free(aio_request);
    }
}

static void qemu_openvstorage_complete_aio(void *opaque)
{
    OpenvStorageAIORequest *aio_request = (OpenvStorageAIORequest*) opaque;
    OpenvStorageAIOCB *aio_cb = aio_request->aio_cb;
    struct ovs_aiocb *aiocbp = aio_request->aiocbp;

    qemu_bh_delete(aio_cb->bh);
    aio_cb->common.cb(aio_cb->common.opaque, aio_cb->ret);

    qemu_aio_unref(aio_cb);
    g_free(aiocbp);
    g_free(aio_request);
}

static QemuOptsList openvstorage_runtime_opts = {
    .name = "openvstorage",
    .head = QTAILQ_HEAD_INITIALIZER(openvstorage_runtime_opts.head),
    .desc = {
        {
            .name = OVS_OPT_VOLUME,
            .type = QEMU_OPT_STRING,
            .help = "Name of the volume image",
        },
        {
            .name = OVS_OPT_SNAP_TIMEOUT,
            .type = QEMU_OPT_NUMBER,
            .help = "Timeout for the snapshot to be synced on the backend",
        },
        {/* end of list */}
    },
};

static void
openvstorage_parse_filename_opts(const char *filename,
                                 Error **errp,
                                 char **volume,
                                 gpointer *snapshot_timeout)
{
    const char *start, *a;
    char *endptr;
    char *tokens[2], *ds;
    int timeout;

    strstart(filename, "openvstorage:", &start);
    ds = g_strdup(start);
    tokens[0] = strtok(ds, ":");
    tokens[1] = strtok(NULL, "\0");

    if (!strlen(tokens[0]))
    {
        error_setg(errp, "volume name must be specified");
        g_free(ds);
        return;
    }

    *volume = g_strdup(tokens[0]);
    if (tokens[1] != NULL && strstart(tokens[1], OVS_OPT_SNAP_TIMEOUT"=", &a))
    {
        if (strlen(a) > 0)
        {
            timeout = strtoul(a, &endptr, 10);
            if (strlen(endptr))
            {
                return;
            }
            *snapshot_timeout = GINT_TO_POINTER(timeout);
        }
    }
}

static void qemu_openvstorage_parse_filename(const char *filename,
                                             QDict *options,
                                             Error **errp)
{
    const char *start;
    char *volume = NULL;
    gpointer snapshot_timeout = NULL;

    if (qdict_haskey(options, OVS_OPT_VOLUME)
            || qdict_haskey(options, OVS_OPT_SNAP_TIMEOUT))
    {
        error_setg(errp, "volume/stimeout and a filename may not"
                         " be specified at the same time");
        return;
    }

    if (!strstart(filename, "openvstorage:", &start))
    {
        error_setg(errp, "Filename must start with 'openvstorage:'");
        return;
    }

    if (!strlen(start))
    {
        error_setg(errp, "volume name must be specified");
        return;
    }

    openvstorage_parse_filename_opts(filename,
                                     errp,
                                     &volume,
                                     &snapshot_timeout);
    if (volume)
    {
        qdict_put(options,
                  OVS_OPT_VOLUME,
                  qstring_from_str(volume));
        g_free(volume);
    }
    if (snapshot_timeout)
    {
        qdict_put(options,
                  OVS_OPT_SNAP_TIMEOUT,
                  qint_from_int(GPOINTER_TO_INT(snapshot_timeout)));
    }
}

static int
qemu_openvstorage_open(BlockDriverState *bs,
                       QDict *options,
                       int flags,
                       Error **errp)
{
    int ret = 0;
    QemuOpts *opts;
    Error *local_err = NULL;
    const char *volume_name;
    BDRVOpenvStorageState *s = bs->opaque;

    opts = qemu_opts_create(&openvstorage_runtime_opts, NULL, 0, &error_abort);
    qemu_opts_absorb_qdict(opts, options, &local_err);

    if (local_err)
    {
        error_propagate(errp, local_err);
        ret = -EINVAL;
        goto err_exit;
    }

    volume_name = qemu_opt_get(opts, OVS_OPT_VOLUME);
    s->ctx = ovs_ctx_init(volume_name, O_RDWR);
    if (s->ctx == NULL)
    {
        ret = -errno;
        error_setg(errp, "cannot create OpenvStorage context");
        goto err_exit;
    }
    else
    {
        s->volume_name = g_strdup(volume_name);
        s->snapshot_timeout = qemu_opt_get_number(opts,
                                                  OVS_OPT_SNAP_TIMEOUT,
                                                  OVS_DFL_SNAP_TIMEOUT);
    }
    return 0;
err_exit:
    g_free(s->volume_name);
    qemu_opts_del(opts);
    return ret;
}

static void
qemu_openvstorage_close(BlockDriverState *bs)
{
    BDRVOpenvStorageState *s = bs->opaque;
    assert(s->ctx);
    g_free(s->volume_name);
    ovs_ctx_destroy(s->ctx);
}

static int64_t
qemu_openvstorage_getlength(BlockDriverState *bs)
{
    BDRVOpenvStorageState *s = bs->opaque;
    assert(s->ctx);
    struct stat st;
    int ret = ovs_stat(s->ctx, &st);
    if (ret < 0)
    {
        return ret;
    }
    return st.st_size;
}

static QemuOptsList openvstorage_create_opts = {
    .name = "openvstorage-create-opts",
    .head = QTAILQ_HEAD_INITIALIZER(openvstorage_create_opts.head),
    .desc = {
        {
            .name = BLOCK_OPT_SIZE,
            .type = QEMU_OPT_SIZE,
            .help = "Virtual disk size"
        },
        {/* end if list */}
    }
};

static int
qemu_openvstorage_create(const char* filename,
                         QemuOpts *opts,
                         Error **errp)
{
    char *volume_name = NULL;
    const char *start;
    gpointer stimeout = NULL;
    uint64_t size = 0;
    int ret;

    if (!strstart(filename, "openvstorage:", &start))
    {
        error_setg(errp, "Filename must start with 'openvstorage:'");
        return -EINVAL;
    }

    if (!strlen(start))
    {
        error_setg(errp, "volume name must be specified");
        return -EINVAL;
    }

    openvstorage_parse_filename_opts(filename,
                                     errp,
                                     &volume_name,
                                     &stimeout);

    size = ROUND_UP(qemu_opt_get_size_del(opts, BLOCK_OPT_SIZE, 0),
                    BDRV_SECTOR_SIZE);

    if (size > 0 && volume_name != NULL)
    {
        ret = ovs_create_volume(volume_name,
                                size);
        if (ret < 0)
        {
            error_setg(errp, "cannot create volume");
            ret = -errno;
        }
    }
    else
    {
        ret = -EINVAL;
    }
    return ret;
}

static const AIOCBInfo openvstorage_aiocb_info = {
    .aiocb_size = sizeof(OpenvStorageAIOCB),
};

static int
qemu_openvstorage_submit_aio_request(BlockDriverState *bs,
                                     uint64_t pos,
                                     uint64_t size,
                                     off_t offset,
                                     OpenvStorageAIOCB *aio_cb,
                                     OpenvStorageAIOSegregatedReq *f,
                                     OpenvStorageCmd cmd)
{
    BDRVOpenvStorageState *s = bs->opaque;
    int ret;
    ovs_buffer_t *ovs_buf = NULL;
    void *buf = NULL;

    if (cmd != OVS_OP_FLUSH)
    {
        ovs_buf = ovs_allocate(s->ctx,
                               size);
        if (ovs_buf == NULL)
        {
            error_report("%s: cannot allocate shm buffer", __func__);
            goto failed_on_allocation;
        }
        buf = ovs_buffer_data(ovs_buf);
    }

    if (cmd == OVS_OP_WRITE)
    {
        qemu_iovec_to_buf(aio_cb->qiov,
                          pos,
                          buf,
                          size);
    }

    struct ovs_aiocb *aiocbp = g_new(struct ovs_aiocb, 1);
    aiocbp->aio_buf = buf;
    aiocbp->aio_nbytes = size;
    aiocbp->aio_offset = offset;

    OpenvStorageAIORequest *aio_request = g_new(OpenvStorageAIORequest, 1);
    aio_request->aiocbp = aiocbp;
    aio_request->aio_cb = aio_cb;
    aio_request->ovs_buffer = ovs_buf;
    aio_request->pos = pos;
    aio_request->seg_request = f;

    ovs_completion_t *completion =
        ovs_aio_create_completion((ovs_callback_t) openvstorage_finish_aiocb,
                                  (void*)aio_request);

    if (completion == NULL)
    {
        error_report("%s: could not create completion", __func__);
        goto failed_on_completion;
    }

    switch (cmd)
    {
    case OVS_OP_WRITE:
        ret = ovs_aio_writecb(s->ctx, aiocbp, completion);
        break;
    case OVS_OP_READ:
        ret = ovs_aio_readcb(s->ctx, aiocbp, completion);
        break;
    case OVS_OP_FLUSH:
        ret = ovs_aio_flushcb(s->ctx, completion);
        break;
    default:
        ret = -EINVAL;
    }

    if (ret < 0)
    {
        goto err_exit;
    }

    return 0;

err_exit:
    error_report("%s: failed to submit aio request", __func__);
    ovs_aio_release_completion(completion);
failed_on_completion:
    ovs_deallocate(s->ctx,
                   ovs_buf);
    g_free(aiocbp);
    g_free(aio_request);
failed_on_allocation:
    return -EIO;
}

static int
qemu_openvstorage_aio_segregated_rw(BlockDriverState *bs,
                                    uint64_t size,
                                    off_t offset,
                                    OpenvStorageAIOCB *aio_cb,
                                    OpenvStorageCmd cmd)
{
    int ret, requests_nr;
    uint64_t pos = 0;
    OpenvStorageAIOSegregatedReq *seg_request;

    seg_request = g_new0(OpenvStorageAIOSegregatedReq, 1);

    if (cmd == OVS_OP_FLUSH)
    {
        requests_nr = 1;
    }
    else
    {
        requests_nr = (int)(size / MAX_REQUEST_SIZE) + \
                      ((size % MAX_REQUEST_SIZE) ? 1 : 0);
    }
    seg_request->total = size;
    atomic_mb_set(&seg_request->ref, requests_nr);

    while (requests_nr > 1)
    {
        ret = qemu_openvstorage_submit_aio_request(bs,
                                                   pos,
                                                   MAX_REQUEST_SIZE,
                                                   offset + pos,
                                                   aio_cb,
                                                   seg_request,
                                                   cmd);
        if (ret < 0)
        {
            goto err_exit;
        }
        size -= MAX_REQUEST_SIZE;
        pos += MAX_REQUEST_SIZE;
        requests_nr--;
    }
    ret = qemu_openvstorage_submit_aio_request(bs,
                                               pos,
                                               size,
                                               offset + pos,
                                               aio_cb,
                                               seg_request,
                                               cmd);
    if (ret < 0)
    {
        goto err_exit;
    }
    return 0;

err_exit:
    seg_request->failed = true;
    if (atomic_fetch_sub(&seg_request->ref, requests_nr) == requests_nr)
    {
        g_free(seg_request);
    }
    return ret;
}

static BlockAIOCB *qemu_openvstorage_aio_rw(BlockDriverState *bs,
                                            int64_t sector_num,
                                            QEMUIOVector *qiov,
                                            int nb_sectors,
                                            BlockCompletionFunc *cb,
                                            void *opaque,
                                            OpenvStorageCmd cmd)
{
    int ret;
    int64_t size, offset;
    OpenvStorageAIOCB *aio_cb;
    BDRVOpenvStorageState *s = bs->opaque;

    aio_cb = qemu_aio_get(&openvstorage_aiocb_info, bs, cb, opaque);
    aio_cb->cmd = cmd;
    aio_cb->qiov = qiov;
    aio_cb->ret = 0;
    aio_cb->s = s;

    size = nb_sectors * BDRV_SECTOR_SIZE;
    offset = sector_num * BDRV_SECTOR_SIZE;
    aio_cb->size = size;

    ret = qemu_openvstorage_aio_segregated_rw(bs,
                                              size,
                                              offset,
                                              aio_cb,
                                              cmd);
    if (ret < 0)
    {
        goto err_exit;
    }
    return &aio_cb->common;

err_exit:
    error_report("%s: I/O error", __func__);
    qemu_aio_unref(aio_cb);
    return NULL;
}

static BlockAIOCB *qemu_openvstorage_aio_readv(BlockDriverState *bs,
                                               int64_t sector_num,
                                               QEMUIOVector *qiov,
                                               int nb_sectors,
                                               BlockCompletionFunc *cb,
                                               void *opaque)
{
    return qemu_openvstorage_aio_rw(bs,
                                    sector_num,
                                    qiov,
                                    nb_sectors,
                                    cb,
                                    opaque,
                                    OVS_OP_READ);
};

static BlockAIOCB *qemu_openvstorage_aio_writev(BlockDriverState *bs,
                                                int64_t sector_num,
                                                QEMUIOVector *qiov,
                                                int nb_sectors,
                                                BlockCompletionFunc *cb,
                                                void *opaque)
{
    return qemu_openvstorage_aio_rw(bs,
                                    sector_num,
                                    qiov,
                                    nb_sectors,
                                    cb,
                                    opaque,
                                    OVS_OP_WRITE);
};

static BlockAIOCB *qemu_openvstorage_aio_flush(BlockDriverState *bs,
                                               BlockCompletionFunc *cb,
                                               void *opaque)
{
    return qemu_openvstorage_aio_rw(bs,
                                    0,
                                    NULL,
                                    0,
                                    cb,
                                    opaque,
                                    OVS_OP_FLUSH);
}

static int qemu_openvstorage_snap_create(BlockDriverState *bs,
                                         QEMUSnapshotInfo *sn_info)
{
    BDRVOpenvStorageState *s = bs->opaque;
    int ret;

    if (sn_info->name[0] == '\0')
    {
        return -EINVAL;
    }

    if (sn_info->id_str[0] != '\0' &&
        strcmp(sn_info->id_str, sn_info->name) != 0)
    {
        return -EINVAL;
    }

    if (strlen(sn_info->name) >= sizeof(sn_info->id_str))
    {
        return -ERANGE;
    }

    ret = ovs_snapshot_create(s->volume_name,
                              sn_info->name,
                              s->snapshot_timeout);
    if (ret < 0)
    {
        ret = -errno;
        error_report("failed to create snapshot: %s", strerror(errno));
    }
    return ret;
}

static int qemu_openvstorage_snap_remove(BlockDriverState *bs,
                                         const char *snapshot_id,
                                         const char *snapshot_name,
                                         Error **errp)
{
    BDRVOpenvStorageState *s = bs->opaque;
    int ret;

    if (!snapshot_name)
    {
        error_setg(errp, "openvstorage needs a valid snapshot name");
        return -EINVAL;
    }

    if (snapshot_id && strcmp(snapshot_id, snapshot_name))
    {
        error_setg(errp,
                   "openvstorage doesn't support snapshot id, it should be "
                   "NULL or equal to snapshot name");
        return -EINVAL;
    }

    ret = ovs_snapshot_remove(s->volume_name, snapshot_name);
    if (ret < 0)
    {
        ret = -errno;
        error_setg_errno(errp, errno, "failed to remove snapshot");
    }
    return ret;
}

static int qemu_openvstorage_snap_rollback(BlockDriverState *bs,
                                           const char *snapshot_name)
{
    BDRVOpenvStorageState *s = bs->opaque;
    int ret;

    ret = ovs_snapshot_rollback(s->volume_name, snapshot_name);
    if (ret < 0)
    {
        ret = -errno;
    }
    return ret;
}

static int qemu_openvstorage_snap_list(BlockDriverState *bs,
                                       QEMUSnapshotInfo **psn_tab)
{
    BDRVOpenvStorageState *s = bs->opaque;
    QEMUSnapshotInfo *sn_info, *sn_tab = NULL;
    int i, snap_count;
    ovs_snapshot_info_t *snaps;
    int max_snaps = OVS_MAX_SNAPS;

    do {
        snaps = g_new(ovs_snapshot_info_t, max_snaps);
        snap_count = ovs_snapshot_list(s->volume_name,
                                       snaps,
                                       &max_snaps);
        if (snap_count <= 0)
        {
            g_free(snaps);
        }
    } while (snap_count == -1 && errno == ERANGE);

    if (snap_count <= 0)
    {
        snap_count = -errno;
        goto done;
    }

    sn_tab = g_new0(QEMUSnapshotInfo, snap_count);

    for (i = 0; i < snap_count; i++)
    {
        const char *snap_name = snaps[i].name;

        sn_info = sn_tab + i;
        pstrcpy(sn_info->id_str, sizeof(sn_info->id_str), snap_name);
        pstrcpy(sn_info->name, sizeof(sn_info->name), snap_name);

        sn_info->vm_state_size = snaps[i].size;
        sn_info->date_sec = 0;
        sn_info->date_nsec = 0;
        sn_info->vm_clock_nsec = 0;
    }
    ovs_snapshot_list_free(snaps);
    g_free(snaps);
done:
    *psn_tab = sn_tab;
    return snap_count;
}

static BlockDriver bdrv_openvstorage = {
    .format_name         = "openvstorage",
    .protocol_name       = "openvstorage",
    .instance_size       = sizeof(BDRVOpenvStorageState),
    .bdrv_parse_filename = qemu_openvstorage_parse_filename,

    .bdrv_file_open      = qemu_openvstorage_open,
    .bdrv_close          = qemu_openvstorage_close,
    .bdrv_getlength      = qemu_openvstorage_getlength,
    .bdrv_aio_readv      = qemu_openvstorage_aio_readv,
    .bdrv_aio_writev     = qemu_openvstorage_aio_writev,
    .bdrv_aio_flush      = qemu_openvstorage_aio_flush,
    .bdrv_has_zero_init  = bdrv_has_zero_init_1,

    .bdrv_snapshot_create = qemu_openvstorage_snap_create,
    .bdrv_snapshot_delete = qemu_openvstorage_snap_remove,
    .bdrv_snapshot_list   = qemu_openvstorage_snap_list,
    .bdrv_snapshot_goto   = qemu_openvstorage_snap_rollback,

    .bdrv_create         = qemu_openvstorage_create,
    .create_opts         = &openvstorage_create_opts,
};

static void bdrv_openvstorage_init(void)
{
    bdrv_register(&bdrv_openvstorage);
}

block_init(bdrv_openvstorage_init);

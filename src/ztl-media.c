/* xZTL: Zone Translation Layer User-space Library
 *
 * Copyright 2019 Samsung Electronics
 *
 * Written by Ivan L. Picoli <i.picoli@samsung.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#include <xztl.h>
#include <xztl-media.h>
#include <ztl-media.h>
#include <libxnvme.h>
#include <time.h>
#include <unistd.h>
#include <libznd.h>
#include <pthread.h>
#include <sys/queue.h>
#include <sched.h>

static struct znd_media zndmedia;

inline struct znd_media *get_znd_media(void) {
    return &zndmedia;
}


extern char *dev_name;

static pthread_spinlock_t cb_spin;
static STAILQ_HEAD(callback_head, xztl_io_mcmd) cb_head;

static void znd_media_async_cb(struct xnvme_req *xreq, void *cb_arg) {
    struct xztl_io_mcmd *cmd;
    uint16_t sec_i = 0;

    cmd = (struct xztl_io_mcmd *) cb_arg;
    cmd->status = xnvme_req_cpl_status(xreq);

    if (!cmd->status && cmd->opcode == XZTL_ZONE_APPEND)
        cmd->paddr[sec_i] = *(uint64_t *) &xreq->cpl.cdw0; // NOLINT

    if (cmd->opcode == XZTL_CMD_WRITE)
        cmd->paddr[sec_i] = cmd->addr[sec_i].g.sect;

    if (cmd->status) {
        xztl_print_mcmd(cmd);
        xnvme_req_pr(xreq, 0);
    }

    pthread_spin_lock(&cb_spin);
    STAILQ_INSERT_TAIL(&cb_head, cmd, entry);
    pthread_spin_unlock(&cb_spin);
}

static int znd_media_submit_read_synch(struct xztl_io_mcmd *cmd) {
    struct xnvme_req *xreq;
    uint64_t slba;
    uint16_t sec_i = 0;
    struct timespec ts_s, ts_e;

    xreq = &cmd->media_ctx;
    xreq->async.ctx    = NULL;
    xreq->async.cb_arg = NULL;

    /* The read path is not group based. It uses only sectors */
    slba = cmd->addr[sec_i].g.sect;

    int ret;

    GET_MICROSECONDS(cmd->us_start, ts_s);
    ret = xnvme_cmd_read(zndmedia.dev,
                          xnvme_dev_get_nsid(zndmedia.dev),
                          slba,
                          (uint16_t) cmd->nsec[sec_i] - 1,
                          (void *) cmd->prp[sec_i], // NOLINT
                          NULL,
                          XNVME_CMD_SYNC,
                          xreq);
    GET_MICROSECONDS(cmd->us_end, ts_e);

    /* WARNING: Uncommenting this line causes performance drop */
    // xztl_prometheus_add_read_latency (cmd->us_end - cmd->us_start);

    if (ret)
        xztl_print_mcmd(cmd);

    return ret;
}

static int znd_media_submit_read_asynch(struct xztl_io_mcmd *cmd) {
    uint16_t sec_i = 0;
    uint64_t slba;
    void *dbuf;
    struct xztl_mthread_ctx *tctx;
    struct xnvme_req *xreq;

    tctx = cmd->async_ctx;
    xreq = &cmd->media_ctx;

    dbuf = (void *) cmd->prp[sec_i]; // NOLINT

    /* The read path is not group based. It uses only sectors */
    slba = cmd->addr[sec_i].g.sect;

    xreq->async.ctx    = tctx->asynch;
    xreq->async.cb     = znd_media_async_cb;
    xreq->async.cb_arg = (void *) cmd; // NOLINT

    return xnvme_cmd_read(zndmedia.dev,
                           xnvme_dev_get_nsid(zndmedia.dev),
                           slba,
                           (uint16_t) cmd->nsec[sec_i] - 1,
                           dbuf,
                           NULL,
                           XNVME_CMD_ASYNC,
                           xreq);
}

static int znd_media_submit_write_synch(struct xztl_io_mcmd *cmd) {
    uint16_t sec_i = 0;
    uint64_t slba;
    void *dbuf;
    struct xztl_mthread_ctx *tctx;
    struct xnvme_req *xreq;
    int ret;

    tctx = cmd->async_ctx;
    xreq = &cmd->media_ctx;

    dbuf = (void *) cmd->prp[sec_i]; // NOLINT

    /* The write path is not group based. It uses only sectors */
    slba = cmd->addr[sec_i].g.sect;

    xreq->async.ctx    = tctx->asynch;
    xreq->async.cb     = NULL;
    xreq->async.cb_arg = NULL; // NOLINT

    pthread_spin_lock(&tctx->qpair_spin);

    ret = xnvme_cmd_write(zndmedia.dev,
                           xnvme_dev_get_nsid(zndmedia.dev),
                           slba,
                           (uint16_t) cmd->nsec[sec_i] - 1,
                           dbuf,
                           NULL,
                           XNVME_CMD_SYNC,
                           xreq);

    pthread_spin_unlock(&tctx->qpair_spin);

    if (ret)
        xztl_print_mcmd(cmd);

    return ret;
}

static int znd_media_submit_write_asynch(struct xztl_io_mcmd *cmd) {
    uint16_t sec_i = 0;
    uint64_t slba;
    void *dbuf;
    struct xztl_mthread_ctx *tctx;
    struct xnvme_req *xreq;
    int ret;

    tctx = cmd->async_ctx;
    xreq = &cmd->media_ctx;

    dbuf = (void *) cmd->prp[sec_i]; // NOLINT

    /* The write path is not group based. It uses only sectors */
    slba = cmd->addr[sec_i].g.sect;

    xreq->async.ctx    = tctx->asynch;
    xreq->async.cb     = znd_media_async_cb;
    xreq->async.cb_arg = (void *) cmd; // NOLINT

    pthread_spin_lock(&tctx->qpair_spin);

    ret = xnvme_cmd_write(zndmedia.dev,
                           xnvme_dev_get_nsid(zndmedia.dev),
                           slba,
                           (uint16_t) cmd->nsec[sec_i] - 1,
                           dbuf,
                           NULL,
                           XNVME_CMD_ASYNC,
                           xreq);

    pthread_spin_unlock(&tctx->qpair_spin);

    if (ret)
        xztl_print_mcmd(cmd);

    return ret;
}

static int znd_media_submit_append_synch(struct xztl_io_mcmd *cmd) {
    return ZND_INVALID_OPCODE;
}

static int znd_media_submit_append_asynch(struct xztl_io_mcmd *cmd) {
    uint16_t zone_i = 0;
    uint64_t zlba;
    const void *dbuf;
    struct xztl_mthread_ctx *tctx;
    struct xnvme_req *xreq;
    int ret;

    tctx      = cmd->async_ctx;
    xreq      = &cmd->media_ctx;

    dbuf = (const void *) cmd->prp[zone_i];

    /* The write path separates zones into groups */
    zlba = (zndmedia.media.geo.zn_grp * cmd->addr[zone_i].g.grp +
            cmd->addr[zone_i].g.zone) * zndmedia.devgeo->nsect;

    xreq->async.ctx    = tctx->asynch;
    xreq->async.cb     = znd_media_async_cb;
    xreq->async.cb_arg = (void *) cmd; // NOLINT

    ret = (!XZTL_WRITE_APPEND) ? znd_cmd_append(zndmedia.dev,
                                 xnvme_dev_get_nsid(zndmedia.dev),
                                 zlba,
                                 (uint16_t) cmd->nsec[zone_i] - 1,
                                 dbuf,
                                 NULL,
                                 XNVME_CMD_ASYNC,
                                 xreq) :
                                 -1;
    if (ret)
        xztl_print_mcmd(cmd);

    return ret;
}

static int znd_media_submit_io(struct xztl_io_mcmd *cmd) {
    switch (cmd->opcode) {
        case XZTL_ZONE_APPEND:
            return (cmd->synch) ? znd_media_submit_append_synch(cmd) :
                                  znd_media_submit_append_asynch(cmd);
        case XZTL_CMD_READ:
            return (cmd->synch) ? znd_media_submit_read_synch(cmd) :
                                  znd_media_submit_read_asynch(cmd);
        case XZTL_CMD_WRITE:
            return (cmd->synch) ? znd_media_submit_write_synch(cmd) :
                                  znd_media_submit_write_asynch(cmd);
        default:
            return ZND_INVALID_OPCODE;
    }
    return 0;
}

static inline int znd_media_zone_manage(struct xztl_zn_mcmd *cmd, uint8_t op) {
    uint32_t lba;
    struct xnvme_req devreq;
    int ret;

    lba = ( (zndmedia.devgeo->nzone * cmd->addr.g.grp) +
             cmd->addr.g.zone) * zndmedia.devgeo->nsect;

    devreq.async.ctx    = NULL;
    devreq.async.cb_arg = NULL;

    ret = znd_cmd_mgmt_send(zndmedia.dev,
                             xnvme_dev_get_nsid(zndmedia.dev),
                             lba,
                             op,
                             0,
                             NULL,
                             XNVME_CMD_SYNC,
                             &devreq);

    cmd->status = (ret) ? xnvme_req_cpl_status(&devreq) : XZTL_OK;

    // return (ret) ? op : XZTL_OK;
    return ret;
}

static int znd_media_zone_report(struct xztl_zn_mcmd *cmd) {
    struct znd_report *rep;
    size_t limit;
    uint32_t lba;

    // Reading everything until not necessary anymore
    lba   = 0; // ((zndmedia.devgeo->nzone * cmd->addr.g.grp) + cmd->addr.g.zone) * zndmedia.devgeo->nsect;
    limit = 0; // cmd->nzones;
    rep = znd_report_from_dev(zndmedia.dev, lba, limit, 0);
    if (!rep)
        return ZND_MEDIA_REPORT_ERR;

    cmd->opaque = (void *) rep; // NOLINT

    return XZTL_OK;
}

static int znd_media_zone_mgmt(struct xztl_zn_mcmd *cmd) {
    switch (cmd->opcode) {
        case XZTL_ZONE_MGMT_CLOSE:
            return znd_media_zone_manage(cmd, ZND_SEND_CLOSE);
        case XZTL_ZONE_MGMT_FINISH:
            return znd_media_zone_manage(cmd, ZND_SEND_FINISH);
        case XZTL_ZONE_MGMT_OPEN:
            return znd_media_zone_manage(cmd, ZND_SEND_OPEN);
        case XZTL_ZONE_MGMT_RESET:
            xztl_stats_inc(XZTL_STATS_RESET_MCMD, 1);
            return znd_media_zone_manage(cmd, ZND_SEND_RESET);
        case XZTL_ZONE_MGMT_REPORT:
            return znd_media_zone_report(cmd);
        default:
            return ZND_INVALID_OPCODE;
    }

    return XZTL_OK;
}

static void *znd_media_dma_alloc(size_t size, uint64_t *phys) {
    return xnvme_buf_alloc(zndmedia.dev, size, phys);
}

static void znd_media_dma_free(void *ptr) {
    xnvme_buf_free(zndmedia.dev, ptr);
}

static int znd_media_async_poke(struct xnvme_async_ctx *ctx,
                              uint32_t *c, uint16_t max) {
    int ret;
    ret = xnvme_async_poke(zndmedia.dev, ctx, max);
    if (ret < 0)
        return ZND_MEDIA_POKE_ERR;

    *c = ret;

    return XZTL_OK;
}

static int znd_media_async_outs(struct xnvme_async_ctx *ctx, uint32_t *c) {
    int ret;

    ret = xnvme_async_get_outstanding(ctx);
    if (ret < 0)
        return ZND_MEDIA_OUTS_ERR;

    *c = ret;

    return XZTL_OK;
}

static int znd_media_async_wait(struct xnvme_async_ctx *ctx, uint32_t *c) {
    int ret;

    ret = xnvme_async_get_outstanding(ctx);
    if (ret)
        return ZND_MEDIA_WAIT_ERR;

    *c = ret;

    return XZTL_OK;
}

static void *znd_media_asynch_comp_th(void *args) {
    struct xztl_misc_cmd    *cmd_misc;
    struct xztl_io_mcmd     *cmd;
    struct xztl_mthread_ctx *tctx;

#if ZTL_WRITE_AFFINITY
    cpu_set_t cpuset;

    /* Set affinity to writing core */
    CPU_ZERO(&cpuset);
    CPU_SET(ZTL_WRITE_CORE, &cpuset);
    pthread_setaffinity_np (pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif

    cmd_misc   = (struct xztl_misc_cmd *) args;
    tctx       = cmd_misc->asynch.ctx_ptr;

    tctx->comp_active = 1;

    while (tctx->comp_active) {
        usleep(1);

NEXT:
        if (!STAILQ_EMPTY(&cb_head)) {
            pthread_spin_lock(&cb_spin);
            cmd = STAILQ_FIRST(&cb_head);
            if (!cmd) {
                pthread_spin_unlock(&cb_spin);
                continue;
            }

            STAILQ_REMOVE_HEAD(&cb_head, entry);
            pthread_spin_unlock(&cb_spin);

            cmd->callback(cmd);

            goto NEXT;
        }
    }

    return XZTL_OK;
}

static int znd_media_asynch_init(struct xztl_misc_cmd *cmd) {
    struct xztl_mthread_ctx *tctx;
    int ret;

    tctx = cmd->asynch.ctx_ptr;

    ret = xnvme_async_init(zndmedia.dev, &tctx->asynch, cmd->asynch.depth,
                                XNVME_ASYNC_SQPOLL | XNVME_ASYNC_IOPOLL);
    if (ret) {
        return ZND_MEDIA_ASYNCH_ERR;
    }

    STAILQ_INIT(&cb_head);
    if (pthread_spin_init(&cb_spin, 0)) {
        return ZND_MEDIA_ASYNCH_ERR;
    }

    tctx->comp_active = 0;

    if (pthread_create(&tctx->comp_tid,
                        NULL,
                        znd_media_asynch_comp_th,
                        (void *) cmd)) { // NOLINT

        xnvme_async_term(zndmedia.dev, tctx->asynch);
        tctx->asynch = NULL;
        pthread_spin_destroy(&cb_spin);

        return ZND_MEDIA_ASYNCH_TH;
    }

    /* Wait for the thread to start */
    while (!tctx->comp_active) {
        usleep(1);
    }

    return XZTL_OK;
}

static int znd_media_asynch_term(struct xztl_misc_cmd *cmd) {
    int ret;

    /* Join the completion thread (should be terminated by the caller) */
    pthread_join(cmd->asynch.ctx_ptr->comp_tid, NULL);

    ret = xnvme_async_term(zndmedia.dev, cmd->asynch.ctx_ptr->asynch);
    if (ret)
        return ZND_MEDIA_ASYNCH_ERR;

    pthread_spin_destroy(&cb_spin);

    return XZTL_OK;
}

static int znd_media_cmd_exec(struct xztl_misc_cmd *cmd) {
    switch (cmd->opcode) {

    case XZTL_MISC_ASYNCH_INIT:
        return znd_media_asynch_init(cmd);

    case XZTL_MISC_ASYNCH_TERM:
        return znd_media_asynch_term(cmd);

    case XZTL_MISC_ASYNCH_POKE:
        return znd_media_async_poke(
                    cmd->asynch.ctx_ptr->asynch,
                    &cmd->asynch.count,
                    cmd->asynch.limit);

    case XZTL_MISC_ASYNCH_OUTS:
        return znd_media_async_outs(
                    cmd->asynch.ctx_ptr->asynch,
                    &cmd->asynch.count);

    case XZTL_MISC_ASYNCH_WAIT:
        return znd_media_async_wait(
                    cmd->asynch.ctx_ptr->asynch,
                    &cmd->asynch.count);

    default:
        return ZND_INVALID_OPCODE;
    }
}

static int znd_media_init(void) {
    return XZTL_OK;
}

static int znd_media_exit(void) {
    if (zndmedia.dev)
        xnvme_dev_close(zndmedia.dev);

    return XZTL_OK;
}

int znd_media_register(const char *dev_name) {
    const struct xnvme_geo *devgeo;
    struct xnvme_dev *dev;
    struct xztl_media *m;

    dev = xnvme_dev_open(dev_name);
    if (!dev)
        return ZND_MEDIA_NODEVICE;

    devgeo = xnvme_dev_get_geo(dev);
    if (!devgeo) {
        xnvme_dev_close(dev);
        return ZND_MEDIA_NOGEO;
    }

    zndmedia.dev    = dev;
    zndmedia.devgeo = devgeo;
    m = &zndmedia.media;

    m->geo.ngrps         = devgeo->npugrp;
    m->geo.pu_grp        = devgeo->npunit;
    m->geo.zn_pu         = devgeo->nzone;
    m->geo.sec_zn        = devgeo->nsect;
    m->geo.nbytes        = devgeo->nbytes;
    m->geo.nbytes_oob    = devgeo->nbytes_oob;

    m->init_fn   = znd_media_init;
    m->exit_fn   = znd_media_exit;
    m->submit_io = znd_media_submit_io;
    m->zone_fn   = znd_media_zone_mgmt;
    m->dma_alloc = znd_media_dma_alloc;
    m->dma_free  = znd_media_dma_free;
    m->cmd_exec  = znd_media_cmd_exec;

    return xztl_media_set (m);
}

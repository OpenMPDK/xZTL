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
#include <xztl-ztl.h>
#include <ztl.h>
#include <unistd.h>
#include <sched.h>

#define ZTL_MCMD_ENTS	 XZTL_IO_MAX_MCMD

static STAILQ_HEAD(uc_head, xztl_io_ucmd)  ucmd_head;
static pthread_spinlock_t           ucmd_spin;
static struct xztl_mthread_ctx      *tctx;
static pthread_t     wca_thread;
static uint8_t       wca_running;

/* This function checks if the media offsets are sequential.
 * If not, we return a negative value. For now we do not support
 * multi-piece mapping in ZTL-managed mapping */
static int ztl_wca_check_offset_seq(struct xztl_io_ucmd *ucmd) {
    uint32_t off_i;

    for (off_i = 1; off_i < ucmd->nmcmd; off_i++) {
        if (ucmd->moffset[off_i] !=
            ucmd->moffset[off_i - 1] + ucmd->msec[off_i - 1]) {
            return -1;
        }
    }

    return 0;
}

/* This function prepares a multi-piece mapping to return to the user.
 * Each entry contains the offset and size, and the full list represents
 * the entire buffer. */
static void ztl_wca_reorg_ucmd_off(struct xztl_io_ucmd *ucmd) {
    uint32_t off_i, curr, first_off, size;

    curr      = 0;
    first_off = 0;
    size      = 0;

    for (off_i = 1; off_i < ucmd->nmcmd; off_i++) {
        size += ucmd->msec[off_i - 1];

        /* If offset of sector is not sequential to the previousi one */
        if ( (ucmd->moffset[off_i] !=
            ucmd->moffset[off_i - 1] + ucmd->msec[off_i - 1]) ||

        /* Or zone is not the same as the previous one */
            (ucmd->mcmd[off_i]->addr[0].g.zone !=
            ucmd->mcmd[off_i -1]->addr[0].g.zone) ) {

            /* Close the piece and set first offset + size */
            ucmd->moffset[curr] = ucmd->moffset[first_off];
            ucmd->msec[curr]    = size;

            first_off = off_i;
            size = 0;
            curr++;

            /* If this is the last sector, we need to add it to the list */
            if (off_i == ucmd->nmcmd - 1) {
                size += ucmd->msec[first_off];
                ucmd->moffset[curr] = ucmd->moffset[first_off];
                ucmd->msec[curr]    = size;
                curr++;
            }

        /* If this is the last sector and belongs to the previous piece */
        } else if (off_i == ucmd->nmcmd - 1) {

            /* Merge sector to the previous piece */
            size += ucmd->msec[off_i];
            ucmd->moffset[curr] = ucmd->moffset[first_off];
            ucmd->msec[curr]    = size;
            curr++;
        }
    }

    ucmd->noffs = (ucmd->nmcmd > 1) ? curr : 1;
}

static void ztl_wca_callback_mcmd(void *arg) {
    struct xztl_io_ucmd  *ucmd;
    struct xztl_io_mcmd  *mcmd;
    struct app_map_entry map;
    struct app_zmd_entry *zmd;
    uint64_t old;
    int ret, off_i;

    mcmd = (struct xztl_io_mcmd *) arg;
    ucmd = (struct xztl_io_ucmd *) mcmd->opaque;

    pthread_spin_lock(&ucmd->inflight_spin);
    ucmd->minflight[mcmd->sequence_zn] = 0;
    pthread_spin_unlock(&ucmd->inflight_spin);

    if (mcmd->status) {
        ucmd->status = mcmd->status;
    } else {
        ucmd->moffset[mcmd->sequence] = mcmd->paddr[0];
    }

    pthread_spin_lock(&ucmd->inflight_spin);
    xztl_atomic_int16_update(&ucmd->ncb, ucmd->ncb + 1);
    pthread_spin_unlock(&ucmd->inflight_spin);

    if (mcmd->status)
        ZDEBUG(ZDEBUG_WCA, "ztl-wca: Callback. (ID %lu, S %d/%d, C %d, WOFF 0x%lx). St: %d",
                            ucmd->id,
                            mcmd->sequence,
                            ucmd->nmcmd,
                            ucmd->ncb,
                            ucmd->moffset[mcmd->sequence],
                            mcmd->status);

    xztl_mempool_put(mcmd->mp_cmd, XZTL_MEMPOOL_MCMD, ZTL_PRO_TUSER);

    if (ucmd->ncb == ucmd->nmcmd) {
        ucmd->noffs = 0;

        /* Update mapping if managed by the ZTL */
        if (!ucmd->status && !ucmd->app_md) {

            /* Check if media offsets are sequential within the zone
             * For ZTL-managed mapping, we do not support multi-piece entries */
            if (!ztl_wca_check_offset_seq(ucmd)) {
                map.addr     = 0;
                map.g.offset = ucmd->moffset[0];
                map.g.nsec   = ucmd->msec[0];
                map.g.multi  = 0;
                ret = ztl()->map->upsert_fn(ucmd->id, map.addr, &old, 0);
                ztl()->mpe->flush_fn();
                if (ret)
                    ucmd->status = XZTL_ZTL_MAP_ERR;
            } else {
                ucmd->status = XZTL_ZTL_APPEND_ERR;
            }
        }

        /* If command is successfull, reorganize media offsets for multi-piece
         * mapping used by the user application */
        if (!ucmd->status)
            ztl_wca_reorg_ucmd_off(ucmd);

        for (off_i = 0; off_i < ucmd->noffs; off_i++) {
            zmd = ztl()->zmd->get_fn(ucmd->prov->grp, ucmd->moffset[off_i], 1);
            xztl_atomic_int32_update(&zmd->npieces, zmd->npieces + 1);

            if (ZDEBUG_WCA) {
                log_infoa("ztl-wca: off_id %d, moff 0x%lx, nsec %d. "
                            "ZN(%d) pieces: %d\n", off_i, ucmd->moffset[off_i],
                            ucmd->msec[off_i], zmd->addr.g.zone, zmd->npieces);
            }
        }

        ztl()->pro->free_fn(ucmd->prov);

        if (ucmd->callback) {
            ucmd->completed = 1;
            pthread_spin_destroy(&ucmd->inflight_spin);
            ucmd->callback(ucmd);
        } else {
            ucmd->completed = 1;
        }
    }
}

static void ztl_wca_callback(struct xztl_io_mcmd *mcmd) {
    ztl_wca_callback_mcmd(mcmd);
}

static int ztl_wca_submit(struct xztl_io_ucmd *ucmd) {
    pthread_spin_lock(&ucmd_spin);
    STAILQ_INSERT_TAIL(&ucmd_head, ucmd, entry);
    pthread_spin_unlock(&ucmd_spin);

    return 0;
}

static uint32_t ztl_wca_ncmd_prov_based(struct app_pro_addr *prov) {
    uint32_t zn_i, ncmd;

    ncmd = 0;
    for (zn_i = 0; zn_i < prov->naddr; zn_i++) {
        ncmd += prov->nsec[zn_i] / ZTL_WCA_SEC_MCMD;
        if (prov->nsec[zn_i] % ZTL_WCA_SEC_MCMD != 0)
            ncmd++;
    }

    return ncmd;
}

static void ztl_wca_poke_ctx(void) {
    struct xztl_misc_cmd misc;
    misc.opcode		  = XZTL_MISC_ASYNCH_POKE;
    misc.asynch.ctx_ptr   = tctx;
    misc.asynch.limit     = 0;
    misc.asynch.count     = 0;

    if (!xztl_media_submit_misc(&misc)) {
        if (!misc.asynch.count) {
        // We may check outstanding commands here
        }
    }
}

static void ztl_wca_process_ucmd(struct xztl_io_ucmd *ucmd) {
    struct app_pro_addr *prov;
    struct xztl_mp_entry *mp_cmd;
    struct xztl_io_mcmd *mcmd;
    struct xztl_core *core;
    get_xztl_core(&core);
    uint32_t nsec, nsec_zn, ncmd, cmd_i, zn_i, submitted;
    int zn_cmd_id[ZTL_PRO_STRIPE * 2];
    uint64_t boff;
    int ret, ncmd_zn, zncmd_i;

    ZDEBUG(ZDEBUG_WCA, "ztl-wca: Processing user write. ID %lu", ucmd->id);

    nsec = ucmd->size / core->media->geo.nbytes;

    /* We do not support non-aligned buffers */
    if (ucmd->size % (core->media->geo.nbytes * ZTL_WCA_SEC_MCMD_MIN != 0)) {
        log_erra("ztl-wca: Buffer is not aligned to %d bytes: %lu bytes.",
                core->media->geo.nbytes * ZTL_WCA_SEC_MCMD_MIN, ucmd->size);
        goto FAILURE;
    }

    /* First we check the number of commands based on ZTL_WCA_SEC_MCMD */
    ncmd = nsec / ZTL_WCA_SEC_MCMD;
    if (nsec % ZTL_WCA_SEC_MCMD != 0)
        ncmd++;

    if (ncmd > XZTL_IO_MAX_MCMD) {
        log_erra("ztl-wca: User command exceed XZTL_IO_MAX_MCMD. "
                            "%d of %d", ncmd, XZTL_IO_MAX_MCMD);
        goto FAILURE;
    }

    /* Note: Provisioning types are user level metadata, if other
     * types of provisioning are added we need to support it here.
     *
     * Note: We currently not support multi-piece mapping for application-
     * managed metadata/recovery (indicated by ucmd->app_md == 0 */
    prov = ztl()->pro->new_fn(nsec, ucmd->prov_type, ucmd->app_md);
    if (!prov) {
        log_erra("ztl-wca: Provisioning failed. nsec %d, prov_type %d",
                                 nsec, ucmd->prov_type);
        goto FAILURE;
    }

    /* We check the number of commands again based on the provisioning */
    ncmd = ztl_wca_ncmd_prov_based(prov);
    if (ncmd > XZTL_IO_MAX_MCMD) {
        log_erra("ztl-wca: User command exceed XZTL_IO_MAX_MCMD. "
                                "%d of %d", ncmd, XZTL_IO_MAX_MCMD);
        goto FAIL_NCMD;
    }

    ucmd->prov  = prov;
    ucmd->nmcmd = ncmd;
    ucmd->completed = 0;
    ucmd->ncb = 0;

    boff = (uint64_t) ucmd->buf;

    ZDEBUG(ZDEBUG_WCA, "ztl-wca: NMCMD: %d", ncmd);

    for (zn_i = 0; zn_i < ZTL_PRO_STRIPE * 2; zn_i++)
        zn_cmd_id[zn_i] = -1;

    /* Populate media commands */
    cmd_i = 0;
    for (zn_i = 0; zn_i < prov->naddr; zn_i++) {
        ncmd_zn = prov->nsec[zn_i] / ZTL_WCA_SEC_MCMD;
        if (prov->nsec[zn_i] % ZTL_WCA_SEC_MCMD != 0)
            ncmd_zn++;

        nsec_zn = prov->nsec[zn_i];

        for (zncmd_i = 0; zncmd_i < ncmd_zn; zncmd_i++) {

            /* We are using a memory pool for user commands, if other types
             * such as GC in introduced, we need to choose the provisioning
             * type here */
            mp_cmd = xztl_mempool_get(XZTL_MEMPOOL_MCMD, ZTL_PRO_TUSER);
            if (!mp_cmd) {
                log_err("ztl-wca: Mempool failed.");
                goto FAIL_MP;
            }

            mcmd = (struct xztl_io_mcmd *) mp_cmd->opaque;

            memset(mcmd, 0x0, sizeof (struct xztl_io_mcmd));
            mcmd->mp_cmd    = mp_cmd;
            mcmd->opcode    = (XZTL_WRITE_APPEND) ? XZTL_ZONE_APPEND :
                             XZTL_CMD_WRITE;
            mcmd->synch     = 0;
            mcmd->submitted = 0;
            mcmd->sequence  = cmd_i;
            mcmd->sequence_zn = zn_i;
            mcmd->naddr     = 1;
            mcmd->status    = 0;
            mcmd->nsec[0]   = (nsec_zn >= ZTL_WCA_SEC_MCMD) ?
                                        ZTL_WCA_SEC_MCMD : nsec_zn;

            mcmd->addr[0].g.grp  = prov->addr[zn_i].g.grp;
            mcmd->addr[0].g.zone = prov->addr[zn_i].g.zone;

            if (!XZTL_WRITE_APPEND)
                mcmd->addr[0].g.sect = (uint64_t) prov->addr[zn_i].g.sect +
                                (uint64_t) (prov->nsec[zn_i] - nsec_zn);

            nsec_zn -= mcmd->nsec[0];
            ucmd->msec[cmd_i] = mcmd->nsec[0];

            mcmd->prp[0] = boff;
            boff += core->media->geo.nbytes * mcmd->nsec[0];

            mcmd->callback  = ztl_wca_callback_mcmd;
            mcmd->opaque    = ucmd;
            mcmd->async_ctx = tctx;

            ucmd->mcmd[cmd_i] = mcmd;

            if (zn_cmd_id[zn_i] == -1)
                zn_cmd_id[zn_i] = cmd_i;

            cmd_i++;
        }
    }

    ZDEBUG(ZDEBUG_WCA, "ztl-wca: Populated: %d", cmd_i);

    /* Submit media commands */
    for (cmd_i = 0; cmd_i < ZTL_PRO_STRIPE * 2; cmd_i++)
        ucmd->minflight[cmd_i] = 0;

    pthread_spin_init(&ucmd->inflight_spin, 0);

    submitted = 0;
    while (submitted < ncmd) {
        for (zn_i = 0; zn_i < prov->naddr; zn_i++) {
            if (zn_cmd_id[zn_i] < 0)
                continue;

            if (zn_cmd_id[zn_i] >= ncmd ||
                ucmd->mcmd[zn_cmd_id[zn_i]]->sequence_zn != zn_i) {
                zn_cmd_id[zn_i] = -1;
                continue;
            }

            /* Limit to 1 write per zone if append is not supported */
            if (!XZTL_WRITE_APPEND) {
                if (ucmd->minflight[zn_i]) {
                    ztl_wca_poke_ctx();
                    continue;
                }

                pthread_spin_lock(&ucmd->inflight_spin);
                ucmd->minflight[zn_i] = 1;
                pthread_spin_unlock(&ucmd->inflight_spin);
            }

            ret = xztl_media_submit_io(ucmd->mcmd[zn_cmd_id[zn_i]]);
            if (ret)
                goto FAIL_SUBMIT;

            ucmd->mcmd[zn_cmd_id[zn_i]]->submitted = 1;
            submitted++;
            zn_cmd_id[zn_i]++;

            if (submitted % ZTL_PRO_STRIPE == 0)
                ztl_wca_poke_ctx();
        }
        usleep(1);
    }

    /* Poke the context for completions */
    while (ucmd->ncb < ucmd->nmcmd) {
        ztl_wca_poke_ctx();
        if (!STAILQ_EMPTY (&ucmd_head))
            break;
    }

    ZDEBUG(ZDEBUG_WCA, "  Submitted: %d", submitted);

    return;

/* If we get a submit failure but previous I/Os have been
 * submitted, we fail all subsequent I/Os and completion is
 * performed by the callback function */
FAIL_SUBMIT:
    if (submitted) {
        ucmd->status = XZTL_ZTL_WCA_S2_ERR;
        for (cmd_i = 0; cmd_i < ncmd; cmd_i++) {
            if (!ucmd->mcmd[cmd_i]->submitted)
                xztl_atomic_int16_update(&ucmd->ncb, ucmd->ncb + 1);
        }

        /* Check for completion in case of completion concurrence */
        if (ucmd->ncb == ucmd->nmcmd) {
            ucmd->completed = 1;
        }
    } else {
        cmd_i = ncmd;
        goto FAIL_MP;
    }

    /* Poke the context for completions */
    while (ucmd->ncb < ucmd->nmcmd) {
        ztl_wca_poke_ctx();
        if (!STAILQ_EMPTY (&ucmd_head))
            break;
    }

    return;

FAIL_MP:
    while (cmd_i) {
        cmd_i--;
        xztl_mempool_put(ucmd->mcmd[cmd_i]->mp_cmd,
                            XZTL_MEMPOOL_MCMD,
                            ZTL_PRO_TUSER);
        ucmd->mcmd[cmd_i]->mp_cmd = NULL;
        ucmd->mcmd[cmd_i] = NULL;
    }

FAIL_NCMD:
    for (zn_i = 0; zn_i < prov->naddr; zn_i++)
        prov->nsec[zn_i] = 0;
    ztl()->pro->free_fn(prov);

FAILURE:
    ucmd->status = XZTL_ZTL_WCA_S_ERR;

    if (ucmd->callback) {
        ucmd->completed = 1;
        pthread_spin_destroy(&ucmd->inflight_spin);
        ucmd->callback(ucmd);
    } else {
        ucmd->completed = 1;
    }
}

static void *ztl_wca_write_th(void *arg) {
    struct xztl_io_ucmd *ucmd;

#if ZTL_WRITE_AFFINITY
    cpu_set_t cpuset;

    /* Set affinity to writing core */
    CPU_ZERO(&cpuset);
    CPU_SET(ZTL_WRITE_CORE, &cpuset);
    pthread_setaffinity_np (pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif

    wca_running = 1;

    while (wca_running) {
NEXT:
    if (!STAILQ_EMPTY(&ucmd_head)) {

        pthread_spin_lock(&ucmd_spin);
        ucmd = STAILQ_FIRST(&ucmd_head);
        STAILQ_REMOVE_HEAD(&ucmd_head, entry);
        pthread_spin_unlock(&ucmd_spin);

        ztl_wca_process_ucmd(ucmd);

        goto NEXT;
    }
    pthread_spin_unlock(&ucmd_spin);

    usleep(1);
    }

    return NULL;
}

static int ztl_wca_init(void) {

    STAILQ_INIT(&ucmd_head);

    /* Initialize thread media context
     * If more write threads are to be used, we need more contexts */


        tctx = xztl_ctx_media_init(0, ZTL_MCMD_ENTS);
        if (!tctx)
            return XZTL_ZTL_WCA_ERR;


    if (pthread_spin_init(&ucmd_spin, 0))
        goto TCTX;

    if (pthread_create(&wca_thread, NULL, ztl_wca_write_th, NULL))
        goto SPIN;

    log_info("ztl-wca: Write-caching started.");

    return 0;

SPIN:
    pthread_spin_destroy(&ucmd_spin);
TCTX:


        xztl_ctx_media_exit(tctx);

    return XZTL_ZTL_WCA_ERR;
}

static void ztl_wca_exit(void) {
    wca_running = 0;
    pthread_join(wca_thread, NULL);
    pthread_spin_destroy(&ucmd_spin);
    xztl_ctx_media_exit(tctx);

    log_info("ztl-wca: Write-caching stopped.");
}

static struct app_wca_mod libztl_wca = {
    .mod_id      = LIBZTL_WCA,
    .name        = "LIBZTL-WCA",
    .init_fn     = ztl_wca_init,
    .exit_fn     = ztl_wca_exit,
    .submit_fn   = ztl_wca_submit,
    .callback_fn = ztl_wca_callback
};

void ztl_wca_register(void) {
    ztl_mod_register(ZTLMOD_WCA, LIBZTL_WCA, &libztl_wca);
}

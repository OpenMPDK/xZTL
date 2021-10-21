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

#include <sys/queue.h>
#include <stdlib.h>
#include <xztl.h>
#include <xztl-mempool.h>
#include <xztl-ztl.h>
#include <ztl.h>
#include <ztl_metadata.h>

extern uint32_t app_ngrps;
struct app_group **glist;
static uint16_t cur_grp[ZTL_PRO_TYPES];

void ztl_pro_free(struct app_pro_addr *ctx) {
    uint32_t zn_i;

    for (zn_i = 0; zn_i < ctx->naddr; zn_i++)
        ztl_pro_grp_free(ctx->grp, ctx->addr[zn_i].g.zone,
                            ctx->nsec[zn_i], ctx->ptype);

    /* We assume a single group for now */
    app_grp_ctx_sub(ctx->grp);

    xztl_mempool_put(ctx->mp_entry, XZTL_ZTL_PRO_CTX, ctx->thread_id);
}

struct app_pro_addr *ztl_pro_new(uint32_t nsec, uint16_t type, uint8_t multi) {
    struct xztl_mp_entry *mpe;
    struct app_pro_addr *ctx;
    struct app_group *grp;
    int ret;

    ZDEBUG(ZDEBUG_PRO, "ztl-pro      (new): nsec %d, type %d", nsec, type);

    if (type >= ZTL_PRO_TYPES) {
        log_erra("ztl-pro: Provisioning type not supported: %d", type);
        return NULL;
    }

    mpe = xztl_mempool_get(XZTL_ZTL_PRO_CTX, type);
    if (!mpe) {
        log_erra("ztl-pro: mempool is empty. Type %x", type);
        return NULL;
    }

    ctx = (struct app_pro_addr *) mpe->opaque;

    /* For now, we consider a single group */
    grp = glist[cur_grp[type]];

    ret = ztl_pro_grp_get(grp, ctx, nsec, type, multi);
    if (ret) {
        xztl_mempool_put(mpe, XZTL_ZTL_PRO_CTX, type);
        log_erra("ztl-pro: Get group zone failed. Type %x", type);
        return NULL;
    }

    ctx->grp       = grp;
    ctx->thread_id = type;
    ctx->ptype     = type;
    ctx->mp_entry  = mpe;

    app_grp_ctx_add(grp);

    /* We do not check for disabled groups (disabled by built-in functions).
     * For now, it is ok because we don't have garbage collection */
    cur_grp[type] = (cur_grp[type] == app_ngrps - 1) ? 0 : cur_grp[type] + 1;

    return ctx;
}

int ztl_pro_put_zone(struct app_group *grp, uint32_t zid) {
    return ztl_pro_grp_put_zone(grp, zid);
}

int ztl_pro_finish_zone(struct app_group *grp, uint32_t zid, uint8_t type) {
    return ztl_pro_grp_finish_zn(grp, zid, type);
}

void ztl_pro_check_gc(struct app_group *grp) {
}

void ztl_pro_exit(void) {
    int ret;

    ret = ztl()->groups.get_list_fn(glist, app_ngrps);
    if (ret != app_ngrps)
        log_infoa("ztl-pro (exit): Groups mismatch (%d,%d).", ret, app_ngrps);

    while (ret) {
        ret--;
        ztl_pro_grp_exit(glist[ret]);
    }

    free(glist);

    log_info("ztl-pro: Global provisioning stopped.");
}

static int ztl_mempool_init(void) {
    uint32_t th_i;
    int ret;

        for (th_i = 0; th_i < ZTL_PRO_TYPES; th_i++) {
            ret = xztl_mempool_create(XZTL_ZTL_PRO_CTX,
                                       th_i,
                                       ZTL_PRO_MP_SZ,
                                       sizeof (struct app_pro_addr),
                                       NULL,
                                       NULL
                                       );
        if (ret)
            return ret;
    }
    return XZTL_OK;
}

static void ztl_mempool_exit(void) {
    int th_i;
    for (th_i = 0; th_i < ZTL_PRO_TYPES; th_i++) {
            xztl_mempool_destroy(XZTL_ZTL_PRO_CTX, th_i);
    }
}

int ztl_pro_init(void) {
    int ret, grp_i = 0;

    glist = calloc(sizeof (struct app_group *), app_ngrps);
    if (!glist)
        return XZTL_ZTL_GROUP_ERR;

    if (ztl_mempool_init())
        goto FREE;

    ret = ztl()->groups.get_list_fn(glist, app_ngrps);
    if (ret != app_ngrps)
        goto MP;
    if (ztl_metadata_init(glist[0]))
        goto EXIT;
    for (grp_i = 0; grp_i < app_ngrps; grp_i++) {
        if (ztl_pro_grp_init(glist[grp_i]))
            goto EXIT;
    }

    memset(cur_grp, 0x0, sizeof (uint16_t) * ZTL_PRO_TYPES);

    log_info("ztl-pro: Global provisioning started.");

    return XZTL_OK;

EXIT:
    while (grp_i) {
        grp_i--;
        ztl_pro_grp_exit(glist[grp_i]);
    }

MP:
    ztl_mempool_exit();
FREE:
    free(glist);
    return XZTL_ZTL_GROUP_ERR;
}

static struct app_pro_mod ztl_pro = {
    .mod_id         = LIBZTL_PRO,
    .name           = "LIBZTL-PRO",
    .init_fn        = ztl_pro_init,
    .exit_fn        = ztl_pro_exit,
    .check_gc_fn    = ztl_pro_check_gc,
    .finish_zn_fn   = ztl_pro_finish_zone,
    .put_zone_fn    = ztl_pro_put_zone,
    .new_fn         = ztl_pro_new,
    .free_fn        = ztl_pro_free
};

void ztl_pro_register(void) {
    ztl_mod_register(ZTLMOD_PRO, LIBZTL_PRO, &ztl_pro);
}

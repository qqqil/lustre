/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2002, 2003 Cluster File Systems, Inc.
 * Author: Phil Schwan <phil@clusterfs.com>
 *         Peter Braam <braam@clusterfs.com>
 *         Mike Shaver <shaver@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_LOV
#ifdef __KERNEL__
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/seq_file.h>
#include <asm/div64.h>
#else
#include <liblustre.h>
#endif

#include <linux/obd_support.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_net.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_dlm.h>
#include <linux/lustre_mds.h>
#include <linux/obd_class.h>
#include <linux/obd_lov.h>
#include <linux/obd_ost.h>
#include <linux/lprocfs_status.h>

#include "lov_internal.h"

/* obd methods */
#define MAX_STRING_SIZE 128
static int lov_connect_obd(struct obd_device *obd, struct lov_tgt_desc *tgt,
                           int activate, struct obd_connect_data *conn_data,
                           unsigned long connect_flags)
{
        struct obd_uuid lov_osc_uuid = { "LOV_OSC_UUID" };
        struct obd_uuid *tgt_uuid = &tgt->uuid;

#ifdef __KERNEL__
        struct proc_dir_entry *lov_proc_dir;
#endif
        struct lov_obd *lov = &obd->u.lov;
        struct lustre_handle conn = {0, };
        struct obd_device *tgt_obd;
        int rc;
        ENTRY;

        tgt_obd = class_find_client_obd(tgt_uuid, OBD_OSC_DEVICENAME,
                                        &obd->obd_uuid);

        if (!tgt_obd) {
                CERROR("Target %s not attached\n", tgt_uuid->uuid);
                RETURN(-EINVAL);
        }

        if (!tgt_obd->obd_set_up) {
                CERROR("Target %s not set up\n", tgt_uuid->uuid);
                RETURN(-EINVAL);
        }

        if (activate) {
                tgt_obd->obd_no_recov = 0;
                ptlrpc_activate_import(tgt_obd->u.cli.cl_import);
        }

        if (tgt_obd->u.cli.cl_import->imp_invalid) {
                CERROR("not connecting OSC %s; administratively "
                       "disabled\n", tgt_uuid->uuid);
                rc = obd_register_observer(tgt_obd, obd);
                if (rc) {
                        CERROR("Target %s register_observer error %d; "
                               "will not be able to reactivate\n",
                               tgt_uuid->uuid, rc);
                }
                RETURN(0);
        }

        rc = obd_connect(&conn, tgt_obd, &lov_osc_uuid, conn_data,
                         connect_flags);
        if (rc) {
                CERROR("Target %s connect error %d\n", tgt_uuid->uuid, rc);
                RETURN(rc);
        }
        tgt->ltd_exp = class_conn2export(&conn);

        rc = obd_register_observer(tgt_obd, obd);
        if (rc) {
                CERROR("Target %s register_observer error %d\n",
                       tgt_uuid->uuid, rc);
                obd_disconnect(tgt->ltd_exp, 0);
                tgt->ltd_exp = NULL;
                RETURN(rc);
        }

        tgt->active = 1;
        lov->desc.ld_active_tgt_count++;

#ifdef __KERNEL__
        lov_proc_dir = lprocfs_srch(obd->obd_proc_entry, "target_obds");
        if (lov_proc_dir) {
                struct obd_device *osc_obd = class_conn2obd(&conn);
                struct proc_dir_entry *osc_symlink;
                char name[MAX_STRING_SIZE + 1];

                LASSERT(osc_obd != NULL);
                LASSERT(osc_obd->obd_type != NULL);
                LASSERT(osc_obd->obd_type->typ_name != NULL);
                name[MAX_STRING_SIZE] = '\0';
                snprintf(name, MAX_STRING_SIZE, "../../../%s/%s",
                         osc_obd->obd_type->typ_name,
                         osc_obd->obd_name);
                osc_symlink = proc_symlink(osc_obd->obd_name, lov_proc_dir,
                                           name);
                if (osc_symlink == NULL) {
                        CERROR("could not register LOV target "
                               "/proc/fs/lustre/%s/%s/target_obds/%s\n",
                               obd->obd_type->typ_name, obd->obd_name,
                               osc_obd->obd_name);
                        lprocfs_remove(lov_proc_dir);
                        lov_proc_dir = NULL;
                }
        }
#endif

        RETURN(0);
}

static int lov_connect(struct lustre_handle *conn, struct obd_device *obd,
                       struct obd_uuid *cluuid, struct obd_connect_data *data,
                       unsigned long flags)
{
#ifdef __KERNEL__
        struct proc_dir_entry *lov_proc_dir;
#endif
        struct lov_obd *lov = &obd->u.lov;
        struct lov_tgt_desc *tgt;
        struct obd_export *exp;
        int rc, rc2, i;
        ENTRY;

        rc = class_connect(conn, obd, cluuid);
        if (rc)
                RETURN(rc);

        exp = class_conn2export(conn);

        /* We don't want to actually do the underlying connections more than
         * once, so keep track. */
        lov->refcount++;
        if (lov->refcount > 1) {
                class_export_put(exp);
                RETURN(0);
        }

#ifdef __KERNEL__
        lov_proc_dir = lprocfs_register("target_obds", obd->obd_proc_entry,
                                        NULL, NULL);
        if (IS_ERR(lov_proc_dir)) {
                CERROR("could not register /proc/fs/lustre/%s/%s/target_obds.",
                       obd->obd_type->typ_name, obd->obd_name);
                lov_proc_dir = NULL;
        }
#endif

        /* connect_flags is the MDS number, save for use in lov_add_obd */
        lov->lov_connect_flags = flags;
        for (i = 0, tgt = lov->tgts; i < lov->desc.ld_tgt_count; i++, tgt++) {
                if (obd_uuid_empty(&tgt->uuid))
                        continue;
                rc = lov_connect_obd(obd, tgt, 0, data, flags);
                if (rc)
                        GOTO(out_disc, rc);
        }

        class_export_put(exp);
        RETURN (0);

 out_disc:
#ifdef __KERNEL__
        if (lov_proc_dir)
                lprocfs_remove(lov_proc_dir);
#endif

        while (i-- > 0) {
                struct obd_uuid uuid;
                --tgt;
                --lov->desc.ld_active_tgt_count;
                tgt->active = 0;
                /* save for CERROR below; (we know it's terminated) */
                uuid = tgt->uuid;
                rc2 = obd_disconnect(tgt->ltd_exp, 0);
                if (rc2)
                        CERROR("error: LOV target %s disconnect on OST idx %d: "
                               "rc = %d\n", uuid.uuid, i, rc2);
        }
        class_disconnect(exp, 0);
        RETURN (rc);
}

static int lov_disconnect_obd(struct obd_device *obd, 
			      struct lov_tgt_desc *tgt,
                              unsigned long flags)
{
#ifdef __KERNEL__
        struct proc_dir_entry *lov_proc_dir;
#endif
        struct obd_device *osc_obd = class_exp2obd(tgt->ltd_exp);
        struct lov_obd *lov = &obd->u.lov;
        int rc;
        ENTRY;

#ifdef __KERNEL__
        lov_proc_dir = lprocfs_srch(obd->obd_proc_entry, "target_obds");
        if (lov_proc_dir) {
                struct proc_dir_entry *osc_symlink;

                osc_symlink = lprocfs_srch(lov_proc_dir, osc_obd->obd_name);
                if (osc_symlink) {
                        lprocfs_remove(osc_symlink);
                } else {
                        CERROR("/proc/fs/lustre/%s/%s/target_obds/%s missing\n",
                               obd->obd_type->typ_name, obd->obd_name,
                               osc_obd->obd_name);
                }
        }
#endif
        if (obd->obd_no_recov) {
                /* Pass it on to our clients.
                 * XXX This should be an argument to disconnect,
                 * XXX not a back-door flag on the OBD.  Ah well.
                 */
                if (osc_obd)
                        osc_obd->obd_no_recov = 1;
        }

        obd_register_observer(tgt->ltd_exp->exp_obd, NULL);
        rc = obd_disconnect(tgt->ltd_exp, flags);
        if (rc) {
                if (tgt->active) {
                        CERROR("Target %s disconnect error %d\n",
                               tgt->uuid.uuid, rc);
                }
                rc = 0;
        }

        if (tgt->active) {
                tgt->active = 0;
                lov->desc.ld_active_tgt_count--;
        }
        tgt->ltd_exp = NULL;
        RETURN(0);
}

static int lov_disconnect(struct obd_export *exp, unsigned long flags)
{
        struct obd_device *obd = class_exp2obd(exp);
#ifdef __KERNEL__
        struct proc_dir_entry *lov_proc_dir;
#endif
        struct lov_obd *lov = &obd->u.lov;
        struct lov_tgt_desc *tgt;
        int rc, i;
        ENTRY;

        if (!lov->tgts)
                goto out_local;

        /* Only disconnect the underlying layers on the final disconnect. */
        lov->refcount--;
        if (lov->refcount != 0)
                goto out_local;

        for (i = 0, tgt = lov->tgts; i < lov->desc.ld_tgt_count; i++, tgt++) {
                if (tgt->ltd_exp)
                        lov_disconnect_obd(obd, tgt, flags);
        }

#ifdef __KERNEL__
        lov_proc_dir = lprocfs_srch(obd->obd_proc_entry, "target_obds");
        if (lov_proc_dir) {
                lprocfs_remove(lov_proc_dir);
        } else {
                CERROR("/proc/fs/lustre/%s/%s/target_obds missing.",
                       obd->obd_type->typ_name, obd->obd_name);
        }
#endif
        
 out_local:
        rc = class_disconnect(exp, 0);
        RETURN(rc);
}

/* Error codes:
 *
 *  -EINVAL  : UUID can't be found in the LOV's target list
 *  -ENOTCONN: The UUID is found, but the target connection is bad (!)
 *  -EBADF   : The UUID is found, but the OBD is the wrong type (!)
 */
static int lov_set_osc_active(struct lov_obd *lov, struct obd_uuid *uuid,
                              int activate)
{
        struct lov_tgt_desc *tgt;
        int i, rc = 0;
        ENTRY;

        CDEBUG(D_INFO, "Searching in lov %p for uuid %s (activate=%d)\n",
               lov, uuid->uuid, activate);

        spin_lock(&lov->lov_lock);
        for (i = 0, tgt = lov->tgts; i < lov->desc.ld_tgt_count; i++, tgt++) {
                if (tgt->ltd_exp == NULL)
                        continue;

                CDEBUG(D_INFO, "lov idx %d is %s conn "LPX64"\n",
                       i, tgt->uuid.uuid, tgt->ltd_exp->exp_handle.h_cookie);
                
                if (obd_uuid_equals(uuid, &tgt->uuid))
                        break;
        }

        if (i == lov->desc.ld_tgt_count)
                GOTO(out, rc = -EINVAL);


        if (tgt->active == activate) {
                CDEBUG(D_INFO, "OSC %s already %sactive!\n", uuid->uuid,                       
                        activate ? "" : "in");
                GOTO(out, rc);
        }

        CDEBUG(D_INFO, "Marking OSC %s %sactive\n", uuid->uuid,
               activate ? "" : "in");
        CDEBUG(D_ERROR, "Marking OSC %s %sactive\n", uuid->uuid,
               activate ? "" : "in");

        tgt->active = activate;
        if (activate)
                lov->desc.ld_active_tgt_count++;
        else
                lov->desc.ld_active_tgt_count--;

        EXIT;
 out:
        spin_unlock(&lov->lov_lock);
        return rc;
}

static int lov_notify(struct obd_device *obd, struct obd_device *watched,
                      int active, void *data)
{
        struct obd_uuid *uuid;
        int rc;
        ENTRY;

        if (strcmp(watched->obd_type->typ_name, OBD_OSC_DEVICENAME)) {
                CERROR("unexpected notification of %s %s!\n",
                       watched->obd_type->typ_name,
                       watched->obd_name);
                return -EINVAL;
        }
        uuid = &watched->u.cli.cl_import->imp_target_uuid;

        /* Set OSC as active before notifying the observer, so the
         * observer can use the OSC normally.  
         */
        rc = lov_set_osc_active(&obd->u.lov, uuid, active);
        if (rc) {
                CERROR("%sactivation of %s failed: %d\n",
                       active ? "" : "de", uuid->uuid, rc);
                RETURN(rc);
        }

        if (obd->obd_observer)
                /* Pass the notification up the chain. */
                rc = obd_notify(obd->obd_observer, watched, active, data);

        RETURN(rc);
}

int lov_attach(struct obd_device *dev, obd_count len, void *data)
{
        struct lprocfs_static_vars lvars;
        int rc;

        lprocfs_init_vars(lov, &lvars);
        rc = lprocfs_obd_attach(dev, lvars.obd_vars);
        if (rc == 0) {
#ifdef __KERNEL__
                struct proc_dir_entry *entry;

                entry = create_proc_entry("target_obd_status", 0444, 
                                          dev->obd_proc_entry);
                if (entry == NULL) {
                        rc = -ENOMEM;
                } else {
                        entry->proc_fops = &lov_proc_target_fops;
                        entry->data = dev;
                }
#endif
        }
        return rc;
}

int lov_detach(struct obd_device *dev)
{
        return lprocfs_obd_detach(dev);
}

static int lov_setup(struct obd_device *obd, obd_count len, void *buf)
{
        struct lov_obd *lov = &obd->u.lov;
        struct lustre_cfg *lcfg = buf;
        struct lov_desc *desc;
        int count;
        ENTRY;

        if (LUSTRE_CFG_BUFLEN(lcfg, 1) < 1) {
                CERROR("LOV setup requires a descriptor\n");
                RETURN(-EINVAL);
        }

        desc = (struct lov_desc *)lustre_cfg_string(lcfg, 1);
        if (sizeof(*desc) > LUSTRE_CFG_BUFLEN(lcfg, 1)) {
                CERROR("descriptor size wrong: %d > %d\n",
                       (int)sizeof(*desc), LUSTRE_CFG_BUFLEN(lcfg, 1));
                RETURN(-EINVAL);
        }
 
        /* Because of 64-bit divide/mod operations only work with a 32-bit
         * divisor in a 32-bit kernel, we cannot support a stripe width
         * of 4GB or larger on 32-bit CPUs.
         */
       
        count = desc->ld_default_stripe_count;
        if (count && (count * desc->ld_default_stripe_size) > ~0UL) {
                CERROR("LOV: stripe width "LPU64"x%u > %lu on 32-bit system\n",
                       desc->ld_default_stripe_size, count, ~0UL);
                RETURN(-EINVAL);
        }
        if (desc->ld_tgt_count > 0) {
                lov->bufsize= sizeof(struct lov_tgt_desc) * desc->ld_tgt_count;
        } else {
                lov->bufsize = sizeof(struct lov_tgt_desc) * LOV_MAX_TGT_COUNT;  
        }
        OBD_ALLOC(lov->tgts, lov->bufsize);
        if (lov->tgts == NULL) {
                lov->bufsize = 0;
                CERROR("couldn't allocate %d bytes for target table.\n",
                       lov->bufsize);
                RETURN(-EINVAL);
        }

        desc->ld_tgt_count = 0;
        desc->ld_active_tgt_count = 0;
        lov->desc = *desc;
        spin_lock_init(&lov->lov_lock);
        sema_init(&lov->lov_llog_sem, 1);

        RETURN(0);
}

static int lov_cleanup(struct obd_device *obd, int flags)
{
        struct lov_obd *lov = &obd->u.lov;

        OBD_FREE(lov->tgts, lov->bufsize);
        RETURN(0);
}

static int
lov_add_obd(struct obd_device *obd, struct obd_uuid *uuidp, int index, int gen)
{
        struct lov_obd *lov = &obd->u.lov;
        struct lov_tgt_desc *tgt;
        int rc;
        ENTRY;

        CDEBUG(D_CONFIG, "uuid: %s idx: %d gen: %d\n",
               uuidp->uuid, index, gen);

        if ((index < 0) || (index >= LOV_MAX_TGT_COUNT)) {
                CERROR("request to add OBD %s at invalid index: %d\n",
                       uuidp->uuid, index);
                RETURN(-EINVAL);
        }

        if (gen <= 0) {
                CERROR("request to add OBD %s with invalid generation: %d\n",
                       uuidp->uuid, gen);
                RETURN(-EINVAL);
        }

        tgt = lov->tgts + index;
        if (!obd_uuid_empty(&tgt->uuid)) {
                CERROR("OBD already assigned at LOV target index %d\n",
                       index);
                RETURN(-EEXIST);
        }

        tgt->uuid = *uuidp;
        /* XXX - add a sanity check on the generation number. */
        tgt->ltd_gen = gen;

        if (index >= lov->desc.ld_tgt_count)
                lov->desc.ld_tgt_count = index + 1;

        CDEBUG(D_CONFIG, "idx: %d ltd_gen: %d ld_tgt_count: %d\n",
                index, tgt->ltd_gen, lov->desc.ld_tgt_count);

        if (lov->refcount == 0)
                RETURN(0);

        if (tgt->ltd_exp) {
                struct obd_device *osc_obd;

                osc_obd = class_exp2obd(tgt->ltd_exp);
                if (osc_obd)
                        osc_obd->obd_no_recov = 0;
        }

        rc = lov_connect_obd(obd, tgt, 1, NULL, lov->lov_connect_flags);
        if (rc)
                GOTO(out, rc);

        if (obd->obd_observer) {
                /* tell the mds_lov about the new target */
                rc = obd_notify(obd->obd_observer, tgt->ltd_exp->exp_obd, 1,
                                (void *)index);
        }

        GOTO(out, rc);
 out:
        if (rc && tgt->ltd_exp != NULL)
                lov_disconnect_obd(obd, tgt, 0);
        return rc;
}

static int
lov_del_obd(struct obd_device *obd, struct obd_uuid *uuidp, int index, int gen)
{
        struct lov_obd *lov = &obd->u.lov;
        struct lov_tgt_desc *tgt;
        int count = lov->desc.ld_tgt_count;
        int rc = 0;
        ENTRY;

        CDEBUG(D_CONFIG, "uuid: %s idx: %d gen: %d\n",
               uuidp->uuid, index, gen);

        if (index >= count) {
                CERROR("LOV target index %d >= number of LOV OBDs %d.\n",
                       index, count);
                RETURN(-EINVAL);
        }

        tgt = lov->tgts + index;

        if (obd_uuid_empty(&tgt->uuid)) {
                CERROR("LOV target at index %d is not setup.\n", index);
                RETURN(-EINVAL);
        }

        if (!obd_uuid_equals(uuidp, &tgt->uuid)) {
                CERROR("LOV target UUID %s at index %d doesn't match %s.\n",
                       tgt->uuid.uuid, index, uuidp->uuid);
                RETURN(-EINVAL);
        }

        if (tgt->ltd_exp) {
                struct obd_device *osc_obd;

                osc_obd = class_exp2obd(tgt->ltd_exp);
                if (osc_obd) {
                        osc_obd->obd_no_recov = 1;
                        rc = obd_llog_finish(osc_obd, &osc_obd->obd_llogs, 1);
                        if (rc)
                                CERROR("osc_llog_finish error: %d\n", rc);
                }
                lov_disconnect_obd(obd, tgt, 0);
        }

        /* XXX - right now there is a dependency on ld_tgt_count being the
         * maximum tgt index for computing the mds_max_easize. So we can't
         * shrink it. */

        /* lt_gen = 0 will mean it will not match the gen of any valid loi */
        memset(tgt, 0, sizeof(*tgt));

        CDEBUG(D_CONFIG, "uuid: %s idx: %d gen: %d exp: %p active: %d\n",
               tgt->uuid.uuid, index, tgt->ltd_gen, tgt->ltd_exp, tgt->active);

        RETURN(rc);
}

static int lov_process_config(struct obd_device *obd, obd_count len, void *buf)
{
        struct lustre_cfg *lcfg = buf;
        struct obd_uuid obd_uuid;
        int cmd;
        int index;
        int gen;
        int rc = 0;
        ENTRY;

        switch(cmd = lcfg->lcfg_command) {
        case LCFG_LOV_ADD_OBD:
        case LCFG_LOV_DEL_OBD: {
                if (LUSTRE_CFG_BUFLEN(lcfg, 1) > sizeof(obd_uuid.uuid))
                        GOTO(out, rc = -EINVAL);

                obd_str2uuid(&obd_uuid, lustre_cfg_string(lcfg, 1));

                if (sscanf(lustre_cfg_buf(lcfg, 2), "%d", &index) != 1)
                        GOTO(out, rc = -EINVAL);
                if (sscanf(lustre_cfg_buf(lcfg, 3), "%d", &gen) != 1)
                        GOTO(out, rc = -EINVAL);
                if (cmd == LCFG_LOV_ADD_OBD)
                        rc = lov_add_obd(obd, &obd_uuid, index, gen);
                else
                        rc = lov_del_obd(obd, &obd_uuid, index, gen);
                GOTO(out, rc);
        }
        default: {
                CERROR("Unknown command: %d\n", lcfg->lcfg_command);
                GOTO(out, rc = -EINVAL);

        }
        }
out:
        RETURN(rc);
}

#ifndef log2
#define log2(n) ffz(~(n))
#endif

static int lov_clear_orphans(struct obd_export *export,
                             struct obdo *src_oa,
                             struct lov_stripe_md **ea,
                             struct obd_trans_info *oti)
{
        struct lov_obd *lov;
        struct obdo *tmp_oa;
        struct obd_uuid *ost_uuid = NULL;
        int rc = 0, i;
        ENTRY;

        LASSERT(src_oa->o_valid & OBD_MD_FLFLAGS &&
                src_oa->o_flags == OBD_FL_DELORPHAN);

        lov = &export->exp_obd->u.lov;

        tmp_oa = obdo_alloc();
        if (tmp_oa == NULL)
                RETURN(-ENOMEM);

        if (src_oa->o_valid & OBD_MD_FLINLINE) {
                ost_uuid = (struct obd_uuid *)src_oa->o_inline;
                CDEBUG(D_HA, "clearing orphans only for %s\n",
                       ost_uuid->uuid);
        }

        for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                int err;
                struct lov_stripe_md obj_md;
                struct lov_stripe_md *obj_mdp = &obj_md;

                /*
                 * if called for a specific target, we don't care if it is not
                 * active.
                 */
                if (lov->tgts[i].active == 0 && ost_uuid == NULL) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", i);
                        continue;
                }

                if (ost_uuid && !obd_uuid_equals(ost_uuid, &lov->tgts[i].uuid))
                        continue;

                /* 
                 * setting up objid OSS objects should be destroyed starting
                 * from it.
                 */
                memcpy(tmp_oa, src_oa, sizeof(*tmp_oa));
                tmp_oa->o_valid |= OBD_MD_FLID;
                tmp_oa->o_id = oti->oti_objid[i];

                /* XXX: LOV STACKING: use real "obj_mdp" sub-data */
                err = obd_create(lov->tgts[i].ltd_exp, tmp_oa, NULL, 0,
                                 &obj_mdp, oti);
                if (err) {
                        /*
                         * this export will be disabled until it is recovered,
                         * and then orphan recovery will be completed.
                         */
                        CERROR("error in orphan recovery on OST idx %d/%d: "
                               "rc = %d\n", i, lov->desc.ld_tgt_count, err);
                }

                if (ost_uuid)
                        break;
        }
        obdo_free(tmp_oa);
        RETURN(rc);
}

/* the LOV expects oa->o_id to be set to the LOV object id */
static int
lov_create(struct obd_export *exp, struct obdo *src_oa,
           void *acl, int acl_size, struct lov_stripe_md **ea,
           struct obd_trans_info *oti)
{
        struct lov_request_set *set = NULL;
        struct list_head *pos;
        struct lov_obd *lov;
        int rc = 0;
        ENTRY;

        LASSERT(ea != NULL);
        if (exp == NULL)
                RETURN(-EINVAL);

        if ((src_oa->o_valid & OBD_MD_FLFLAGS) &&
            src_oa->o_flags == OBD_FL_DELORPHAN) {
                rc = lov_clear_orphans(exp, src_oa, ea, oti);
                RETURN(rc);
        }

        lov = &exp->exp_obd->u.lov;
        if (!lov->desc.ld_active_tgt_count)
                RETURN(-EIO);

        LASSERT(oti->oti_flags & OBD_MODE_CROW);
                
        /* main creation loop */
        rc = lov_prep_create_set(exp, ea, src_oa, oti, &set);
        if (rc)
                RETURN(rc);

        list_for_each (pos, &set->set_list) {
                struct lov_request *req = 
                        list_entry(pos, struct lov_request, rq_link);

                /* XXX: LOV STACKING: use real "obj_mdp" sub-data */
                rc = obd_create(lov->tgts[req->rq_idx].ltd_exp,
                                req->rq_oa, NULL, 0, &req->rq_md, oti);
                lov_update_create_set(set, req, rc);
        }
        rc = lov_fini_create_set(set, ea);
        RETURN(rc);
}

#define lsm_bad_magic(LSMP)                                     \
({                                                              \
        struct lov_stripe_md *_lsm__ = (LSMP);                  \
        int _ret__ = 0;                                         \
        if (!_lsm__) {                                          \
                CERROR("LOV requires striping ea\n");           \
                _ret__ = 1;                                     \
        } else if (_lsm__->lsm_magic != LOV_MAGIC) {            \
                CERROR("LOV striping magic bad %#x != %#x\n",   \
                       _lsm__->lsm_magic, LOV_MAGIC);           \
                _ret__ = 1;                                     \
        }                                                       \
        _ret__;                                                 \
})

static int lov_destroy(struct obd_export *exp, struct obdo *oa,
                       struct lov_stripe_md *lsm, struct obd_trans_info *oti)
{
        struct lov_request_set *set;
        struct lov_request *req;
        struct list_head *pos;
        struct lov_obd *lov;
        int rc = 0;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_destroy_set(exp, oa, lsm, oti, &set);
        if (rc)
                RETURN(rc);

        list_for_each (pos, &set->set_list) {
                int err;
                req = list_entry(pos, struct lov_request, rq_link);

                /* XXX update the cookie position */
                oti->oti_logcookies = set->set_cookies + req->rq_stripe;
                rc = obd_destroy(lov->tgts[req->rq_idx].ltd_exp, req->rq_oa,
                                 NULL, oti);
                err = lov_update_common_set(set, req, rc);
                if (rc) {
                        CERROR("error: destroying objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n", 
                               set->set_oa->o_id, req->rq_oa->o_id, 
                               req->rq_idx, rc);
                        if (!rc)
                                rc = err;
                }
        }
        lov_fini_destroy_set(set);
        RETURN(rc);
}

static int lov_getattr(struct obd_export *exp, struct obdo *oa,
                       struct lov_stripe_md *lsm)
{
        struct lov_request_set *set;
        struct lov_request *req;
        struct list_head *pos;
        struct lov_obd *lov;
        int err = 0, rc = 0;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        
        rc = lov_prep_getattr_set(exp, oa, lsm, &set);
        if (rc)
                RETURN(rc);

        list_for_each (pos, &set->set_list) {
                req = list_entry(pos, struct lov_request, rq_link);
                
                CDEBUG(D_INFO, "objid "LPX64"[%d] has subobj "LPX64" at idx "
                       "%u\n", oa->o_id, req->rq_stripe, req->rq_oa->o_id, 
                       req->rq_idx);

                rc = obd_getattr(lov->tgts[req->rq_idx].ltd_exp, 
                                 req->rq_oa, NULL);
                err = lov_update_common_set(set, req, rc);
                if (err) {
                        CERROR("error: getattr objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n",
                               set->set_oa->o_id, req->rq_oa->o_id, 
                               req->rq_idx, err);
                        break;
                }
        }
        
        rc = lov_fini_getattr_set(set);
        if (err)
                rc = err;
        RETURN(rc);
}

static int lov_getattr_interpret(struct ptlrpc_request_set *rqset, void *data,
                                 int rc)
{
        struct lov_request_set *lovset = (struct lov_request_set *)data;
        ENTRY;

        /* don't do attribute merge if this aysnc op failed */
        if (rc) {
                lovset->set_completes = 0;
                lov_fini_getattr_set(lovset);
        } else {
                rc = lov_fini_getattr_set(lovset);
        }
        RETURN (rc);
}

static int lov_getattr_async(struct obd_export *exp, struct obdo *oa,
                              struct lov_stripe_md *lsm,
                              struct ptlrpc_request_set *rqset)
{
        struct lov_request_set *lovset;
        struct lov_obd *lov;
        struct list_head *pos;
        struct lov_request *req;
        int rc = 0;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;

        rc = lov_prep_getattr_set(exp, oa, lsm, &lovset);
        if (rc)
                RETURN(rc);

        CDEBUG(D_INFO, "objid "LPX64": %ux%u byte stripes\n",
               lsm->lsm_object_id, lsm->lsm_stripe_count, lsm->lsm_stripe_size);

        list_for_each (pos, &lovset->set_list) {
                req = list_entry(pos, struct lov_request, rq_link);
                
                CDEBUG(D_INFO, "objid "LPX64"[%d] has subobj "LPX64" at idx "
                       "%u\n", oa->o_id, req->rq_stripe, req->rq_oa->o_id, 
                       req->rq_idx);
                rc = obd_getattr_async(lov->tgts[req->rq_idx].ltd_exp,
                                       req->rq_oa, NULL, rqset);
                if (rc) {
                        CERROR("error: getattr objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n",
                               lovset->set_oa->o_id, req->rq_oa->o_id, 
                               req->rq_idx, rc);
                        GOTO(out, rc);
                }
                lov_update_common_set(lovset, req, rc);
        }
        
        LASSERT(rc == 0);
        LASSERT (rqset->set_interpret == NULL);
        rqset->set_interpret = lov_getattr_interpret;
        rqset->set_arg = (void *)lovset;
        RETURN(rc);
out:
        LASSERT(rc);
        lov_fini_getattr_set(lovset);
        RETURN(rc);
}

static int lov_setattr(struct obd_export *exp, struct obdo *src_oa,
                       struct lov_stripe_md *lsm, struct obd_trans_info *oti)
{
        struct lov_request_set *set;
        struct lov_obd *lov;
        struct list_head *pos;
        struct lov_request *req;
        int err = 0, rc = 0;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        LASSERT(!(src_oa->o_valid & ~(OBD_MD_FLID|OBD_MD_FLTYPE | OBD_MD_FLMODE|
                                      OBD_MD_FLATIME | OBD_MD_FLMTIME |
                                      OBD_MD_FLCTIME | OBD_MD_FLFLAGS |
                                      OBD_MD_FLSIZE | OBD_MD_FLGROUP |
                                      OBD_MD_FLUID | OBD_MD_FLGID |
                                      OBD_MD_FLINLINE | OBD_MD_FLIFID)));

        LASSERT(!(src_oa->o_valid & OBD_MD_FLGROUP) || src_oa->o_gr > 0);

        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_setattr_set(exp, src_oa, lsm, NULL, &set);
        if (rc)
                RETURN(rc);

        list_for_each (pos, &set->set_list) {
                req = list_entry(pos, struct lov_request, rq_link);
                
                rc = obd_setattr(lov->tgts[req->rq_idx].ltd_exp, req->rq_oa,
                                 NULL, NULL);
                err = lov_update_common_set(set, req, rc);
                if (err) {
                        CERROR("error: setattr objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n",
                               set->set_oa->o_id, req->rq_oa->o_id,
                               req->rq_idx, err);
                        if (!rc)
                                rc = err;
                }
        }
        err = lov_fini_setattr_set(set);
        if (!rc)
                rc = err;
        RETURN(rc);
}

static int lov_revalidate_policy(struct lov_obd *lov, struct lov_stripe_md *lsm)
{
        static int next_idx = 0;
        struct lov_tgt_desc *tgt;
        int i, count;

        /* XXX - we should do something clever and take lsm
         * into account but just do round robin for now. */

        /* last_idx must always be less that count because
         * ld_tgt_count currently cannot shrink. */
        count = lov->desc.ld_tgt_count;

        for (i = next_idx, tgt = lov->tgts + i; i < count; i++, tgt++) {
                if (tgt->active) {
                        next_idx = (i + 1) % count;
                        RETURN(i);
                }
        }

        for (i = 0, tgt = lov->tgts; i < next_idx; i++, tgt++) {
                if (tgt->active) {
                        next_idx = (i + 1) % count;
                        RETURN(i);
                }
        }

        RETURN(-EIO);
}

static int lov_revalidate_md(struct obd_export *exp, struct obdo *src_oa,
                             struct lov_stripe_md *ea,
                             struct obd_trans_info *oti)
{
        struct obd_export *osc_exp;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        struct lov_stripe_md *lsm = ea;
        struct lov_stripe_md obj_md;
        struct lov_stripe_md *obj_mdp = &obj_md;
        struct lov_oinfo *loi;
        struct obdo *tmp_oa;
        int ost_idx, updates = 0, i;
        ENTRY;

        tmp_oa = obdo_alloc();
        if (tmp_oa == NULL)
                RETURN(-ENOMEM);

        loi = lsm->lsm_oinfo;
        for (i = 0; i < lsm->lsm_stripe_count; i++, loi++) {
                int rc;
                if (!obd_uuid_empty(&lov->tgts[loi->loi_ost_idx].uuid))
                        continue;

                ost_idx = lov_revalidate_policy(lov, lsm);
                if (ost_idx < 0) {
                        /* FIXME: punt for now. */
                        CERROR("lov_revalidate_policy failed; no active "
                               "OSCs?\n");
                        continue;
                }

                /* create a new object */
                memcpy(tmp_oa, src_oa, sizeof(*tmp_oa));
                /* XXX: LOV STACKING: use real "obj_mdp" sub-data */
                osc_exp = lov->tgts[ost_idx].ltd_exp;
                rc = obd_create(osc_exp, tmp_oa, NULL, 0, &obj_mdp, oti);
                if (rc) {
                        CERROR("error creating new subobj at idx %d; "
                               "rc = %d\n", ost_idx, rc);
                        continue;
                }
                if (oti->oti_objid)
                        oti->oti_objid[ost_idx] = tmp_oa->o_id;
                loi->loi_id = tmp_oa->o_id;
                loi->loi_gr = tmp_oa->o_gr;
                loi->loi_ost_idx = ost_idx;
                loi->loi_ost_gen = lov->tgts[ost_idx].ltd_gen;
                CDEBUG(D_INODE, "replacing objid "LPX64" subobj "LPX64
                       " with idx %d gen %d.\n", lsm->lsm_object_id,
                       loi->loi_id, ost_idx, loi->loi_ost_gen);
                updates = 1;
        }

        /* If we got an error revalidating an entry there's no need to
         * cleanup up objects we allocated here because the bad entry
         * still points to a deleted OST. */

        obdo_free(tmp_oa);
        RETURN(updates);
}

/* FIXME: maybe we'll just make one node the authoritative attribute node, then
 * we can send this 'punch' to just the authoritative node and the nodes
 * that the punch will affect. */
static int lov_punch(struct obd_export *exp, struct obdo *oa,
                     struct lov_stripe_md *lsm,
                     obd_off start, obd_off end, struct obd_trans_info *oti)
{
        struct lov_request_set *set;
        struct lov_obd *lov;
        struct list_head *pos;
        struct lov_request *req;
        int err = 0, rc = 0;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_punch_set(exp, oa, lsm, start, end, oti, &set);
        if (rc)
                RETURN(rc);

        list_for_each (pos, &set->set_list) {
                req = list_entry(pos, struct lov_request, rq_link);

                rc = obd_punch(lov->tgts[req->rq_idx].ltd_exp, req->rq_oa, 
                               NULL, req->rq_extent.start, 
                               req->rq_extent.end, NULL);
                err = lov_update_punch_set(set, req, rc);
                if (err) {
                        CERROR("error: punch objid "LPX64" subobj "LPX64
                               " on OST idx %d: rc = %d\n", set->set_oa->o_id,
                               req->rq_oa->o_id, req->rq_idx, rc);
                        if (!rc)
                                rc = err;
                }
        }
        err = lov_fini_punch_set(set);
        if (!rc)
                rc = err;
        RETURN(rc);
}

static int lov_sync(struct obd_export *exp, struct obdo *oa,
                    struct lov_stripe_md *lsm, obd_off start, obd_off end)
{
        struct lov_request_set *set;
        struct lov_obd *lov;
        struct list_head *pos;
        struct lov_request *req;
        int err = 0, rc = 0;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (!exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_sync_set(exp, oa, lsm, start, end, &set);
        if (rc)
                RETURN(rc);

        list_for_each (pos, &set->set_list) {
                req = list_entry(pos, struct lov_request, rq_link);

                rc = obd_sync(lov->tgts[req->rq_idx].ltd_exp, req->rq_oa, 
                              NULL, req->rq_extent.start, req->rq_extent.end);
                err = lov_update_common_set(set, req, rc);
                if (err) {
                        CERROR("error: fsync objid "LPX64" subobj "LPX64
                               " on OST idx %d: rc = %d\n", set->set_oa->o_id,
                               req->rq_oa->o_id, req->rq_idx, rc);
                        if (!rc)
                                rc = err;
                }
        }
        err = lov_fini_sync_set(set);
        if (!rc)
                rc = err;
        RETURN(rc);
}

static int lov_brw_check(struct lov_obd *lov, struct obdo *oa,
                         struct lov_stripe_md *lsm,
                         obd_count oa_bufs, struct brw_page *pga)
{
        int i, rc = 0;
        ENTRY;

        /* The caller just wants to know if there's a chance that this
         * I/O can succeed */
        for (i = 0; i < oa_bufs; i++) {
                int stripe = lov_stripe_number(lsm, pga[i].disk_offset);
                int ost = lsm->lsm_oinfo[stripe].loi_ost_idx;
                obd_off start, end;

                if (!lov_stripe_intersects(lsm, i, pga[i].disk_offset,
                                           pga[i].disk_offset + pga[i].count,
                                           &start, &end))
                        continue;

                if (lov->tgts[ost].active == 0) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", ost);
                        RETURN(-EIO);
                }
                rc = obd_brw(OBD_BRW_CHECK, lov->tgts[ost].ltd_exp, oa,
                             NULL, 1, &pga[i], NULL);
                if (rc)
                        break;
        }
        RETURN(rc);
}

static int lov_brw(int cmd, struct obd_export *exp, struct obdo *src_oa,
                   struct lov_stripe_md *lsm, obd_count oa_bufs,
                   struct brw_page *pga, struct obd_trans_info *oti)
{
        struct lov_request_set *set;
        struct lov_request *req;
        struct list_head *pos;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        int err, rc = 0;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (cmd == OBD_BRW_CHECK) {
                rc = lov_brw_check(lov, src_oa, lsm, oa_bufs, pga);
                RETURN(rc);
        }

        rc = lov_prep_brw_set(exp, src_oa, lsm, oa_bufs, pga, oti, &set);
        if (rc)
                RETURN(rc);

        list_for_each (pos, &set->set_list) {
                struct obd_export *sub_exp;
                struct brw_page *sub_pga;
                req = list_entry(pos, struct lov_request, rq_link);
                
                sub_exp = lov->tgts[req->rq_idx].ltd_exp;
                sub_pga = set->set_pga + req->rq_pgaidx;
                rc = obd_brw(cmd, sub_exp, req->rq_oa, req->rq_md, 
                             req->rq_oabufs, sub_pga, oti);
                if (rc)
                        break;
                lov_update_common_set(set, req, rc);
        }

        err = lov_fini_brw_set(set);
        if (!rc)
                rc = err;
        RETURN(rc);
}

static int lov_brw_interpret(struct ptlrpc_request_set *reqset, void *data,
                             int rc)
{
        struct lov_request_set *lovset = (struct lov_request_set *)data;
        ENTRY;
        
        if (rc) {
                lovset->set_completes = 0;
                lov_fini_brw_set(lovset);
        } else {
                rc = lov_fini_brw_set(lovset);
        }
                
        RETURN(rc);
}

static int lov_brw_async(int cmd, struct obd_export *exp, struct obdo *oa,
                         struct lov_stripe_md *lsm, obd_count oa_bufs,
                         struct brw_page *pga, struct ptlrpc_request_set *set,
                         struct obd_trans_info *oti)
{
        struct lov_request_set *lovset;
        struct lov_request *req;
        struct list_head *pos;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        int rc = 0;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (cmd == OBD_BRW_CHECK) {
                rc = lov_brw_check(lov, oa, lsm, oa_bufs, pga);
                RETURN(rc);
        }

        rc = lov_prep_brw_set(exp, oa, lsm, oa_bufs, pga, oti, &lovset);
        if (rc)
                RETURN(rc);

        list_for_each (pos, &lovset->set_list) {
                struct obd_export *sub_exp;
                struct brw_page *sub_pga;
                req = list_entry(pos, struct lov_request, rq_link);
                
                sub_exp = lov->tgts[req->rq_idx].ltd_exp;
                sub_pga = lovset->set_pga + req->rq_pgaidx;
                rc = obd_brw_async(cmd, sub_exp, req->rq_oa, req->rq_md,
                                   req->rq_oabufs, sub_pga, set, oti);
                if (rc)
                        GOTO(out, rc);
                lov_update_common_set(lovset, req, rc);
        }
        LASSERT(rc == 0);
        LASSERT(set->set_interpret == NULL);
        set->set_interpret = (set_interpreter_func)lov_brw_interpret;
        set->set_arg = (void *)lovset;
        
        RETURN(rc);
out:
        lov_fini_brw_set(lovset);
        RETURN(rc);
}

static int lov_ap_make_ready(void *data, int cmd)
{
        struct lov_async_page *lap = LAP_FROM_COOKIE(data);

        return lap->lap_caller_ops->ap_make_ready(lap->lap_caller_data, cmd);
}
static int lov_ap_refresh_count(void *data, int cmd)
{
        struct lov_async_page *lap = LAP_FROM_COOKIE(data);

        return lap->lap_caller_ops->ap_refresh_count(lap->lap_caller_data,
                                                     cmd);
}
static void lov_ap_fill_obdo(void *data, int cmd, struct obdo *oa)
{
        struct lov_async_page *lap = LAP_FROM_COOKIE(data);

        lap->lap_caller_ops->ap_fill_obdo(lap->lap_caller_data, cmd, oa);
        /* XXX woah, shouldn't we be altering more here?  size? */
        oa->o_id = lap->lap_loi_id;
}

static void lov_ap_completion(void *data, int cmd, struct obdo *oa, int rc)
{
        struct lov_async_page *lap = LAP_FROM_COOKIE(data);

        /* in a raid1 regime this would down a count of many ios
         * in flight, onl calling the caller_ops completion when all
         * the raid1 ios are complete */
        lap->lap_caller_ops->ap_completion(lap->lap_caller_data, cmd, oa, rc);
}

static struct obd_async_page_ops lov_async_page_ops = {
        .ap_make_ready =        lov_ap_make_ready,
        .ap_refresh_count =     lov_ap_refresh_count,
        .ap_fill_obdo =         lov_ap_fill_obdo,
        .ap_completion =        lov_ap_completion,
};

static int lov_prep_async_page(struct obd_export *exp,
                               struct lov_stripe_md *lsm,
                               struct lov_oinfo *loi, struct page *page,
                               obd_off offset, struct obd_async_page_ops *ops,
                               void *data, void **res)
{
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        struct lov_async_page *lap;
        int rc, stripe;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);
        LASSERT(loi == NULL);

        stripe = lov_stripe_number(lsm, offset);
        loi = &lsm->lsm_oinfo[stripe];

        if (obd_uuid_empty(&lov->tgts[loi->loi_ost_idx].uuid))
                RETURN(-EIO);
        if (lov->tgts[loi->loi_ost_idx].active == 0)
                RETURN(-EIO);
        if (lov->tgts[loi->loi_ost_idx].ltd_exp == NULL) {
                CERROR("ltd_exp == NULL, but OST idx %d doesn't appear to be "
                       "deleted or inactive.\n", loi->loi_ost_idx);
                RETURN(-EIO);
        }

        OBD_ALLOC(lap, sizeof(*lap));
        if (lap == NULL)
                RETURN(-ENOMEM);

        lap->lap_magic = LAP_MAGIC;
        lap->lap_caller_ops = ops;
        lap->lap_caller_data = data;

        /* FIXME handle multiple oscs after landing b_raid1 */
        lap->lap_stripe = stripe;
        switch (lsm->lsm_pattern) {
                case LOV_PATTERN_RAID0:
                        lov_stripe_offset(lsm, offset, lap->lap_stripe, 
                                          &lap->lap_sub_offset);
                        break;
                case LOV_PATTERN_CMOBD:
                        lap->lap_sub_offset = offset;
                        break;
                default:
                        LBUG();
        }

        /* so the callback doesn't need the lsm */
        lap->lap_loi_id = loi->loi_id;

        rc = obd_prep_async_page(lov->tgts[loi->loi_ost_idx].ltd_exp,
                                 lsm, loi, page, lap->lap_sub_offset,
                                 &lov_async_page_ops, lap,
                                 &lap->lap_sub_cookie);
        if (rc) {
                OBD_FREE(lap, sizeof(*lap));
                RETURN(rc);
        }
        CDEBUG(D_CACHE, "lap %p page %p cookie %p off "LPU64"\n", lap, page,
               lap->lap_sub_cookie, offset);
        *res = lap;
        RETURN(0);
}

static int lov_queue_async_io(struct obd_export *exp,
                              struct lov_stripe_md *lsm,
                              struct lov_oinfo *loi, void *cookie,
                              int cmd, obd_off off, int count,
                              obd_flags brw_flags, obd_flags async_flags)
{
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        struct lov_async_page *lap;
        int rc;

        LASSERT(loi == NULL);

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        lap = LAP_FROM_COOKIE(cookie);

        loi = &lsm->lsm_oinfo[lap->lap_stripe];

        rc = obd_queue_async_io(lov->tgts[loi->loi_ost_idx].ltd_exp, lsm,
                                loi, lap->lap_sub_cookie, cmd, off, count,
                                brw_flags, async_flags);
        RETURN(rc);
}

static int lov_set_async_flags(struct obd_export *exp,
                               struct lov_stripe_md *lsm,
                               struct lov_oinfo *loi, void *cookie,
                               obd_flags async_flags)
{
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        struct lov_async_page *lap;
        int rc;

        LASSERT(loi == NULL);

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        lap = LAP_FROM_COOKIE(cookie);

        loi = &lsm->lsm_oinfo[lap->lap_stripe];

        rc = obd_set_async_flags(lov->tgts[loi->loi_ost_idx].ltd_exp,
                                 lsm, loi, lap->lap_sub_cookie, async_flags);
        RETURN(rc);
}

static int lov_queue_group_io(struct obd_export *exp,
                              struct lov_stripe_md *lsm,
                              struct lov_oinfo *loi,
                              struct obd_io_group *oig, void *cookie,
                              int cmd, obd_off off, int count,
                              obd_flags brw_flags, obd_flags async_flags)
{
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        struct lov_async_page *lap;
        int rc;

        LASSERT(loi == NULL);

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        lap = LAP_FROM_COOKIE(cookie);

        loi = &lsm->lsm_oinfo[lap->lap_stripe];

        rc = obd_queue_group_io(lov->tgts[loi->loi_ost_idx].ltd_exp, lsm, loi,
                                oig, lap->lap_sub_cookie, cmd, off, count,
                                brw_flags, async_flags);
        RETURN(rc);
}

/* this isn't exactly optimal.  we may have queued sync io in oscs on
 * all stripes, but we don't record that fact at queue time.  so we
 * trigger sync io on all stripes. */
static int lov_trigger_group_io(struct obd_export *exp,
                                struct lov_stripe_md *lsm,
                                struct lov_oinfo *loi,
                                struct obd_io_group *oig)
{
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        int rc = 0, i, err;

        LASSERT(loi == NULL);

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        loi = lsm->lsm_oinfo;
        for (i = 0; i < lsm->lsm_stripe_count; i++, loi++) {
                if (lov->tgts[loi->loi_ost_idx].active == 0) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);
                        continue;
                }

                err = obd_trigger_group_io(lov->tgts[loi->loi_ost_idx].ltd_exp,
                                           lsm, loi, oig);
                if (rc == 0 && err != 0)
                        rc = err;
        };
        RETURN(rc);
}

static int lov_teardown_async_page(struct obd_export *exp,
                                   struct lov_stripe_md *lsm,
                                   struct lov_oinfo *loi, void *cookie)
{
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        struct lov_async_page *lap;
        int rc;

        LASSERT(loi == NULL);

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        lap = LAP_FROM_COOKIE(cookie);

        loi = &lsm->lsm_oinfo[lap->lap_stripe];

        rc = obd_teardown_async_page(lov->tgts[loi->loi_ost_idx].ltd_exp,
                                     lsm, loi, lap->lap_sub_cookie);
        if (rc) {
                CERROR("unable to teardown sub cookie %p: %d\n",
                       lap->lap_sub_cookie, rc);
                RETURN(rc);
        }
        OBD_FREE(lap, sizeof(*lap));
        RETURN(rc);
}

static int lov_enqueue(struct obd_export *exp, struct lov_stripe_md *lsm,
                       __u32 type, ldlm_policy_data_t *policy, __u32 mode,
                       int *flags, void *bl_cb, void *cp_cb, void *gl_cb,
                       void *data,__u32 lvb_len, void *lvb_swabber,
                       struct lustre_handle *lockh)
{
        struct lov_request_set *set;
        struct lov_request *req;
        struct list_head *pos;
        struct lustre_handle *lov_lockhp;
        struct lov_obd *lov;
        ldlm_error_t rc;
        int save_flags = *flags;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        /* we should never be asked to replay a lock this way. */
        LASSERT((*flags & LDLM_FL_REPLAY) == 0);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_enqueue_set(exp, lsm, policy, mode, lockh, &set);
        if (rc)
                RETURN(rc);

        list_for_each (pos, &set->set_list) {
                ldlm_policy_data_t sub_policy;
                req = list_entry(pos, struct lov_request, rq_link);
                lov_lockhp = set->set_lockh->llh_handles + req->rq_stripe;
                LASSERT(lov_lockhp);

                *flags = save_flags;
                sub_policy.l_extent.start = req->rq_extent.start;
                sub_policy.l_extent.end = req->rq_extent.end;

                rc = obd_enqueue(lov->tgts[req->rq_idx].ltd_exp, req->rq_md,
                                 type, &sub_policy, mode, flags, bl_cb,
                                 cp_cb, gl_cb, data, lvb_len, lvb_swabber,
                                 lov_lockhp);
                rc = lov_update_enqueue_set(set, req, rc, save_flags);
                if (rc != ELDLM_OK)
                        break;
        }

        lov_fini_enqueue_set(set, mode);
        RETURN(rc);
}

static int lov_match(struct obd_export *exp, struct lov_stripe_md *lsm,
                     __u32 type, ldlm_policy_data_t *policy, __u32 mode,
                     int *flags, void *data, struct lustre_handle *lockh)
{
        struct lov_request_set *set;
        struct lov_request *req;
        struct list_head *pos;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        struct lustre_handle *lov_lockhp;
        int lov_flags, rc = 0;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_match_set(exp, lsm, policy, mode, lockh, &set);
        if (rc)
                RETURN(rc);

        list_for_each (pos, &set->set_list) {
                ldlm_policy_data_t sub_policy;
                req = list_entry(pos, struct lov_request, rq_link);
                lov_lockhp = set->set_lockh->llh_handles + req->rq_stripe;
                LASSERT(lov_lockhp);

                sub_policy.l_extent.start = req->rq_extent.start;
                sub_policy.l_extent.end = req->rq_extent.end;
                lov_flags = *flags;

                rc = obd_match(lov->tgts[req->rq_idx].ltd_exp, req->rq_md,
                               type, &sub_policy, mode, &lov_flags, data,
                               lov_lockhp);
                rc = lov_update_match_set(set, req, rc);
                if (rc != 1)
                        break;
        }
        lov_fini_match_set(set, mode, *flags);
        RETURN(rc);
}

static int dump_missed_lock(struct ldlm_lock *lock, void *data)
{
        LDLM_ERROR(lock, "forgotten lock");
        return LDLM_ITER_CONTINUE;
}

static int lov_change_cbdata(struct obd_export *exp,
                             struct lov_stripe_md *lsm, ldlm_iterator_t it,
                             void *data)
{
        struct lov_obd *lov;
        struct lov_oinfo *loi;
        int rc = 0, i;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        LASSERT(lsm->lsm_object_gr > 0);

        lov = &exp->exp_obd->u.lov;
        for (i = 0,loi = lsm->lsm_oinfo; i < lsm->lsm_stripe_count; i++,loi++) {
                struct lov_stripe_md submd;
                if (lov->tgts[loi->loi_ost_idx].active == 0) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);
                        submd.lsm_object_id = loi->loi_id;
                        submd.lsm_object_gr = lsm->lsm_object_gr;
                        submd.lsm_stripe_count = 0;
                        obd_change_cbdata(lov->tgts[loi->loi_ost_idx].ltd_exp,
                                          &submd, dump_missed_lock, NULL);
                        /*continue;*/
                }

                submd.lsm_object_id = loi->loi_id;
                submd.lsm_object_gr = lsm->lsm_object_gr;
                submd.lsm_stripe_count = 0;
                rc = obd_change_cbdata(lov->tgts[loi->loi_ost_idx].ltd_exp,
                                       &submd, it, data);
        }
        RETURN(rc);
}

static int lov_cancel(struct obd_export *exp, struct lov_stripe_md *lsm,
                      __u32 mode, struct lustre_handle *lockh)
{
        struct lov_request_set *set;
        struct lov_request *req;
        struct list_head *pos;
        struct lov_obd *lov = &exp->exp_obd->u.lov;
        struct lustre_handle *lov_lockhp;
        int err = 0, rc = 0;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        LASSERT(lsm->lsm_object_gr > 0);

        LASSERT(lockh);
        lov = &exp->exp_obd->u.lov;
        rc = lov_prep_cancel_set(exp, lsm, mode, lockh, &set);
        if (rc)
                RETURN(rc);

        list_for_each (pos, &set->set_list) {
                req = list_entry(pos, struct lov_request, rq_link);
                lov_lockhp = set->set_lockh->llh_handles + req->rq_stripe;

                rc = obd_cancel(lov->tgts[req->rq_idx].ltd_exp, req->rq_md,
                                mode, lov_lockhp);
                rc = lov_update_common_set(set, req, rc);
                if (rc) {
                        CERROR("error: cancel objid "LPX64" subobj "
                               LPX64" on OST idx %d: rc = %d\n",
                               lsm->lsm_object_id,
                               req->rq_md->lsm_object_id, req->rq_idx, rc);
                        err = rc;
                }
 
        }
        lov_fini_cancel_set(set);
        RETURN(err);
}

static int lov_cancel_unused(struct obd_export *exp,
                             struct lov_stripe_md *lsm, 
			     int flags, void *opaque)
{
        struct lov_obd *lov;
        struct lov_oinfo *loi;
        int rc = 0, i;
        ENTRY;

        lov = &exp->exp_obd->u.lov;
        if (lsm == NULL) {
                for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                        int err = obd_cancel_unused(lov->tgts[i].ltd_exp,
                                                    NULL, flags, opaque);
                        if (!rc)
                                rc = err;
                }
                RETURN(rc);
        }

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        LASSERT(lsm->lsm_object_gr > 0);

        for (i = 0,loi = lsm->lsm_oinfo; i < lsm->lsm_stripe_count; i++,loi++) {
                struct lov_stripe_md submd;
                int err;

                if (lov->tgts[loi->loi_ost_idx].active == 0)
                        CDEBUG(D_HA, "lov idx %d inactive\n", loi->loi_ost_idx);

                submd.lsm_object_id = loi->loi_id;
                submd.lsm_object_gr = lsm->lsm_object_gr;
                submd.lsm_stripe_count = 0;
                err = obd_cancel_unused(lov->tgts[loi->loi_ost_idx].ltd_exp,
                                        &submd, flags, opaque);
                if (err && lov->tgts[loi->loi_ost_idx].active) {
                        CERROR("error: cancel unused objid "LPX64" subobj "LPX64
                               " on OST idx %d: rc = %d\n", lsm->lsm_object_id,
                               loi->loi_id, loi->loi_ost_idx, err);
                        if (!rc)
                                rc = err;
                }
        }
        RETURN(rc);
}

#define LOV_U64_MAX ((__u64)~0ULL)
#define LOV_SUM_MAX(tot, add)                                           \
        do {                                                            \
                if ((tot) + (add) < (tot))                              \
                        (tot) = LOV_U64_MAX;                            \
                else                                                    \
                        (tot) += (add);                                 \
        } while(0)

static int lov_statfs(struct obd_device *obd, struct obd_statfs *osfs,
                      unsigned long max_age)
{
        struct lov_obd *lov = &obd->u.lov;
        struct obd_statfs lov_sfs;
        int set = 0;
        int rc = 0;
        int i;
        ENTRY;


        /* We only get block data from the OBD */
        for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                int err;
                if (!lov->tgts[i].active) {
                        CDEBUG(D_HA, "lov idx %d inactive\n", i);
                        continue;
                }

                err = obd_statfs(class_exp2obd(lov->tgts[i].ltd_exp), &lov_sfs,
                                 max_age);
                if (err) {
                        if (lov->tgts[i].active && !rc)
                                rc = err;
                        continue;
                }

                if (!set) {
                        memcpy(osfs, &lov_sfs, sizeof(lov_sfs));
                        set = 1;
                } else {
                        osfs->os_bfree += lov_sfs.os_bfree;
                        osfs->os_bavail += lov_sfs.os_bavail;
                        osfs->os_blocks += lov_sfs.os_blocks;
                        /* XXX not sure about this one - depends on policy.
                         *   - could be minimum if we always stripe on all OBDs
                         *     (but that would be wrong for any other policy,
                         *     if one of the OBDs has no more objects left)
                         *   - could be sum if we stripe whole objects
                         *   - could be average, just to give a nice number
                         *
                         * To give a "reasonable" (if not wholly accurate)
                         * number, we divide the total number of free objects
                         * by expected stripe count (watch out for overflow).
                         */
                        LOV_SUM_MAX(osfs->os_files, lov_sfs.os_files);
                        LOV_SUM_MAX(osfs->os_ffree, lov_sfs.os_ffree);
                }
        }

        if (set) {
                __u32 expected_stripes = lov->desc.ld_default_stripe_count ?
                                         lov->desc.ld_default_stripe_count :
                                         lov->desc.ld_active_tgt_count;

                if (osfs->os_files != LOV_U64_MAX)
                        do_div(osfs->os_files, expected_stripes);
                if (osfs->os_ffree != LOV_U64_MAX)
                        do_div(osfs->os_ffree, expected_stripes);
        } else if (!rc)
                rc = -EIO;

        RETURN(rc);
}

static int lov_iocontrol(unsigned int cmd, struct obd_export *exp, int len,
                         void *karg, void *uarg)
{
        struct obd_device *obddev = class_exp2obd(exp);
        struct lov_obd *lov = &obddev->u.lov;
        int i, rc, count = lov->desc.ld_tgt_count;
        struct obd_uuid *uuidp;
        ENTRY;

        switch (cmd) {
        case OBD_IOC_LOV_GET_CONFIG: {
                struct obd_ioctl_data *data = karg;
                struct lov_tgt_desc *tgtdesc;
                struct lov_desc *desc;
                char *buf = NULL;
                __u32 *genp;

                buf = NULL;
                len = 0;
                if (obd_ioctl_getdata(&buf, &len, (void *)uarg))
                        RETURN(-EINVAL);

                data = (struct obd_ioctl_data *)buf;

                if (sizeof(*desc) > data->ioc_inllen1) {
                        obd_ioctl_freedata(buf, len);
                        RETURN(-EINVAL);
                }

                if (sizeof(uuidp->uuid) * count > data->ioc_inllen2) {
                        obd_ioctl_freedata(buf, len);
                        RETURN(-EINVAL);
                }

                if (sizeof(__u32) * count > data->ioc_inllen3) {
                        obd_ioctl_freedata(buf, len);
                        RETURN(-EINVAL);
                }

                desc = (struct lov_desc *)data->ioc_inlbuf1;
                memcpy(desc, &(lov->desc), sizeof(*desc));

                uuidp = (struct obd_uuid *)data->ioc_inlbuf2;
                genp = (__u32 *)data->ioc_inlbuf3;
                tgtdesc = lov->tgts;
                /* the uuid will be empty for deleted OSTs */
                for (i = 0; i < count; i++, uuidp++, genp++, tgtdesc++) {
                        obd_str2uuid(uuidp, (char *)tgtdesc->uuid.uuid);
                        *genp = tgtdesc->ltd_gen;
                }

                rc = copy_to_user((void *)uarg, buf, len);
                if (rc)
                        rc = -EFAULT;
                obd_ioctl_freedata(buf, len);
                break;
        }
        case LL_IOC_LOV_SETSTRIPE:
                rc = lov_setstripe(exp, karg, uarg);
                break;
        case LL_IOC_LOV_GETSTRIPE:
                rc = lov_getstripe(exp, karg, uarg);
                break;
        case LL_IOC_LOV_SETEA:
                rc = lov_setea(exp, karg, uarg);
                break;
        default: {
                int set = 0;
                if (count == 0)
                        RETURN(-ENOTTY);
                rc = 0;
                for (i = 0; i < count; i++) {
                        int err;

                        /* OST was deleted */
                        if (obd_uuid_empty(&lov->tgts[i].uuid))
                                continue;

                        err = obd_iocontrol(cmd, lov->tgts[i].ltd_exp,
                                            len, karg, uarg);
                        if (err) {
                                if (lov->tgts[i].active) {
                                        CERROR("error: iocontrol OSC %s on OST "
                                               "idx %d cmd %x: err = %d\n",
                                               lov->tgts[i].uuid.uuid, i,
                                               cmd, err);
                                        if (!rc)
                                                rc = err;
                                }
                        } else
                                set = 1;
                }
                if (!set && !rc)
                        rc = -EIO;
        }
        }

        RETURN(rc);
}

static int lov_get_info(struct obd_export *exp, __u32 keylen,
                        void *key, __u32 *vallen, void *val)
{
        struct obd_device *obddev = class_exp2obd(exp);
        struct lov_obd *lov = &obddev->u.lov;
        int i;
        ENTRY;

        if (!vallen || !val)
                RETURN(-EFAULT);

        if (keylen > strlen("lock_to_stripe") &&
            strcmp(key, "lock_to_stripe") == 0) {
                struct {
                        char name[16];
                        struct ldlm_lock *lock;
                        struct lov_stripe_md *lsm;
                } *data = key;
                struct lov_oinfo *loi;
                struct ldlm_res_id *res_id = &data->lock->l_resource->lr_name;
                __u32 *stripe = val;

                if (*vallen < sizeof(*stripe))
                        RETURN(-EFAULT);
                *vallen = sizeof(*stripe);

                /* XXX This is another one of those bits that will need to
                 * change if we ever actually support nested LOVs.  It uses
                 * the lock's export to find out which stripe it is. */
                /* XXX - it's assumed all the locks for deleted OSTs have
                 * been cancelled. Also, the export for deleted OSTs will
                 * be NULL and won't match the lock's export. */
                for (i = 0, loi = data->lsm->lsm_oinfo;
                     i < data->lsm->lsm_stripe_count;
                     i++, loi++) {
                        if (lov->tgts[loi->loi_ost_idx].ltd_exp ==
                                        data->lock->l_conn_export &&
                            loi->loi_id == res_id->name[0] &&
                            loi->loi_gr == res_id->name[2]) {
                                *stripe = i;
                                RETURN(0);
                        }
                }
                LDLM_ERROR(data->lock, "lock on inode without such object");
                dump_lsm(D_ERROR, data->lsm);
                portals_debug_dumpstack(NULL);
                RETURN(-ENXIO);
        } else if (keylen >= strlen("size_to_stripe") &&
                   strcmp(key, "size_to_stripe") == 0) {
                struct {
                        int stripe_number;
                        __u64 size;
                        struct lov_stripe_md *lsm;
                } *data = val;

                if (*vallen < sizeof(*data))
                        RETURN(-EFAULT);

                data->size = lov_size_to_stripe(data->lsm, data->size,
                                                data->stripe_number);
                RETURN(0);
        } else if (keylen >= strlen("last_id") && strcmp(key, "last_id") == 0) {
                __u32 size = sizeof(obd_id);
                obd_id *ids = val;
                int rc = 0;

                for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                        if (!lov->tgts[i].active)
                                continue;
                        rc = obd_get_info(lov->tgts[i].ltd_exp,
                                          keylen, key, &size, &(ids[i]));
                        if (rc != 0)
                                RETURN(rc);
                }
                RETURN(0);
        } else if (keylen >= strlen("lovdesc") && strcmp(key, "lovdesc") == 0) {
                struct lov_desc *desc_ret = val;
                *desc_ret = lov->desc;

                RETURN(0);
        }

        RETURN(-EINVAL);
}

static int lov_set_info(struct obd_export *exp, obd_count keylen,
                        void *key, obd_count vallen, void *val)
{
        struct obd_device *obddev = class_exp2obd(exp);
        struct lov_obd *lov = &obddev->u.lov;
        int i, rc = 0, err;
        ENTRY;

#define KEY_IS(str) \
        (keylen == strlen(str) && memcmp(key, str, keylen) == 0)

        if (KEY_IS("async")) {
                struct lov_desc *desc = &lov->desc;
                struct lov_tgt_desc *tgts = lov->tgts;

                if (vallen != sizeof(int))
                        RETURN(-EINVAL);
                lov->async = *((int*) val);

                for (i = 0; i < desc->ld_tgt_count; i++, tgts++) {
                        struct obd_uuid *tgt_uuid = &tgts->uuid;
                        struct obd_device *tgt_obd;

                        tgt_obd = class_find_client_obd(tgt_uuid,
                                                        OBD_OSC_DEVICENAME,
                                                        &obddev->obd_uuid);
                        if (!tgt_obd) {
                                CERROR("Target %s not attached\n",
                                        tgt_uuid->uuid);
                                if (!rc)
                                        rc = -EINVAL;
                                continue;
                        }

                        err = obd_set_info(tgt_obd->obd_self_export,
                                           keylen, key, vallen, val);
                        if (err) {
                                CERROR("Failed to set async on target %s\n",
                                        tgt_obd->obd_name);
                                if (!rc)
                                        rc = err;
                        }
                }
                RETURN(rc);
        }

        if (KEY_IS("mds_conn")) {
                if (vallen != sizeof(__u32))
                        RETURN(-EINVAL);
        } else if (KEY_IS("unlinked") || KEY_IS("unrecovery")) {
                if (vallen != 0)
                        RETURN(-EINVAL);
        } else if (KEY_IS("sec") || KEY_IS("sec_flags")) {
                struct lov_tgt_desc *tgt;
                struct obd_export *exp;
                int rc = 0, err, i;

                spin_lock(&lov->lov_lock);
                for (i = 0, tgt = lov->tgts; i < lov->desc.ld_tgt_count;
                     i++, tgt++) {
                        exp = tgt->ltd_exp;
                        /* during setup time the connections to osc might
                         * haven't been established.
                         */
                        if (exp == NULL) {
                                struct obd_device *tgt_obd;

                                tgt_obd = class_find_client_obd(&tgt->uuid,
                                                                OBD_OSC_DEVICENAME,
                                                                &obddev->obd_uuid);
                                if (!tgt_obd) {
                                        CERROR("can't set security flavor, "
                                               "device %s not attached?\n",
                                                tgt->uuid.uuid);
                                        rc = -EINVAL;
                                        continue;
                                }
                                exp = tgt_obd->obd_self_export;
                        }

                        err = obd_set_info(exp, keylen, key, vallen, val);
                        if (!rc)
                                rc = err;
                }
                spin_unlock(&lov->lov_lock);

                RETURN(rc);
        } else if (KEY_IS("flush_cred") || KEY_IS("crypto_cb")) {
                struct lov_tgt_desc *tgt;
                int rc = 0, i;

                for (i = 0, tgt = lov->tgts; i < lov->desc.ld_tgt_count;
                     i++, tgt++) {
                        if (!tgt->ltd_exp)
                                continue;
                        rc = obd_set_info(tgt->ltd_exp,
                                          keylen, key, vallen, val);
                        if (rc)
                                RETURN(rc);
                }

                RETURN(0);
        } else {
                RETURN(-EINVAL);
        }

        for (i = 0; i < lov->desc.ld_tgt_count; i++) {
                if (val && !obd_uuid_equals(val, &lov->tgts[i].uuid))
                        continue;

                if (!val && !lov->tgts[i].active)
                        continue;

                err = obd_set_info(lov->tgts[i].ltd_exp,
                                  keylen, key, vallen, val);
                if (!rc)
                        rc = err;
        }
        RETURN(rc);
#undef KEY_IS
}

#if 0
struct lov_multi_wait {
        struct ldlm_lock *lock;
        wait_queue_t      wait;
        int               completed;
        int               generation;
};

int lov_complete_many(struct obd_export *exp, struct lov_stripe_md *lsm,
                      struct lustre_handle *lockh)
{
        struct lov_lock_handles *lov_lockh = NULL;
        struct lustre_handle *lov_lockhp;
        struct lov_obd *lov;
        struct lov_oinfo *loi;
        struct lov_multi_wait *queues;
        int rc = 0, i;
        ENTRY;

        if (lsm_bad_magic(lsm))
                RETURN(-EINVAL);

        if (!exp || !exp->exp_obd)
                RETURN(-ENODEV);

        LASSERT(lockh != NULL);
        if (lsm->lsm_stripe_count > 1) {
                lov_lockh = lov_handle2llh(lockh);
                if (lov_lockh == NULL) {
                        CERROR("LOV: invalid lov lock handle %p\n", lockh);
                        RETURN(-EINVAL);
                }

                lov_lockhp = lov_lockh->llh_handles;
        } else {
                lov_lockhp = lockh;
        }

        OBD_ALLOC(queues, lsm->lsm_stripe_count * sizeof(*queues));
        if (queues == NULL)
                GOTO(out, rc = -ENOMEM);

        lov = &exp->exp_obd->u.lov;
        for (i = 0, loi = lsm->lsm_oinfo; i < lsm->lsm_stripe_count;
             i++, loi++, lov_lockhp++) {
                struct ldlm_lock *lock;
                struct obd_device *obd;
                unsigned long irqflags;

                lock = ldlm_handle2lock(lov_lockhp);
                if (lock == NULL) {
                        CDEBUG(D_HA, "lov idx %d subobj "LPX64" no lock?\n",
                               loi->loi_ost_idx, loi->loi_id);
                        queues[i].completed = 1;
                        continue;
                }

                queues[i].lock = lock;
                init_waitqueue_entry(&(queues[i].wait), current);
                add_wait_queue(lock->l_waitq, &(queues[i].wait));

                obd = class_exp2obd(lock->l_conn_export);
                if (obd != NULL)
                        imp = obd->u.cli.cl_import;
                if (imp != NULL) {
                        spin_lock_irqsave(&imp->imp_lock, irqflags);
                        queues[i].generation = imp->imp_generation;
                        spin_unlock_irqrestore(&imp->imp_lock, irqflags);
                }
        }

        lwi = LWI_TIMEOUT_INTR(obd_timeout * HZ, ldlm_expired_completion_wait,
                               interrupted_completion_wait, &lwd);
        rc = l_wait_event_added(check_multi_complete(queues, lsm), &lwi);

        for (i = 0; i < lsm->lsm_stripe_count; i++)
                remove_wait_queue(lock->l_waitq, &(queues[i].wait));

        if (rc == -EINTR || rc == -ETIMEDOUT) {


        }

 out:
        if (lov_lockh != NULL)
                lov_llh_put(lov_lockh);
        RETURN(rc);
}
#endif

struct obd_ops lov_obd_ops = {
        .o_owner               = THIS_MODULE,
        .o_attach              = lov_attach,
        .o_detach              = lov_detach,
        .o_setup               = lov_setup,
        .o_cleanup             = lov_cleanup,
        .o_process_config      = lov_process_config,
        .o_connect             = lov_connect,
        .o_disconnect          = lov_disconnect,
        .o_statfs              = lov_statfs,
        .o_packmd              = lov_packmd,
        .o_unpackmd            = lov_unpackmd,
        .o_revalidate_md       = lov_revalidate_md,
        .o_create              = lov_create,
        .o_destroy             = lov_destroy,
        .o_getattr             = lov_getattr,
        .o_getattr_async       = lov_getattr_async,
        .o_setattr             = lov_setattr,
        .o_brw                 = lov_brw,
        .o_brw_async           = lov_brw_async,
        .o_prep_async_page     = lov_prep_async_page,
        .o_queue_async_io      = lov_queue_async_io,
        .o_set_async_flags     = lov_set_async_flags,
        .o_queue_group_io      = lov_queue_group_io,
        .o_trigger_group_io    = lov_trigger_group_io,
        .o_teardown_async_page = lov_teardown_async_page,
        .o_adjust_kms          = lov_adjust_kms,
        .o_punch               = lov_punch,
        .o_sync                = lov_sync,
        .o_enqueue             = lov_enqueue,
        .o_match               = lov_match,
        .o_change_cbdata       = lov_change_cbdata,
        .o_cancel              = lov_cancel,
        .o_cancel_unused       = lov_cancel_unused,
        .o_iocontrol           = lov_iocontrol,
        .o_get_info            = lov_get_info,
        .o_set_info            = lov_set_info,
        .o_llog_init           = lov_llog_init,
        .o_llog_finish         = lov_llog_finish,
        .o_notify              = lov_notify,
};

int __init lov_init(void)
{
        struct lprocfs_static_vars lvars;
        int rc;
        ENTRY;

        lprocfs_init_vars(lov, &lvars);
        rc = class_register_type(&lov_obd_ops, NULL, lvars.module_vars,
                                 OBD_LOV_DEVICENAME);
        RETURN(rc);
}

#ifdef __KERNEL__
static void /*__exit*/ lov_exit(void)
{
        class_unregister_type(OBD_LOV_DEVICENAME);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Lustre Logical Object Volume OBD driver");
MODULE_LICENSE("GPL");

module_init(lov_init);
module_exit(lov_exit);
#endif

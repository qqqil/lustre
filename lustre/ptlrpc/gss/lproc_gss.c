/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#define DEBUG_SUBSYSTEM S_SEC
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/mutex.h>

#include <obd.h>
#include <obd_class.h>
#include <obd_support.h>
#include <lustre_net.h>
#include <lustre_import.h>
#include <lprocfs_status.h>
#include <lustre_sec.h>

#include "gss_err.h"
#include "gss_internal.h"
#include "gss_api.h"

static struct dentry *gss_debugfs_dir_lk;
static struct dentry *gss_debugfs_dir;

/*
 * statistic of "out-of-sequence-window"
 */
static struct {
	spinlock_t	oos_lock;
	atomic_t	oos_cli_count;		/* client occurrence */
	int		oos_cli_behind;		/* client max seqs behind */
	atomic_t	oos_svc_replay[3];	/* server replay detected */
	atomic_t	oos_svc_pass[3];	/* server verified ok */
} gss_stat_oos = {
	.oos_cli_count	= ATOMIC_INIT(0),
	.oos_cli_behind	= 0,
	.oos_svc_replay	= { ATOMIC_INIT(0), },
	.oos_svc_pass	= { ATOMIC_INIT(0), },
};

void gss_stat_oos_record_cli(int behind)
{
	atomic_inc(&gss_stat_oos.oos_cli_count);

	spin_lock(&gss_stat_oos.oos_lock);
	if (behind > gss_stat_oos.oos_cli_behind)
		gss_stat_oos.oos_cli_behind = behind;
	spin_unlock(&gss_stat_oos.oos_lock);
}

void gss_stat_oos_record_svc(int phase, int replay)
{
	LASSERT(phase >= 0 && phase <= 2);

	if (replay)
		atomic_inc(&gss_stat_oos.oos_svc_replay[phase]);
	else
		atomic_inc(&gss_stat_oos.oos_svc_pass[phase]);
}

static int gss_proc_oos_seq_show(struct seq_file *m, void *v)
{
	seq_printf(m, "seqwin:		   %u\n"
		   "backwin:		%u\n"
		   "client fall behind seqwin\n"
		   "  occurrence:	%d\n"
		   "  max seq behind:	%d\n"
		   "server replay detected:\n"
		   "  phase 0:		%d\n"
		   "  phase 1:		%d\n"
		   "  phase 2:		%d\n"
		   "server verify ok:\n"
		   "  phase 2:		%d\n",
		   GSS_SEQ_WIN_MAIN,
		   GSS_SEQ_WIN_BACK,
		   atomic_read(&gss_stat_oos.oos_cli_count),
		   gss_stat_oos.oos_cli_behind,
		   atomic_read(&gss_stat_oos.oos_svc_replay[0]),
		   atomic_read(&gss_stat_oos.oos_svc_replay[1]),
		   atomic_read(&gss_stat_oos.oos_svc_replay[2]),
		   atomic_read(&gss_stat_oos.oos_svc_pass[2]));
	return 0;
}
LDEBUGFS_SEQ_FOPS_RO(gss_proc_oos);

static ssize_t
gss_proc_write_secinit(struct file *file, const char *buffer,
				  size_t count, loff_t *off)
{
        int rc;

        rc = gss_do_ctx_init_rpc((char *) buffer, count);
        if (rc) {
                LASSERT(rc < 0);
                return rc;
        }
	return count;
}

static const struct file_operations gss_proc_secinit = {
	.write = gss_proc_write_secinit,
};

static struct lprocfs_vars gss_debugfs_vars[] = {
	{ .name	=	"replays",
	  .fops	=	&gss_proc_oos_fops	},
	{ .name	=	"init_channel",
	  .fops	=	&gss_proc_secinit,
	  .proc_mode =	0222			},
	{ NULL }
};

/*
 * for userspace helper lgss_keyring.
 *
 * debug_level: [0, 4], defined in utils/gss/lgss_utils.h
 */
static int gss_lk_debug_level = 1;

static int gss_lk_proc_dl_seq_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", gss_lk_debug_level);
	return 0;
}

static ssize_t
gss_lk_proc_dl_seq_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *off)
{
	unsigned int val;
	int rc;

	rc = kstrtouint_from_user(buffer, count, 0, &val);
	if (rc < 0)
		return rc;

	if (val > 4)
		return -ERANGE;

	gss_lk_debug_level = val;

	return count;
}
LDEBUGFS_SEQ_FOPS(gss_lk_proc_dl);

static struct lprocfs_vars gss_lk_debugfs_vars[] = {
	{ .name	=	"debug_level",
	  .fops	=	&gss_lk_proc_dl_fops	},
	{ NULL }
};

void gss_exit_tunables(void)
{
	if (!IS_ERR_OR_NULL(gss_debugfs_dir_lk))
		ldebugfs_remove(&gss_debugfs_dir_lk);

	if (!IS_ERR_OR_NULL(gss_debugfs_dir))
		ldebugfs_remove(&gss_debugfs_dir);
}

int gss_init_tunables(void)
{
	int	rc;

	spin_lock_init(&gss_stat_oos.oos_lock);

	gss_debugfs_dir = ldebugfs_register("gss", sptlrpc_debugfs_dir,
					    gss_debugfs_vars, NULL);
	if (IS_ERR_OR_NULL(gss_debugfs_dir)) {
		rc = gss_debugfs_dir ? PTR_ERR(gss_debugfs_dir) : -ENOMEM;
		gss_debugfs_dir = NULL;
		GOTO(out, rc);
	}

	gss_debugfs_dir_lk = ldebugfs_register("lgss_keyring", gss_debugfs_dir,
					       gss_lk_debugfs_vars, NULL);
	if (IS_ERR(gss_debugfs_dir_lk)) {
		rc = gss_debugfs_dir_lk ? PTR_ERR(gss_debugfs_dir_lk)
					  : -ENOMEM;
		gss_debugfs_dir_lk = NULL;
		GOTO(out, rc);
	}

	return 0;

out:
	CERROR("failed to initialize gss lproc entries: %d\n", rc);
	gss_exit_tunables();
	return rc;
}

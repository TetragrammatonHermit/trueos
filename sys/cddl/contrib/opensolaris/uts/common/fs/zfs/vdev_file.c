/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2013 by Delphix. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/vdev_file.h>
#include <sys/vdev_impl.h>
#include <sys/zio.h>
#include <sys/fs/zfs.h>
#include <sys/fm/fs/zfs.h>

/*
 * Virtual device vector for files.
 */

static void
vdev_file_hold(vdev_t *vd)
{
	ASSERT(vd->vdev_path != NULL);
}

static void
vdev_file_rele(vdev_t *vd)
{
	ASSERT(vd->vdev_path != NULL);
}

static int
vdev_file_open(vdev_t *vd, uint64_t *psize, uint64_t *max_psize,
    uint64_t *logical_ashift, uint64_t *physical_ashift)
{
	vdev_file_t *vf;
	vnode_t *vp;
	vattr_t vattr;
	int error, vfslocked;

	/*
	 * We must have a pathname, and it must be absolute.
	 */
	if (vd->vdev_path == NULL || vd->vdev_path[0] != '/') {
		vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Reopen the device if it's not currently open.  Otherwise,
	 * just update the physical size of the device.
	 */
	if (vd->vdev_tsd != NULL) {
		ASSERT(vd->vdev_reopening);
		vf = vd->vdev_tsd;
		vp = vf->vf_vnode;
		goto skip_open;
	}

	vf = vd->vdev_tsd = kmem_zalloc(sizeof (vdev_file_t), KM_SLEEP);

	/*
	 * We always open the files from the root of the global zone, even if
	 * we're in a local zone.  If the user has gotten to this point, the
	 * administrator has already decided that the pool should be available
	 * to local zone users, so the underlying devices should be as well.
	 */
	ASSERT(vd->vdev_path != NULL && vd->vdev_path[0] == '/');
	error = vn_openat(vd->vdev_path + 1, UIO_SYSSPACE,
	    spa_mode(vd->vdev_spa) | FOFFMAX, 0, &vp, 0, 0, rootdir, -1);

	if (error) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		kmem_free(vd->vdev_tsd, sizeof (vdev_file_t));
		vd->vdev_tsd = NULL;
		return (error);
	}

	vf->vf_vnode = vp;

#ifdef _KERNEL
	/*
	 * Make sure it's a regular file.
	 */
	if (vp->v_type != VREG) {
#ifdef __FreeBSD__
		(void) VOP_CLOSE(vp, spa_mode(vd->vdev_spa), 1, 0, kcred, NULL);
#endif
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
#ifdef __FreeBSD__
		kmem_free(vd->vdev_tsd, sizeof (vdev_file_t));
		vd->vdev_tsd = NULL;
#endif
		return (SET_ERROR(ENODEV));
	}
#endif	/* _KERNEL */

skip_open:
	/*
	 * Determine the physical size of the file.
	 */
	vattr.va_mask = AT_SIZE;
	vfslocked = VFS_LOCK_GIANT(vp->v_mount);
	vn_lock(vp, LK_SHARED | LK_RETRY);
	error = VOP_GETATTR(vp, &vattr, kcred);
	VOP_UNLOCK(vp, 0);
	VFS_UNLOCK_GIANT(vfslocked);
	if (error) {
		(void) VOP_CLOSE(vp, spa_mode(vd->vdev_spa), 1, 0, kcred, NULL);
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		kmem_free(vd->vdev_tsd, sizeof (vdev_file_t));
		vd->vdev_tsd = NULL;
		return (error);
	}

	*max_psize = *psize = vattr.va_size;
	*logical_ashift = SPA_MINBLOCKSHIFT;
	*physical_ashift = SPA_MINBLOCKSHIFT;

	return (0);
}

static void
vdev_file_close(vdev_t *vd)
{
	vdev_file_t *vf = vd->vdev_tsd;

	if (vd->vdev_reopening || vf == NULL)
		return;

	if (vf->vf_vnode != NULL) {
		(void) VOP_CLOSE(vf->vf_vnode, spa_mode(vd->vdev_spa), 1, 0,
		    kcred, NULL);
	}

	vd->vdev_delayed_close = B_FALSE;
	kmem_free(vf, sizeof (vdev_file_t));
	vd->vdev_tsd = NULL;
}

static int
vdev_file_io_start(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	vdev_file_t *vf;
	vnode_t *vp;
	ssize_t resid;

	if (!vdev_readable(vd)) {
		zio->io_error = SET_ERROR(ENXIO);
		zio_interrupt(zio);
		return (ZIO_PIPELINE_STOP);
	}

	vf = vd->vdev_tsd;
	vp = vf->vf_vnode;

	if (zio->io_type == ZIO_TYPE_IOCTL) {
		switch (zio->io_cmd) {
		case DKIOCFLUSHWRITECACHE:
			zio->io_error = VOP_FSYNC(vp, FSYNC | FDSYNC,
			    kcred, NULL);
			break;
		default:
			zio->io_error = SET_ERROR(ENOTSUP);
		}

		zio_interrupt(zio);
		return (ZIO_PIPELINE_STOP);
	}

	zio->io_error = vn_rdwr(zio->io_type == ZIO_TYPE_READ ?
	    UIO_READ : UIO_WRITE, vp, zio->io_data, zio->io_size,
	    zio->io_offset, UIO_SYSSPACE, 0, RLIM64_INFINITY, kcred, &resid);

	if (resid != 0 && zio->io_error == 0)
		zio->io_error = ENOSPC;

	zio_interrupt(zio);

	return (ZIO_PIPELINE_STOP);
}

/* ARGSUSED */
static void
vdev_file_io_done(zio_t *zio)
{
}

vdev_ops_t vdev_file_ops = {
	vdev_file_open,
	vdev_file_close,
	vdev_default_asize,
	vdev_file_io_start,
	vdev_file_io_done,
	NULL,
	vdev_file_hold,
	vdev_file_rele,
	VDEV_TYPE_FILE,		/* name of this vdev type */
	B_TRUE			/* leaf vdev */
};

/*
 * From userland we access disks just like files.
 */
#ifndef _KERNEL

vdev_ops_t vdev_disk_ops = {
	vdev_file_open,
	vdev_file_close,
	vdev_default_asize,
	vdev_file_io_start,
	vdev_file_io_done,
	NULL,
	vdev_file_hold,
	vdev_file_rele,
	VDEV_TYPE_DISK,		/* name of this vdev type */
	B_TRUE			/* leaf vdev */
};

#endif

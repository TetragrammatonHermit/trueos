/*-
 * Copyright (c) 2003 Silicon Graphics International Corp.
 * Copyright (c) 2009-2011 Spectra Logic Corporation
 * Copyright (c) 2012 The FreeBSD Foundation
 * All rights reserved.
 *
 * Portions of this software were developed by Edward Tomasz Napierala
 * under sponsorship from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 *
 * $Id: //depot/users/kenm/FreeBSD-test2/sys/cam/ctl/ctl_backend_block.c#5 $
 */
/*
 * CAM Target Layer driver backend for block devices.
 *
 * Author: Ken Merry <ken@FreeBSD.org>
 */
#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <opt_kdtrace.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/types.h>
#include <sys/kthread.h>
#include <sys/bio.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/malloc.h>
#include <sys/conf.h>
#include <sys/ioccom.h>
#include <sys/queue.h>
#include <sys/sbuf.h>
#include <sys/endian.h>
#include <sys/uio.h>
#include <sys/buf.h>
#include <sys/taskqueue.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/mount.h>
#include <sys/disk.h>
#include <sys/fcntl.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/pcpu.h>
#include <sys/module.h>
#include <sys/sdt.h>
#include <sys/devicestat.h>
#include <sys/sysctl.h>

#include <geom/geom.h>

#include <cam/cam.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_da.h>
#include <cam/ctl/ctl_io.h>
#include <cam/ctl/ctl.h>
#include <cam/ctl/ctl_backend.h>
#include <cam/ctl/ctl_frontend_internal.h>
#include <cam/ctl/ctl_ioctl.h>
#include <cam/ctl/ctl_scsi_all.h>
#include <cam/ctl/ctl_error.h>

/*
 * The idea here is that we'll allocate enough S/G space to hold a 1MB
 * I/O.  If we get an I/O larger than that, we'll split it.
 */
#define	CTLBLK_HALF_IO_SIZE	(512 * 1024)
#define	CTLBLK_MAX_IO_SIZE	(CTLBLK_HALF_IO_SIZE * 2)
#define	CTLBLK_MAX_SEG		MAXPHYS
#define	CTLBLK_HALF_SEGS	MAX(CTLBLK_HALF_IO_SIZE / CTLBLK_MAX_SEG, 1)
#define	CTLBLK_MAX_SEGS		(CTLBLK_HALF_SEGS * 2)

#ifdef CTLBLK_DEBUG
#define DPRINTF(fmt, args...) \
    printf("cbb(%s:%d): " fmt, __FUNCTION__, __LINE__, ##args)
#else
#define DPRINTF(fmt, args...) do {} while(0)
#endif

#define PRIV(io)	\
    ((struct ctl_ptr_len_flags *)&(io)->io_hdr.ctl_private[CTL_PRIV_BACKEND])
#define ARGS(io)	\
    ((struct ctl_lba_len_flags *)&(io)->io_hdr.ctl_private[CTL_PRIV_LBA_LEN])

SDT_PROVIDER_DEFINE(cbb);

typedef enum {
	CTL_BE_BLOCK_LUN_UNCONFIGURED	= 0x01,
	CTL_BE_BLOCK_LUN_CONFIG_ERR	= 0x02,
	CTL_BE_BLOCK_LUN_WAITING	= 0x04,
	CTL_BE_BLOCK_LUN_MULTI_THREAD	= 0x08
} ctl_be_block_lun_flags;

typedef enum {
	CTL_BE_BLOCK_NONE,
	CTL_BE_BLOCK_DEV,
	CTL_BE_BLOCK_FILE
} ctl_be_block_type;

struct ctl_be_block_devdata {
	struct cdev *cdev;
	struct cdevsw *csw;
	int dev_ref;
};

struct ctl_be_block_filedata {
	struct ucred *cred;
};

union ctl_be_block_bedata {
	struct ctl_be_block_devdata dev;
	struct ctl_be_block_filedata file;
};

struct ctl_be_block_io;
struct ctl_be_block_lun;

typedef void (*cbb_dispatch_t)(struct ctl_be_block_lun *be_lun,
			       struct ctl_be_block_io *beio);

/*
 * Backend LUN structure.  There is a 1:1 mapping between a block device
 * and a backend block LUN, and between a backend block LUN and a CTL LUN.
 */
struct ctl_be_block_lun {
	struct ctl_lun_create_params params;
	struct ctl_block_disk *disk;
	char lunname[32];
	char *dev_path;
	ctl_be_block_type dev_type;
	struct vnode *vn;
	union ctl_be_block_bedata backend;
	cbb_dispatch_t dispatch;
	cbb_dispatch_t lun_flush;
	cbb_dispatch_t unmap;
	uma_zone_t lun_zone;
	uint64_t size_blocks;
	uint64_t size_bytes;
	uint32_t blocksize;
	int blocksize_shift;
	uint16_t pblockexp;
	uint16_t pblockoff;
	struct ctl_be_block_softc *softc;
	struct devstat *disk_stats;
	ctl_be_block_lun_flags flags;
	STAILQ_ENTRY(ctl_be_block_lun) links;
	struct ctl_be_lun ctl_be_lun;
	struct taskqueue *io_taskqueue;
	struct task io_task;
	int num_threads;
	STAILQ_HEAD(, ctl_io_hdr) input_queue;
	STAILQ_HEAD(, ctl_io_hdr) config_write_queue;
	STAILQ_HEAD(, ctl_io_hdr) datamove_queue;
	struct mtx io_lock;
	struct mtx queue_lock;
};

/*
 * Overall softc structure for the block backend module.
 */
struct ctl_be_block_softc {
	struct mtx			 lock;
	int				 num_disks;
	STAILQ_HEAD(, ctl_block_disk)	 disk_list;
	int				 num_luns;
	STAILQ_HEAD(, ctl_be_block_lun)	 lun_list;
};

static struct ctl_be_block_softc backend_block_softc;

/*
 * Per-I/O information.
 */
struct ctl_be_block_io {
	union ctl_io			*io;
	struct ctl_sg_entry		sg_segs[CTLBLK_MAX_SEGS];
	struct iovec			xiovecs[CTLBLK_MAX_SEGS];
	int				bio_cmd;
	int				num_segs;
	int				num_bios_sent;
	int				num_bios_done;
	int				send_complete;
	int				num_errors;
	struct bintime			ds_t0;
	devstat_tag_type		ds_tag_type;
	devstat_trans_flags		ds_trans_type;
	uint64_t			io_len;
	uint64_t			io_offset;
	struct ctl_be_block_softc	*softc;
	struct ctl_be_block_lun		*lun;
	void (*beio_cont)(struct ctl_be_block_io *beio); /* to continue processing */
};

static int cbb_num_threads = 14;
TUNABLE_INT("kern.cam.ctl.block.num_threads", &cbb_num_threads);
SYSCTL_NODE(_kern_cam_ctl, OID_AUTO, block, CTLFLAG_RD, 0,
	    "CAM Target Layer Block Backend");
SYSCTL_INT(_kern_cam_ctl_block, OID_AUTO, num_threads, CTLFLAG_RW,
           &cbb_num_threads, 0, "Number of threads per backing file");

static struct ctl_be_block_io *ctl_alloc_beio(struct ctl_be_block_softc *softc);
static void ctl_free_beio(struct ctl_be_block_io *beio);
static void ctl_complete_beio(struct ctl_be_block_io *beio);
static int ctl_be_block_move_done(union ctl_io *io);
static void ctl_be_block_biodone(struct bio *bio);
static void ctl_be_block_flush_file(struct ctl_be_block_lun *be_lun,
				    struct ctl_be_block_io *beio);
static void ctl_be_block_dispatch_file(struct ctl_be_block_lun *be_lun,
				       struct ctl_be_block_io *beio);
static void ctl_be_block_flush_dev(struct ctl_be_block_lun *be_lun,
				   struct ctl_be_block_io *beio);
static void ctl_be_block_unmap_dev(struct ctl_be_block_lun *be_lun,
				   struct ctl_be_block_io *beio);
static void ctl_be_block_dispatch_dev(struct ctl_be_block_lun *be_lun,
				      struct ctl_be_block_io *beio);
static void ctl_be_block_cw_dispatch(struct ctl_be_block_lun *be_lun,
				    union ctl_io *io);
static void ctl_be_block_dispatch(struct ctl_be_block_lun *be_lun,
				  union ctl_io *io);
static void ctl_be_block_worker(void *context, int pending);
static int ctl_be_block_submit(union ctl_io *io);
static int ctl_be_block_ioctl(struct cdev *dev, u_long cmd, caddr_t addr,
				   int flag, struct thread *td);
static int ctl_be_block_open_file(struct ctl_be_block_lun *be_lun,
				  struct ctl_lun_req *req);
static int ctl_be_block_open_dev(struct ctl_be_block_lun *be_lun,
				 struct ctl_lun_req *req);
static int ctl_be_block_close(struct ctl_be_block_lun *be_lun);
static int ctl_be_block_open(struct ctl_be_block_softc *softc,
			     struct ctl_be_block_lun *be_lun,
			     struct ctl_lun_req *req);
static int ctl_be_block_create(struct ctl_be_block_softc *softc,
			       struct ctl_lun_req *req);
static int ctl_be_block_rm(struct ctl_be_block_softc *softc,
			   struct ctl_lun_req *req);
static int ctl_be_block_modify_file(struct ctl_be_block_lun *be_lun,
				  struct ctl_lun_req *req);
static int ctl_be_block_modify_dev(struct ctl_be_block_lun *be_lun,
				 struct ctl_lun_req *req);
static int ctl_be_block_modify(struct ctl_be_block_softc *softc,
			   struct ctl_lun_req *req);
static void ctl_be_block_lun_shutdown(void *be_lun);
static void ctl_be_block_lun_config_status(void *be_lun,
					   ctl_lun_config_status status);
static int ctl_be_block_config_write(union ctl_io *io);
static int ctl_be_block_config_read(union ctl_io *io);
static int ctl_be_block_lun_info(void *be_lun, struct sbuf *sb);
int ctl_be_block_init(void);

static struct ctl_backend_driver ctl_be_block_driver = 
{
	.name = "block",
	.flags = CTL_BE_FLAG_HAS_CONFIG,
	.init = ctl_be_block_init,
	.data_submit = ctl_be_block_submit,
	.data_move_done = ctl_be_block_move_done,
	.config_read = ctl_be_block_config_read,
	.config_write = ctl_be_block_config_write,
	.ioctl = ctl_be_block_ioctl,
	.lun_info = ctl_be_block_lun_info
};

MALLOC_DEFINE(M_CTLBLK, "ctlblk", "Memory used for CTL block backend");
CTL_BACKEND_DECLARE(cbb, ctl_be_block_driver);

static uma_zone_t beio_zone;

static struct ctl_be_block_io *
ctl_alloc_beio(struct ctl_be_block_softc *softc)
{
	struct ctl_be_block_io *beio;

	beio = uma_zalloc(beio_zone, M_WAITOK | M_ZERO);
	beio->softc = softc;
	return (beio);
}

static void
ctl_free_beio(struct ctl_be_block_io *beio)
{
	int duplicate_free;
	int i;

	duplicate_free = 0;

	for (i = 0; i < beio->num_segs; i++) {
		if (beio->sg_segs[i].addr == NULL)
			duplicate_free++;

		uma_zfree(beio->lun->lun_zone, beio->sg_segs[i].addr);
		beio->sg_segs[i].addr = NULL;

		/* For compare we had two equal S/G lists. */
		if (ARGS(beio->io)->flags & CTL_LLF_COMPARE) {
			uma_zfree(beio->lun->lun_zone,
			    beio->sg_segs[i + CTLBLK_HALF_SEGS].addr);
			beio->sg_segs[i + CTLBLK_HALF_SEGS].addr = NULL;
		}
	}

	if (duplicate_free > 0) {
		printf("%s: %d duplicate frees out of %d segments\n", __func__,
		       duplicate_free, beio->num_segs);
	}

	uma_zfree(beio_zone, beio);
}

static void
ctl_complete_beio(struct ctl_be_block_io *beio)
{
	union ctl_io *io = beio->io;

	if (beio->beio_cont != NULL) {
		beio->beio_cont(beio);
	} else {
		ctl_free_beio(beio);
		ctl_data_submit_done(io);
	}
}

static int
ctl_be_block_move_done(union ctl_io *io)
{
	struct ctl_be_block_io *beio;
	struct ctl_be_block_lun *be_lun;
	struct ctl_lba_len_flags *lbalen;
#ifdef CTL_TIME_IO
	struct bintime cur_bt;
#endif
	int i;

	beio = (struct ctl_be_block_io *)PRIV(io)->ptr;
	be_lun = beio->lun;

	DPRINTF("entered\n");

#ifdef CTL_TIME_IO
	getbintime(&cur_bt);
	bintime_sub(&cur_bt, &io->io_hdr.dma_start_bt);
	bintime_add(&io->io_hdr.dma_bt, &cur_bt);
	io->io_hdr.num_dmas++;
#endif  
	io->scsiio.kern_rel_offset += io->scsiio.kern_data_len;

	/*
	 * We set status at this point for read commands, and write
	 * commands with errors.
	 */
	if ((io->io_hdr.port_status == 0) &&
	    ((io->io_hdr.flags & CTL_FLAG_ABORT) == 0) &&
	    ((io->io_hdr.status & CTL_STATUS_MASK) == CTL_STATUS_NONE)) {
		lbalen = ARGS(beio->io);
		if (lbalen->flags & CTL_LLF_READ) {
			ctl_set_success(&io->scsiio);
		} else if (lbalen->flags & CTL_LLF_COMPARE) {
			/* We have two data blocks ready for comparison. */
			for (i = 0; i < beio->num_segs; i++) {
				if (memcmp(beio->sg_segs[i].addr,
				    beio->sg_segs[i + CTLBLK_HALF_SEGS].addr,
				    beio->sg_segs[i].len) != 0)
					break;
			}
			if (i < beio->num_segs)
				ctl_set_sense(&io->scsiio,
				    /*current_error*/ 1,
				    /*sense_key*/ SSD_KEY_MISCOMPARE,
				    /*asc*/ 0x1D,
				    /*ascq*/ 0x00,
				    SSD_ELEM_NONE);
			else
				ctl_set_success(&io->scsiio);
		}
	}
	else if ((io->io_hdr.port_status != 0)
	      && ((io->io_hdr.flags & CTL_FLAG_ABORT) == 0)
	      && ((io->io_hdr.status & CTL_STATUS_MASK) == CTL_STATUS_NONE)) {
		/*
		 * For hardware error sense keys, the sense key
		 * specific value is defined to be a retry count,
		 * but we use it to pass back an internal FETD
		 * error code.  XXX KDM  Hopefully the FETD is only
		 * using 16 bits for an error code, since that's
		 * all the space we have in the sks field.
		 */
		ctl_set_internal_failure(&io->scsiio,
					 /*sks_valid*/ 1,
					 /*retry_count*/
					 io->io_hdr.port_status);
	}

	/*
	 * If this is a read, or a write with errors, it is done.
	 */
	if ((beio->bio_cmd == BIO_READ)
	 || ((io->io_hdr.flags & CTL_FLAG_ABORT) != 0)
	 || ((io->io_hdr.status & CTL_STATUS_MASK) != CTL_STATUS_NONE)) {
		ctl_complete_beio(beio);
		return (0);
	}

	/*
	 * At this point, we have a write and the DMA completed
	 * successfully.  We now have to queue it to the task queue to
	 * execute the backend I/O.  That is because we do blocking
	 * memory allocations, and in the file backing case, blocking I/O.
	 * This move done routine is generally called in the SIM's
	 * interrupt context, and therefore we cannot block.
	 */
	mtx_lock(&be_lun->queue_lock);
	/*
	 * XXX KDM make sure that links is okay to use at this point.
	 * Otherwise, we either need to add another field to ctl_io_hdr,
	 * or deal with resource allocation here.
	 */
	STAILQ_INSERT_TAIL(&be_lun->datamove_queue, &io->io_hdr, links);
	mtx_unlock(&be_lun->queue_lock);

	taskqueue_enqueue(be_lun->io_taskqueue, &be_lun->io_task);

	return (0);
}

static void
ctl_be_block_biodone(struct bio *bio)
{
	struct ctl_be_block_io *beio;
	struct ctl_be_block_lun *be_lun;
	union ctl_io *io;
	int error;

	beio = bio->bio_caller1;
	be_lun = beio->lun;
	io = beio->io;

	DPRINTF("entered\n");

	error = bio->bio_error;
	mtx_lock(&be_lun->io_lock);
	if (error != 0)
		beio->num_errors++;

	beio->num_bios_done++;

	/*
	 * XXX KDM will this cause WITNESS to complain?  Holding a lock
	 * during the free might cause it to complain.
	 */
	g_destroy_bio(bio);

	/*
	 * If the send complete bit isn't set, or we aren't the last I/O to
	 * complete, then we're done.
	 */
	if ((beio->send_complete == 0)
	 || (beio->num_bios_done < beio->num_bios_sent)) {
		mtx_unlock(&be_lun->io_lock);
		return;
	}

	/*
	 * At this point, we've verified that we are the last I/O to
	 * complete, so it's safe to drop the lock.
	 */
	devstat_end_transaction(beio->lun->disk_stats, beio->io_len,
	    beio->ds_tag_type, beio->ds_trans_type,
	    /*now*/ NULL, /*then*/&beio->ds_t0);
	mtx_unlock(&be_lun->io_lock);

	/*
	 * If there are any errors from the backing device, we fail the
	 * entire I/O with a medium error.
	 */
	if (beio->num_errors > 0) {
		if (error == EOPNOTSUPP) {
			ctl_set_invalid_opcode(&io->scsiio);
		} else if (error == ENOSPC) {
			ctl_set_space_alloc_fail(&io->scsiio);
		} else if (beio->bio_cmd == BIO_FLUSH) {
			/* XXX KDM is there is a better error here? */
			ctl_set_internal_failure(&io->scsiio,
						 /*sks_valid*/ 1,
						 /*retry_count*/ 0xbad2);
		} else
			ctl_set_medium_error(&io->scsiio);
		ctl_complete_beio(beio);
		return;
	}

	/*
	 * If this is a write, a flush, a delete or verify, we're all done.
	 * If this is a read, we can now send the data to the user.
	 */
	if ((beio->bio_cmd == BIO_WRITE)
	 || (beio->bio_cmd == BIO_FLUSH)
	 || (beio->bio_cmd == BIO_DELETE)
	 || (ARGS(io)->flags & CTL_LLF_VERIFY)) {
		ctl_set_success(&io->scsiio);
		ctl_complete_beio(beio);
	} else {
#ifdef CTL_TIME_IO
        	getbintime(&io->io_hdr.dma_start_bt);
#endif  
		ctl_datamove(io);
	}
}

static void
ctl_be_block_flush_file(struct ctl_be_block_lun *be_lun,
			struct ctl_be_block_io *beio)
{
	union ctl_io *io = beio->io;
	struct mount *mountpoint;
	int vfs_is_locked, error, lock_flags;

	DPRINTF("entered\n");

	binuptime(&beio->ds_t0);
	mtx_lock(&be_lun->io_lock);
	devstat_start_transaction(beio->lun->disk_stats, &beio->ds_t0);
	mtx_unlock(&be_lun->io_lock);

	vfs_is_locked = VFS_LOCK_GIANT(be_lun->vn->v_mount);

	(void) vn_start_write(be_lun->vn, &mountpoint, V_WAIT);

	if (MNT_SHARED_WRITES(mountpoint)
	 || ((mountpoint == NULL)
	  && MNT_SHARED_WRITES(be_lun->vn->v_mount)))
		lock_flags = LK_SHARED;
	else
		lock_flags = LK_EXCLUSIVE;

	vn_lock(be_lun->vn, lock_flags | LK_RETRY);

	error = VOP_FSYNC(be_lun->vn, MNT_WAIT, curthread);
	VOP_UNLOCK(be_lun->vn, 0);

	vn_finished_write(mountpoint);

	VFS_UNLOCK_GIANT(vfs_is_locked);
	mtx_lock(&be_lun->io_lock);
	devstat_end_transaction(beio->lun->disk_stats, beio->io_len,
	    beio->ds_tag_type, beio->ds_trans_type,
	    /*now*/ NULL, /*then*/&beio->ds_t0);
	mtx_unlock(&be_lun->io_lock);

	if (error == 0)
		ctl_set_success(&io->scsiio);
	else {
		/* XXX KDM is there is a better error here? */
		ctl_set_internal_failure(&io->scsiio,
					 /*sks_valid*/ 1,
					 /*retry_count*/ 0xbad1);
	}

	ctl_complete_beio(beio);
}

SDT_PROBE_DEFINE1(cbb, kernel, read, file_start, "uint64_t");
SDT_PROBE_DEFINE1(cbb, kernel, write, file_start, "uint64_t");
SDT_PROBE_DEFINE1(cbb, kernel, read, file_done,"uint64_t");
SDT_PROBE_DEFINE1(cbb, kernel, write, file_done, "uint64_t");

static void
ctl_be_block_dispatch_file(struct ctl_be_block_lun *be_lun,
			   struct ctl_be_block_io *beio)
{
	struct ctl_be_block_filedata *file_data;
	union ctl_io *io;
	struct uio xuio;
	struct iovec *xiovec;
	int vfs_is_locked, flags;
	int error, i;

	DPRINTF("entered\n");

	file_data = &be_lun->backend.file;
	io = beio->io;
	flags = 0;
	if (ARGS(io)->flags & CTL_LLF_DPO)
		flags |= IO_DIRECT;
	if (beio->bio_cmd == BIO_WRITE && ARGS(io)->flags & CTL_LLF_FUA)
		flags |= IO_SYNC;

	bzero(&xuio, sizeof(xuio));
	if (beio->bio_cmd == BIO_READ) {
		SDT_PROBE(cbb, kernel, read, file_start, 0, 0, 0, 0, 0);
		xuio.uio_rw = UIO_READ;
	} else {
		SDT_PROBE(cbb, kernel, write, file_start, 0, 0, 0, 0, 0);
		xuio.uio_rw = UIO_WRITE;
	}
	xuio.uio_offset = beio->io_offset;
	xuio.uio_resid = beio->io_len;
	xuio.uio_segflg = UIO_SYSSPACE;
	xuio.uio_iov = beio->xiovecs;
	xuio.uio_iovcnt = beio->num_segs;
	xuio.uio_td = curthread;

	for (i = 0, xiovec = xuio.uio_iov; i < xuio.uio_iovcnt; i++, xiovec++) {
		xiovec->iov_base = beio->sg_segs[i].addr;
		xiovec->iov_len = beio->sg_segs[i].len;
	}

	vfs_is_locked = VFS_LOCK_GIANT(be_lun->vn->v_mount);
	binuptime(&beio->ds_t0);
	mtx_lock(&be_lun->io_lock);
	devstat_start_transaction(beio->lun->disk_stats, &beio->ds_t0);
	mtx_unlock(&be_lun->io_lock);

	if (beio->bio_cmd == BIO_READ) {
		vn_lock(be_lun->vn, LK_SHARED | LK_RETRY);

		/*
		 * UFS pays attention to IO_DIRECT for reads.  If the
		 * DIRECTIO option is configured into the kernel, it calls
		 * ffs_rawread().  But that only works for single-segment
		 * uios with user space addresses.  In our case, with a
		 * kernel uio, it still reads into the buffer cache, but it
		 * will just try to release the buffer from the cache later
		 * on in ffs_read().
		 *
		 * ZFS does not pay attention to IO_DIRECT for reads.
		 *
		 * UFS does not pay attention to IO_SYNC for reads.
		 *
		 * ZFS pays attention to IO_SYNC (which translates into the
		 * Solaris define FRSYNC for zfs_read()) for reads.  It
		 * attempts to sync the file before reading.
		 *
		 * So, to attempt to provide some barrier semantics in the
		 * BIO_ORDERED case, set both IO_DIRECT and IO_SYNC.
		 */
		error = VOP_READ(be_lun->vn, &xuio, flags, file_data->cred);

		VOP_UNLOCK(be_lun->vn, 0);
		SDT_PROBE(cbb, kernel, read, file_done, 0, 0, 0, 0, 0);
	} else {
		struct mount *mountpoint;
		int lock_flags;

		(void)vn_start_write(be_lun->vn, &mountpoint, V_WAIT);

		if (MNT_SHARED_WRITES(mountpoint)
		 || ((mountpoint == NULL)
		  && MNT_SHARED_WRITES(be_lun->vn->v_mount)))
			lock_flags = LK_SHARED;
		else
			lock_flags = LK_EXCLUSIVE;

		vn_lock(be_lun->vn, lock_flags | LK_RETRY);

		/*
		 * UFS pays attention to IO_DIRECT for writes.  The write
		 * is done asynchronously.  (Normally the write would just
		 * get put into cache.
		 *
		 * UFS pays attention to IO_SYNC for writes.  It will
		 * attempt to write the buffer out synchronously if that
		 * flag is set.
		 *
		 * ZFS does not pay attention to IO_DIRECT for writes.
		 *
		 * ZFS pays attention to IO_SYNC (a.k.a. FSYNC or FRSYNC)
		 * for writes.  It will flush the transaction from the
		 * cache before returning.
		 *
		 * So if we've got the BIO_ORDERED flag set, we want
		 * IO_SYNC in either the UFS or ZFS case.
		 */
		error = VOP_WRITE(be_lun->vn, &xuio, flags, file_data->cred);
		VOP_UNLOCK(be_lun->vn, 0);

		vn_finished_write(mountpoint);
		SDT_PROBE(cbb, kernel, write, file_done, 0, 0, 0, 0, 0);
        }
        VFS_UNLOCK_GIANT(vfs_is_locked);

	mtx_lock(&be_lun->io_lock);
	devstat_end_transaction(beio->lun->disk_stats, beio->io_len,
	    beio->ds_tag_type, beio->ds_trans_type,
	    /*now*/ NULL, /*then*/&beio->ds_t0);
	mtx_unlock(&be_lun->io_lock);

	/*
	 * If we got an error, set the sense data to "MEDIUM ERROR" and
	 * return the I/O to the user.
	 */
	if (error != 0) {
		char path_str[32];

		ctl_scsi_path_string(io, path_str, sizeof(path_str));
		printf("%s%s command returned errno %d\n", path_str,
		       (beio->bio_cmd == BIO_READ) ? "READ" : "WRITE", error);
		if (error == ENOSPC) {
			ctl_set_space_alloc_fail(&io->scsiio);
		} else
			ctl_set_medium_error(&io->scsiio);
		ctl_complete_beio(beio);
		return;
	}

	/*
	 * If this is a write or a verify, we're all done.
	 * If this is a read, we can now send the data to the user.
	 */
	if ((beio->bio_cmd == BIO_WRITE) ||
	    (ARGS(io)->flags & CTL_LLF_VERIFY)) {
		ctl_set_success(&io->scsiio);
		ctl_complete_beio(beio);
	} else {
#ifdef CTL_TIME_IO
        	getbintime(&io->io_hdr.dma_start_bt);
#endif  
		ctl_datamove(io);
	}
}

static void
ctl_be_block_dispatch_zvol(struct ctl_be_block_lun *be_lun,
			   struct ctl_be_block_io *beio)
{
	struct ctl_be_block_devdata *dev_data;
	union ctl_io *io;
	struct uio xuio;
	struct iovec *xiovec;
	int flags;
	int error, i;

	DPRINTF("entered\n");

	dev_data = &be_lun->backend.dev;
	io = beio->io;
	flags = 0;
	if (ARGS(io)->flags & CTL_LLF_DPO)
		flags |= IO_DIRECT;
	if (beio->bio_cmd == BIO_WRITE && ARGS(io)->flags & CTL_LLF_FUA)
		flags |= IO_SYNC;

	bzero(&xuio, sizeof(xuio));
	if (beio->bio_cmd == BIO_READ) {
		SDT_PROBE(cbb, kernel, read, file_start, 0, 0, 0, 0, 0);
		xuio.uio_rw = UIO_READ;
	} else {
		SDT_PROBE(cbb, kernel, write, file_start, 0, 0, 0, 0, 0);
		xuio.uio_rw = UIO_WRITE;
	}
	xuio.uio_offset = beio->io_offset;
	xuio.uio_resid = beio->io_len;
	xuio.uio_segflg = UIO_SYSSPACE;
	xuio.uio_iov = beio->xiovecs;
	xuio.uio_iovcnt = beio->num_segs;
	xuio.uio_td = curthread;

	for (i = 0, xiovec = xuio.uio_iov; i < xuio.uio_iovcnt; i++, xiovec++) {
		xiovec->iov_base = beio->sg_segs[i].addr;
		xiovec->iov_len = beio->sg_segs[i].len;
	}

	binuptime(&beio->ds_t0);
	mtx_lock(&be_lun->io_lock);
	devstat_start_transaction(beio->lun->disk_stats, &beio->ds_t0);
	mtx_unlock(&be_lun->io_lock);

	if (beio->bio_cmd == BIO_READ) {
		error = (*dev_data->csw->d_read)(dev_data->cdev, &xuio, flags);
		SDT_PROBE(cbb, kernel, read, file_done, 0, 0, 0, 0, 0);
	} else {
		error = (*dev_data->csw->d_write)(dev_data->cdev, &xuio, flags);
		SDT_PROBE(cbb, kernel, write, file_done, 0, 0, 0, 0, 0);
	}

	mtx_lock(&be_lun->io_lock);
	devstat_end_transaction(beio->lun->disk_stats, beio->io_len,
	    beio->ds_tag_type, beio->ds_trans_type,
	    /*now*/ NULL, /*then*/&beio->ds_t0);
	mtx_unlock(&be_lun->io_lock);

	/*
	 * If we got an error, set the sense data to "MEDIUM ERROR" and
	 * return the I/O to the user.
	 */
	if (error != 0) {
		if (error == ENOSPC) {
			ctl_set_space_alloc_fail(&io->scsiio);
		} else
			ctl_set_medium_error(&io->scsiio);
		ctl_complete_beio(beio);
		return;
	}

	/*
	 * If this is a write or a verify, we're all done.
	 * If this is a read, we can now send the data to the user.
	 */
	if ((beio->bio_cmd == BIO_WRITE) ||
	    (ARGS(io)->flags & CTL_LLF_VERIFY)) {
		ctl_set_success(&io->scsiio);
		ctl_complete_beio(beio);
	} else {
#ifdef CTL_TIME_IO
        	getbintime(&io->io_hdr.dma_start_bt);
#endif  
		ctl_datamove(io);
	}
}

static void
ctl_be_block_flush_dev(struct ctl_be_block_lun *be_lun,
		       struct ctl_be_block_io *beio)
{
	struct bio *bio;
	union ctl_io *io;
	struct ctl_be_block_devdata *dev_data;

	dev_data = &be_lun->backend.dev;
	io = beio->io;

	DPRINTF("entered\n");

	/* This can't fail, it's a blocking allocation. */
	bio = g_alloc_bio();

	bio->bio_cmd	    = BIO_FLUSH;
	bio->bio_flags	   |= BIO_ORDERED;
	bio->bio_dev	    = dev_data->cdev;
	bio->bio_offset	    = 0;
	bio->bio_data	    = 0;
	bio->bio_done	    = ctl_be_block_biodone;
	bio->bio_caller1    = beio;
	bio->bio_pblkno	    = 0;

	/*
	 * We don't need to acquire the LUN lock here, because we are only
	 * sending one bio, and so there is no other context to synchronize
	 * with.
	 */
	beio->num_bios_sent = 1;
	beio->send_complete = 1;

	binuptime(&beio->ds_t0);
	mtx_lock(&be_lun->io_lock);
	devstat_start_transaction(be_lun->disk_stats, &beio->ds_t0);
	mtx_unlock(&be_lun->io_lock);

	(*dev_data->csw->d_strategy)(bio);
}

static void
ctl_be_block_unmap_dev_range(struct ctl_be_block_lun *be_lun,
		       struct ctl_be_block_io *beio,
		       uint64_t off, uint64_t len, int last)
{
	struct bio *bio;
	struct ctl_be_block_devdata *dev_data;
	uint64_t maxlen;

	dev_data = &be_lun->backend.dev;
	maxlen = LONG_MAX - (LONG_MAX % be_lun->blocksize);
	while (len > 0) {
		bio = g_alloc_bio();
		bio->bio_cmd	    = BIO_DELETE;
		bio->bio_dev	    = dev_data->cdev;
		bio->bio_offset	    = off;
		bio->bio_length	    = MIN(len, maxlen);
		bio->bio_data	    = 0;
		bio->bio_done	    = ctl_be_block_biodone;
		bio->bio_caller1    = beio;
		bio->bio_pblkno     = off / be_lun->blocksize;

		off += bio->bio_length;
		len -= bio->bio_length;

		mtx_lock(&be_lun->io_lock);
		beio->num_bios_sent++;
		if (last && len == 0)
			beio->send_complete = 1;
		mtx_unlock(&be_lun->io_lock);

		(*dev_data->csw->d_strategy)(bio);
	}
}

static void
ctl_be_block_unmap_dev(struct ctl_be_block_lun *be_lun,
		       struct ctl_be_block_io *beio)
{
	union ctl_io *io;
	struct ctl_be_block_devdata *dev_data;
	struct ctl_ptr_len_flags *ptrlen;
	struct scsi_unmap_desc *buf, *end;
	uint64_t len;

	dev_data = &be_lun->backend.dev;
	io = beio->io;

	DPRINTF("entered\n");

	binuptime(&beio->ds_t0);
	mtx_lock(&be_lun->io_lock);
	devstat_start_transaction(be_lun->disk_stats, &beio->ds_t0);
	mtx_unlock(&be_lun->io_lock);

	if (beio->io_offset == -1) {
		beio->io_len = 0;
		ptrlen = (struct ctl_ptr_len_flags *)&io->io_hdr.ctl_private[CTL_PRIV_LBA_LEN];
		buf = (struct scsi_unmap_desc *)ptrlen->ptr;
		end = buf + ptrlen->len / sizeof(*buf);
		for (; buf < end; buf++) {
			len = (uint64_t)scsi_4btoul(buf->length) *
			    be_lun->blocksize;
			beio->io_len += len;
			ctl_be_block_unmap_dev_range(be_lun, beio,
			    scsi_8btou64(buf->lba) * be_lun->blocksize, len,
			    (end - buf < 2) ? TRUE : FALSE);
		}
	} else
		ctl_be_block_unmap_dev_range(be_lun, beio,
		    beio->io_offset, beio->io_len, TRUE);
}

static void
ctl_be_block_dispatch_dev(struct ctl_be_block_lun *be_lun,
			  struct ctl_be_block_io *beio)
{
	TAILQ_HEAD(, bio) queue = TAILQ_HEAD_INITIALIZER(queue);
	int i;
	struct bio *bio;
	struct ctl_be_block_devdata *dev_data;
	off_t cur_offset;
	int max_iosize;

	DPRINTF("entered\n");

	dev_data = &be_lun->backend.dev;

	/*
	 * We have to limit our I/O size to the maximum supported by the
	 * backend device.  Hopefully it is MAXPHYS.  If the driver doesn't
	 * set it properly, use DFLTPHYS.
	 */
	max_iosize = dev_data->cdev->si_iosize_max;
	if (max_iosize < PAGE_SIZE)
		max_iosize = DFLTPHYS;

	cur_offset = beio->io_offset;
	for (i = 0; i < beio->num_segs; i++) {
		size_t cur_size;
		uint8_t *cur_ptr;

		cur_size = beio->sg_segs[i].len;
		cur_ptr = beio->sg_segs[i].addr;

		while (cur_size > 0) {
			/* This can't fail, it's a blocking allocation. */
			bio = g_alloc_bio();

			KASSERT(bio != NULL, ("g_alloc_bio() failed!\n"));

			bio->bio_cmd = beio->bio_cmd;
			bio->bio_dev = dev_data->cdev;
			bio->bio_caller1 = beio;
			bio->bio_length = min(cur_size, max_iosize);
			bio->bio_offset = cur_offset;
			bio->bio_data = cur_ptr;
			bio->bio_done = ctl_be_block_biodone;
			bio->bio_pblkno = cur_offset / be_lun->blocksize;

			cur_offset += bio->bio_length;
			cur_ptr += bio->bio_length;
			cur_size -= bio->bio_length;

			TAILQ_INSERT_TAIL(&queue, bio, bio_queue);
			beio->num_bios_sent++;
		}
	}
	binuptime(&beio->ds_t0);
	mtx_lock(&be_lun->io_lock);
	devstat_start_transaction(be_lun->disk_stats, &beio->ds_t0);
	beio->send_complete = 1;
	mtx_unlock(&be_lun->io_lock);

	/*
	 * Fire off all allocated requests!
	 */
	while ((bio = TAILQ_FIRST(&queue)) != NULL) {
		TAILQ_REMOVE(&queue, bio, bio_queue);
		(*dev_data->csw->d_strategy)(bio);
	}
}

static void
ctl_be_block_cw_done_ws(struct ctl_be_block_io *beio)
{
	union ctl_io *io;

	io = beio->io;
	ctl_free_beio(beio);
	if ((io->io_hdr.flags & CTL_FLAG_ABORT) ||
	    ((io->io_hdr.status & CTL_STATUS_MASK) != CTL_STATUS_NONE &&
	     (io->io_hdr.status & CTL_STATUS_MASK) != CTL_SUCCESS)) {
		ctl_config_write_done(io);
		return;
	}

	ctl_be_block_config_write(io);
}

static void
ctl_be_block_cw_dispatch_ws(struct ctl_be_block_lun *be_lun,
			    union ctl_io *io)
{
	struct ctl_be_block_io *beio;
	struct ctl_be_block_softc *softc;
	struct ctl_lba_len_flags *lbalen;
	uint64_t len_left, lba;
	int i, seglen;
	uint8_t *buf, *end;

	DPRINTF("entered\n");

	beio = (struct ctl_be_block_io *)PRIV(io)->ptr;
	softc = be_lun->softc;
	lbalen = ARGS(beio->io);

	if (lbalen->flags & ~(SWS_LBDATA | SWS_UNMAP | SWS_ANCHOR | SWS_NDOB) ||
	    (lbalen->flags & (SWS_UNMAP | SWS_ANCHOR) && be_lun->unmap == NULL)) {
		ctl_free_beio(beio);
		ctl_set_invalid_field(&io->scsiio,
				      /*sks_valid*/ 1,
				      /*command*/ 1,
				      /*field*/ 1,
				      /*bit_valid*/ 0,
				      /*bit*/ 0);
		ctl_config_write_done(io);
		return;
	}

	switch (io->scsiio.tag_type) {
	case CTL_TAG_ORDERED:
		beio->ds_tag_type = DEVSTAT_TAG_ORDERED;
		break;
	case CTL_TAG_HEAD_OF_QUEUE:
		beio->ds_tag_type = DEVSTAT_TAG_HEAD;
		break;
	case CTL_TAG_UNTAGGED:
	case CTL_TAG_SIMPLE:
	case CTL_TAG_ACA:
	default:
		beio->ds_tag_type = DEVSTAT_TAG_SIMPLE;
		break;
	}

	if (lbalen->flags & (SWS_UNMAP | SWS_ANCHOR)) {
		beio->io_offset = lbalen->lba * be_lun->blocksize;
		beio->io_len = (uint64_t)lbalen->len * be_lun->blocksize;
		beio->bio_cmd = BIO_DELETE;
		beio->ds_trans_type = DEVSTAT_FREE;

		be_lun->unmap(be_lun, beio);
		return;
	}

	beio->bio_cmd = BIO_WRITE;
	beio->ds_trans_type = DEVSTAT_WRITE;

	DPRINTF("WRITE SAME at LBA %jx len %u\n",
	       (uintmax_t)lbalen->lba, lbalen->len);

	len_left = (uint64_t)lbalen->len * be_lun->blocksize;
	for (i = 0, lba = 0; i < CTLBLK_MAX_SEGS && len_left > 0; i++) {

		/*
		 * Setup the S/G entry for this chunk.
		 */
		seglen = MIN(CTLBLK_MAX_SEG, len_left);
		seglen -= seglen % be_lun->blocksize;
		beio->sg_segs[i].len = seglen;
		beio->sg_segs[i].addr = uma_zalloc(be_lun->lun_zone, M_WAITOK);

		DPRINTF("segment %d addr %p len %zd\n", i,
			beio->sg_segs[i].addr, beio->sg_segs[i].len);

		beio->num_segs++;
		len_left -= seglen;

		buf = beio->sg_segs[i].addr;
		end = buf + seglen;
		for (; buf < end; buf += be_lun->blocksize) {
			memcpy(buf, io->scsiio.kern_data_ptr, be_lun->blocksize);
			if (lbalen->flags & SWS_LBDATA)
				scsi_ulto4b(lbalen->lba + lba, buf);
			lba++;
		}
	}

	beio->io_offset = lbalen->lba * be_lun->blocksize;
	beio->io_len = lba * be_lun->blocksize;

	/* We can not do all in one run. Correct and schedule rerun. */
	if (len_left > 0) {
		lbalen->lba += lba;
		lbalen->len -= lba;
		beio->beio_cont = ctl_be_block_cw_done_ws;
	}

	be_lun->dispatch(be_lun, beio);
}

static void
ctl_be_block_cw_dispatch_unmap(struct ctl_be_block_lun *be_lun,
			    union ctl_io *io)
{
	struct ctl_be_block_io *beio;
	struct ctl_be_block_softc *softc;
	struct ctl_ptr_len_flags *ptrlen;

	DPRINTF("entered\n");

	beio = (struct ctl_be_block_io *)PRIV(io)->ptr;
	softc = be_lun->softc;
	ptrlen = (struct ctl_ptr_len_flags *)&io->io_hdr.ctl_private[CTL_PRIV_LBA_LEN];

	if ((ptrlen->flags & ~SU_ANCHOR) != 0 || be_lun->unmap == NULL) {
		ctl_free_beio(beio);
		ctl_set_invalid_field(&io->scsiio,
				      /*sks_valid*/ 0,
				      /*command*/ 1,
				      /*field*/ 0,
				      /*bit_valid*/ 0,
				      /*bit*/ 0);
		ctl_config_write_done(io);
		return;
	}

	switch (io->scsiio.tag_type) {
	case CTL_TAG_ORDERED:
		beio->ds_tag_type = DEVSTAT_TAG_ORDERED;
		break;
	case CTL_TAG_HEAD_OF_QUEUE:
		beio->ds_tag_type = DEVSTAT_TAG_HEAD;
		break;
	case CTL_TAG_UNTAGGED:
	case CTL_TAG_SIMPLE:
	case CTL_TAG_ACA:
	default:
		beio->ds_tag_type = DEVSTAT_TAG_SIMPLE;
		break;
	}

	beio->io_len = 0;
	beio->io_offset = -1;

	beio->bio_cmd = BIO_DELETE;
	beio->ds_trans_type = DEVSTAT_FREE;

	DPRINTF("UNMAP\n");

	be_lun->unmap(be_lun, beio);
}

static void
ctl_be_block_cw_done(struct ctl_be_block_io *beio)
{
	union ctl_io *io;

	io = beio->io;
	ctl_free_beio(beio);
	ctl_config_write_done(io);
}

static void
ctl_be_block_cw_dispatch(struct ctl_be_block_lun *be_lun,
			 union ctl_io *io)
{
	struct ctl_be_block_io *beio;
	struct ctl_be_block_softc *softc;

	DPRINTF("entered\n");

	softc = be_lun->softc;
	beio = ctl_alloc_beio(softc);
	beio->io = io;
	beio->lun = be_lun;
	beio->beio_cont = ctl_be_block_cw_done;
	PRIV(io)->ptr = (void *)beio;

	switch (io->scsiio.cdb[0]) {
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
		beio->bio_cmd = BIO_FLUSH;
		beio->ds_trans_type = DEVSTAT_NO_DATA;
		beio->ds_tag_type = DEVSTAT_TAG_ORDERED;
		beio->io_len = 0;
		be_lun->lun_flush(be_lun, beio);
		break;
	case WRITE_SAME_10:
	case WRITE_SAME_16:
		ctl_be_block_cw_dispatch_ws(be_lun, io);
		break;
	case UNMAP:
		ctl_be_block_cw_dispatch_unmap(be_lun, io);
		break;
	default:
		panic("Unhandled CDB type %#x", io->scsiio.cdb[0]);
		break;
	}
}

SDT_PROBE_DEFINE1(cbb, kernel, read, start, "uint64_t");
SDT_PROBE_DEFINE1(cbb, kernel, write, start, "uint64_t");
SDT_PROBE_DEFINE1(cbb, kernel, read, alloc_done, "uint64_t");
SDT_PROBE_DEFINE1(cbb, kernel, write, alloc_done, "uint64_t");

static void
ctl_be_block_next(struct ctl_be_block_io *beio)
{
	struct ctl_be_block_lun *be_lun;
	union ctl_io *io;

	io = beio->io;
	be_lun = beio->lun;
	ctl_free_beio(beio);
	if ((io->io_hdr.flags & CTL_FLAG_ABORT) ||
	    ((io->io_hdr.status & CTL_STATUS_MASK) != CTL_STATUS_NONE &&
	     (io->io_hdr.status & CTL_STATUS_MASK) != CTL_SUCCESS)) {
		ctl_data_submit_done(io);
		return;
	}

	io->io_hdr.status &= ~CTL_STATUS_MASK;
	io->io_hdr.status |= CTL_STATUS_NONE;

	mtx_lock(&be_lun->queue_lock);
	/*
	 * XXX KDM make sure that links is okay to use at this point.
	 * Otherwise, we either need to add another field to ctl_io_hdr,
	 * or deal with resource allocation here.
	 */
	STAILQ_INSERT_TAIL(&be_lun->input_queue, &io->io_hdr, links);
	mtx_unlock(&be_lun->queue_lock);

	taskqueue_enqueue(be_lun->io_taskqueue, &be_lun->io_task);
}

static void
ctl_be_block_dispatch(struct ctl_be_block_lun *be_lun,
			   union ctl_io *io)
{
	struct ctl_be_block_io *beio;
	struct ctl_be_block_softc *softc;
	struct ctl_lba_len_flags *lbalen;
	struct ctl_ptr_len_flags *bptrlen;
	uint64_t len_left, lbas;
	int i;

	softc = be_lun->softc;

	DPRINTF("entered\n");

	lbalen = ARGS(io);
	if (lbalen->flags & CTL_LLF_WRITE) {
		SDT_PROBE(cbb, kernel, write, start, 0, 0, 0, 0, 0);
	} else {
		SDT_PROBE(cbb, kernel, read, start, 0, 0, 0, 0, 0);
	}

	beio = ctl_alloc_beio(softc);
	beio->io = io;
	beio->lun = be_lun;
	bptrlen = PRIV(io);
	bptrlen->ptr = (void *)beio;

	switch (io->scsiio.tag_type) {
	case CTL_TAG_ORDERED:
		beio->ds_tag_type = DEVSTAT_TAG_ORDERED;
		break;
	case CTL_TAG_HEAD_OF_QUEUE:
		beio->ds_tag_type = DEVSTAT_TAG_HEAD;
		break;
	case CTL_TAG_UNTAGGED:
	case CTL_TAG_SIMPLE:
	case CTL_TAG_ACA:
	default:
		beio->ds_tag_type = DEVSTAT_TAG_SIMPLE;
		break;
	}

	if (lbalen->flags & CTL_LLF_WRITE) {
		beio->bio_cmd = BIO_WRITE;
		beio->ds_trans_type = DEVSTAT_WRITE;
	} else {
		beio->bio_cmd = BIO_READ;
		beio->ds_trans_type = DEVSTAT_READ;
	}

	DPRINTF("%s at LBA %jx len %u @%ju\n",
	       (beio->bio_cmd == BIO_READ) ? "READ" : "WRITE",
	       (uintmax_t)lbalen->lba, lbalen->len, bptrlen->len);
	if (lbalen->flags & CTL_LLF_COMPARE)
		lbas = CTLBLK_HALF_IO_SIZE;
	else
		lbas = CTLBLK_MAX_IO_SIZE;
	lbas = MIN(lbalen->len - bptrlen->len, lbas / be_lun->blocksize);
	beio->io_offset = (lbalen->lba + bptrlen->len) * be_lun->blocksize;
	beio->io_len = lbas * be_lun->blocksize;
	bptrlen->len += lbas;

	for (i = 0, len_left = beio->io_len; len_left > 0; i++) {
		KASSERT(i < CTLBLK_MAX_SEGS, ("Too many segs (%d >= %d)",
		    i, CTLBLK_MAX_SEGS));

		/*
		 * Setup the S/G entry for this chunk.
		 */
		beio->sg_segs[i].len = min(CTLBLK_MAX_SEG, len_left);
		beio->sg_segs[i].addr = uma_zalloc(be_lun->lun_zone, M_WAITOK);

		DPRINTF("segment %d addr %p len %zd\n", i,
			beio->sg_segs[i].addr, beio->sg_segs[i].len);

		/* Set up second segment for compare operation. */
		if (lbalen->flags & CTL_LLF_COMPARE) {
			beio->sg_segs[i + CTLBLK_HALF_SEGS].len =
			    beio->sg_segs[i].len;
			beio->sg_segs[i + CTLBLK_HALF_SEGS].addr =
			    uma_zalloc(be_lun->lun_zone, M_WAITOK);
		}

		beio->num_segs++;
		len_left -= beio->sg_segs[i].len;
	}
	if (bptrlen->len < lbalen->len)
		beio->beio_cont = ctl_be_block_next;
	io->scsiio.be_move_done = ctl_be_block_move_done;
	/* For compare we have separate S/G lists for read and datamove. */
	if (lbalen->flags & CTL_LLF_COMPARE)
		io->scsiio.kern_data_ptr = (uint8_t *)&beio->sg_segs[CTLBLK_HALF_SEGS];
	else
		io->scsiio.kern_data_ptr = (uint8_t *)beio->sg_segs;
	io->scsiio.kern_data_len = beio->io_len;
	io->scsiio.kern_data_resid = 0;
	io->scsiio.kern_sg_entries = beio->num_segs;
	io->io_hdr.flags |= CTL_FLAG_ALLOCATED | CTL_FLAG_KDPTR_SGLIST;

	/*
	 * For the read case, we need to read the data into our buffers and
	 * then we can send it back to the user.  For the write case, we
	 * need to get the data from the user first.
	 */
	if (beio->bio_cmd == BIO_READ) {
		SDT_PROBE(cbb, kernel, read, alloc_done, 0, 0, 0, 0, 0);
		be_lun->dispatch(be_lun, beio);
	} else {
		SDT_PROBE(cbb, kernel, write, alloc_done, 0, 0, 0, 0, 0);
#ifdef CTL_TIME_IO
        	getbintime(&io->io_hdr.dma_start_bt);
#endif  
		ctl_datamove(io);
	}
}

static void
ctl_be_block_worker(void *context, int pending)
{
	struct ctl_be_block_lun *be_lun;
	struct ctl_be_block_softc *softc;
	union ctl_io *io;

	be_lun = (struct ctl_be_block_lun *)context;
	softc = be_lun->softc;

	DPRINTF("entered\n");

	mtx_lock(&be_lun->queue_lock);
	for (;;) {
		io = (union ctl_io *)STAILQ_FIRST(&be_lun->datamove_queue);
		if (io != NULL) {
			struct ctl_be_block_io *beio;

			DPRINTF("datamove queue\n");

			STAILQ_REMOVE(&be_lun->datamove_queue, &io->io_hdr,
				      ctl_io_hdr, links);

			mtx_unlock(&be_lun->queue_lock);

			beio = (struct ctl_be_block_io *)PRIV(io)->ptr;

			be_lun->dispatch(be_lun, beio);

			mtx_lock(&be_lun->queue_lock);
			continue;
		}
		io = (union ctl_io *)STAILQ_FIRST(&be_lun->config_write_queue);
		if (io != NULL) {

			DPRINTF("config write queue\n");

			STAILQ_REMOVE(&be_lun->config_write_queue, &io->io_hdr,
				      ctl_io_hdr, links);

			mtx_unlock(&be_lun->queue_lock);

			ctl_be_block_cw_dispatch(be_lun, io);

			mtx_lock(&be_lun->queue_lock);
			continue;
		}
		io = (union ctl_io *)STAILQ_FIRST(&be_lun->input_queue);
		if (io != NULL) {
			DPRINTF("input queue\n");

			STAILQ_REMOVE(&be_lun->input_queue, &io->io_hdr,
				      ctl_io_hdr, links);
			mtx_unlock(&be_lun->queue_lock);

			/*
			 * We must drop the lock, since this routine and
			 * its children may sleep.
			 */
			ctl_be_block_dispatch(be_lun, io);

			mtx_lock(&be_lun->queue_lock);
			continue;
		}

		/*
		 * If we get here, there is no work left in the queues, so
		 * just break out and let the task queue go to sleep.
		 */
		break;
	}
	mtx_unlock(&be_lun->queue_lock);
}

/*
 * Entry point from CTL to the backend for I/O.  We queue everything to a
 * work thread, so this just puts the I/O on a queue and wakes up the
 * thread.
 */
static int
ctl_be_block_submit(union ctl_io *io)
{
	struct ctl_be_block_lun *be_lun;
	struct ctl_be_lun *ctl_be_lun;

	DPRINTF("entered\n");

	ctl_be_lun = (struct ctl_be_lun *)io->io_hdr.ctl_private[
		CTL_PRIV_BACKEND_LUN].ptr;
	be_lun = (struct ctl_be_block_lun *)ctl_be_lun->be_lun;

	/*
	 * Make sure we only get SCSI I/O.
	 */
	KASSERT(io->io_hdr.io_type == CTL_IO_SCSI, ("Non-SCSI I/O (type "
		"%#x) encountered", io->io_hdr.io_type));

	PRIV(io)->len = 0;

	mtx_lock(&be_lun->queue_lock);
	/*
	 * XXX KDM make sure that links is okay to use at this point.
	 * Otherwise, we either need to add another field to ctl_io_hdr,
	 * or deal with resource allocation here.
	 */
	STAILQ_INSERT_TAIL(&be_lun->input_queue, &io->io_hdr, links);
	mtx_unlock(&be_lun->queue_lock);
	taskqueue_enqueue(be_lun->io_taskqueue, &be_lun->io_task);

	return (CTL_RETVAL_COMPLETE);
}

static int
ctl_be_block_ioctl(struct cdev *dev, u_long cmd, caddr_t addr,
			int flag, struct thread *td)
{
	struct ctl_be_block_softc *softc;
	int error;

	softc = &backend_block_softc;

	error = 0;

	switch (cmd) {
	case CTL_LUN_REQ: {
		struct ctl_lun_req *lun_req;

		lun_req = (struct ctl_lun_req *)addr;

		switch (lun_req->reqtype) {
		case CTL_LUNREQ_CREATE:
			error = ctl_be_block_create(softc, lun_req);
			break;
		case CTL_LUNREQ_RM:
			error = ctl_be_block_rm(softc, lun_req);
			break;
		case CTL_LUNREQ_MODIFY:
			error = ctl_be_block_modify(softc, lun_req);
			break;
		default:
			lun_req->status = CTL_LUN_ERROR;
			snprintf(lun_req->error_str, sizeof(lun_req->error_str),
				 "invalid LUN request type %d",
				 lun_req->reqtype);
			break;
		}
		break;
	}
	default:
		error = ENOTTY;
		break;
	}

	return (error);
}

static int
ctl_be_block_open_file(struct ctl_be_block_lun *be_lun, struct ctl_lun_req *req)
{
	struct ctl_be_block_filedata *file_data;
	struct ctl_lun_create_params *params;
	struct vattr		      vattr;
	off_t			      pss;
	int			      error;

	error = 0;
	file_data = &be_lun->backend.file;
	params = &be_lun->params;

	be_lun->dev_type = CTL_BE_BLOCK_FILE;
	be_lun->dispatch = ctl_be_block_dispatch_file;
	be_lun->lun_flush = ctl_be_block_flush_file;

	error = VOP_GETATTR(be_lun->vn, &vattr, curthread->td_ucred);
	if (error != 0) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "error calling VOP_GETATTR() for file %s",
			 be_lun->dev_path);
		return (error);
	}

	/*
	 * Verify that we have the ability to upgrade to exclusive
	 * access on this file so we can trap errors at open instead
	 * of reporting them during first access.
	 */
	if (VOP_ISLOCKED(be_lun->vn) != LK_EXCLUSIVE) {
		vn_lock(be_lun->vn, LK_UPGRADE | LK_RETRY);
		if (be_lun->vn->v_iflag & VI_DOOMED) {
			error = EBADF;
			snprintf(req->error_str, sizeof(req->error_str),
				 "error locking file %s", be_lun->dev_path);
			return (error);
		}
	}


	file_data->cred = crhold(curthread->td_ucred);
	if (params->lun_size_bytes != 0)
		be_lun->size_bytes = params->lun_size_bytes;
	else
		be_lun->size_bytes = vattr.va_size;
	/*
	 * We set the multi thread flag for file operations because all
	 * filesystems (in theory) are capable of allowing multiple readers
	 * of a file at once.  So we want to get the maximum possible
	 * concurrency.
	 */
	be_lun->flags |= CTL_BE_BLOCK_LUN_MULTI_THREAD;

	/*
	 * For files we can use any logical block size.  Prefer 512 bytes
	 * for compatibility reasons.  If file's vattr.va_blocksize
	 * (preferred I/O block size) is bigger and multiple to chosen
	 * logical block size -- report it as physical block size.
	 */
	if (params->blocksize_bytes != 0)
		be_lun->blocksize = params->blocksize_bytes;
	else
		be_lun->blocksize = 512;
	pss = vattr.va_blocksize / be_lun->blocksize;
	if ((pss > 0) && (pss * be_lun->blocksize == vattr.va_blocksize) &&
	    ((pss & (pss - 1)) == 0)) {
		be_lun->pblockexp = fls(pss) - 1;
		be_lun->pblockoff = 0;
	}

	/*
	 * Sanity check.  The media size has to be at least one
	 * sector long.
	 */
	if (be_lun->size_bytes < be_lun->blocksize) {
		error = EINVAL;
		snprintf(req->error_str, sizeof(req->error_str),
			 "file %s size %ju < block size %u", be_lun->dev_path,
			 (uintmax_t)be_lun->size_bytes, be_lun->blocksize);
	}
	return (error);
}

static int
ctl_be_block_open_dev(struct ctl_be_block_lun *be_lun, struct ctl_lun_req *req)
{
	struct ctl_lun_create_params *params;
	struct vattr		      vattr;
	struct cdev		     *dev;
	struct cdevsw		     *devsw;
	int			      error;
	off_t			      ps, pss, po, pos;

	params = &be_lun->params;

	be_lun->dev_type = CTL_BE_BLOCK_DEV;
	be_lun->backend.dev.cdev = be_lun->vn->v_rdev;
	be_lun->backend.dev.csw = dev_refthread(be_lun->backend.dev.cdev,
					     &be_lun->backend.dev.dev_ref);
	if (be_lun->backend.dev.csw == NULL)
		panic("Unable to retrieve device switch");
	if (strcmp(be_lun->backend.dev.csw->d_name, "zvol") == 0)
		be_lun->dispatch = ctl_be_block_dispatch_zvol;
	else
		be_lun->dispatch = ctl_be_block_dispatch_dev;
	be_lun->lun_flush = ctl_be_block_flush_dev;
	be_lun->unmap = ctl_be_block_unmap_dev;

	error = VOP_GETATTR(be_lun->vn, &vattr, NOCRED);
	if (error) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "error getting vnode attributes for device %s",
			 be_lun->dev_path);
		return (error);
	}

	dev = be_lun->vn->v_rdev;
	devsw = dev->si_devsw;
	if (!devsw->d_ioctl) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "no d_ioctl for device %s!",
			 be_lun->dev_path);
		return (ENODEV);
	}

	error = devsw->d_ioctl(dev, DIOCGSECTORSIZE,
			       (caddr_t)&be_lun->blocksize, FREAD,
			       curthread);
	if (error) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "error %d returned for DIOCGSECTORSIZE ioctl "
			 "on %s!", error, be_lun->dev_path);
		return (error);
	}

	/*
	 * If the user has asked for a blocksize that is greater than the
	 * backing device's blocksize, we can do it only if the blocksize
	 * the user is asking for is an even multiple of the underlying 
	 * device's blocksize.
	 */
	if ((params->blocksize_bytes != 0)
	 && (params->blocksize_bytes > be_lun->blocksize)) {
		uint32_t bs_multiple, tmp_blocksize;

		bs_multiple = params->blocksize_bytes / be_lun->blocksize;

		tmp_blocksize = bs_multiple * be_lun->blocksize;

		if (tmp_blocksize == params->blocksize_bytes) {
			be_lun->blocksize = params->blocksize_bytes;
		} else {
			snprintf(req->error_str, sizeof(req->error_str),
				 "requested blocksize %u is not an even "
				 "multiple of backing device blocksize %u",
				 params->blocksize_bytes,
				 be_lun->blocksize);
			return (EINVAL);
			
		}
	} else if ((params->blocksize_bytes != 0)
		&& (params->blocksize_bytes != be_lun->blocksize)) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "requested blocksize %u < backing device "
			 "blocksize %u", params->blocksize_bytes,
			 be_lun->blocksize);
		return (EINVAL);
	}

	error = devsw->d_ioctl(dev, DIOCGMEDIASIZE,
			       (caddr_t)&be_lun->size_bytes, FREAD,
			       curthread);
	if (error) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "error %d returned for DIOCGMEDIASIZE "
			 " ioctl on %s!", error,
			 be_lun->dev_path);
		return (error);
	}

	if (params->lun_size_bytes != 0) {
		if (params->lun_size_bytes > be_lun->size_bytes) {
			snprintf(req->error_str, sizeof(req->error_str),
				 "requested LUN size %ju > backing device "
				 "size %ju",
				 (uintmax_t)params->lun_size_bytes,
				 (uintmax_t)be_lun->size_bytes);
			return (EINVAL);
		}

		be_lun->size_bytes = params->lun_size_bytes;
	}

	error = devsw->d_ioctl(dev, DIOCGSTRIPESIZE,
			       (caddr_t)&ps, FREAD, curthread);
	if (error)
		ps = po = 0;
	else {
		error = devsw->d_ioctl(dev, DIOCGSTRIPEOFFSET,
				       (caddr_t)&po, FREAD, curthread);
		if (error)
			po = 0;
	}
	pss = ps / be_lun->blocksize;
	pos = po / be_lun->blocksize;
	if ((pss > 0) && (pss * be_lun->blocksize == ps) && (pss >= pos) &&
	    ((pss & (pss - 1)) == 0) && (pos * be_lun->blocksize == po)) {
		be_lun->pblockexp = fls(pss) - 1;
		be_lun->pblockoff = (pss - pos) % pss;
	}

	return (0);
}

static int
ctl_be_block_close(struct ctl_be_block_lun *be_lun)
{
	DROP_GIANT();
	if (be_lun->vn) {
		int flags = FREAD | FWRITE;
		int vfs_is_locked = 0;

		switch (be_lun->dev_type) {
		case CTL_BE_BLOCK_DEV:
			if (be_lun->backend.dev.csw) {
				dev_relthread(be_lun->backend.dev.cdev,
					      be_lun->backend.dev.dev_ref);
				be_lun->backend.dev.csw  = NULL;
				be_lun->backend.dev.cdev = NULL;
			}
			break;
		case CTL_BE_BLOCK_FILE:
			vfs_is_locked = VFS_LOCK_GIANT(be_lun->vn->v_mount);
			break;
		case CTL_BE_BLOCK_NONE:
			break;
		default:
			panic("Unexpected backend type.");
			break;
		}

		(void)vn_close(be_lun->vn, flags, NOCRED, curthread);
		be_lun->vn = NULL;

		switch (be_lun->dev_type) {
		case CTL_BE_BLOCK_DEV:
			break;
		case CTL_BE_BLOCK_FILE:
			VFS_UNLOCK_GIANT(vfs_is_locked);
			if (be_lun->backend.file.cred != NULL) {
				crfree(be_lun->backend.file.cred);
				be_lun->backend.file.cred = NULL;
			}
			break;
		case CTL_BE_BLOCK_NONE:
			break;
		default:
			panic("Unexpected backend type.");
			break;
		}
		be_lun->dev_type = CTL_BE_BLOCK_NONE;
	}
	PICKUP_GIANT();

	return (0);
}

static int
ctl_be_block_open(struct ctl_be_block_softc *softc,
		       struct ctl_be_block_lun *be_lun, struct ctl_lun_req *req)
{
	struct nameidata nd;
	int		 flags;
	int		 error;
	int		 vfs_is_locked;

	/*
	 * XXX KDM allow a read-only option?
	 */
	flags = FREAD | FWRITE;
	error = 0;

	if (rootvnode == NULL) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "Root filesystem is not mounted");
		return (1);
	}

	if (!curthread->td_proc->p_fd->fd_cdir) {
		curthread->td_proc->p_fd->fd_cdir = rootvnode;
		VREF(rootvnode);
	}
	if (!curthread->td_proc->p_fd->fd_rdir) {
		curthread->td_proc->p_fd->fd_rdir = rootvnode;
		VREF(rootvnode);
	}
	if (!curthread->td_proc->p_fd->fd_jdir) {
		curthread->td_proc->p_fd->fd_jdir = rootvnode;
		VREF(rootvnode);
	}

 again:
	NDINIT(&nd, LOOKUP, FOLLOW, UIO_SYSSPACE, be_lun->dev_path, curthread);
	error = vn_open(&nd, &flags, 0, NULL);
	if (error) {
		/*
		 * This is the only reasonable guess we can make as far as
		 * path if the user doesn't give us a fully qualified path.
		 * If they want to specify a file, they need to specify the
		 * full path.
		 */
		if (be_lun->dev_path[0] != '/') {
			char *dev_path = "/dev/";
			char *dev_name;

			/* Try adding device path at beginning of name */
			dev_name = malloc(strlen(be_lun->dev_path)
					+ strlen(dev_path) + 1,
					  M_CTLBLK, M_WAITOK);
			if (dev_name) {
				sprintf(dev_name, "%s%s", dev_path,
					be_lun->dev_path);
				free(be_lun->dev_path, M_CTLBLK);
				be_lun->dev_path = dev_name;
				goto again;
			}
		}
		snprintf(req->error_str, sizeof(req->error_str),
		    "error opening %s: %d", be_lun->dev_path, error);
		return (error);
	}

	vfs_is_locked = NDHASGIANT(&nd);

	NDFREE(&nd, NDF_ONLY_PNBUF);
		
	be_lun->vn = nd.ni_vp;

	/* We only support disks and files. */
	if (vn_isdisk(be_lun->vn, &error)) {
		error = ctl_be_block_open_dev(be_lun, req);
	} else if (be_lun->vn->v_type == VREG) {
		error = ctl_be_block_open_file(be_lun, req);
	} else {
		error = EINVAL;
		snprintf(req->error_str, sizeof(req->error_str),
			 "%s is not a disk or plain file", be_lun->dev_path);
	}
	VOP_UNLOCK(be_lun->vn, 0);
	VFS_UNLOCK_GIANT(vfs_is_locked);

	if (error != 0) {
		ctl_be_block_close(be_lun);
		return (error);
	}

	be_lun->blocksize_shift = fls(be_lun->blocksize) - 1;
	be_lun->size_blocks = be_lun->size_bytes >> be_lun->blocksize_shift;

	return (0);
}

static int
ctl_be_block_create(struct ctl_be_block_softc *softc, struct ctl_lun_req *req)
{
	struct ctl_be_block_lun *be_lun;
	struct ctl_lun_create_params *params;
	char num_thread_str[16];
	char tmpstr[32];
	char *value;
	int retval, num_threads, unmap;
	int tmp_num_threads;

	params = &req->reqdata.create;
	retval = 0;
	req->status = CTL_LUN_OK;

	num_threads = cbb_num_threads;

	be_lun = malloc(sizeof(*be_lun), M_CTLBLK, M_ZERO | M_WAITOK);

	be_lun->params = req->reqdata.create;
	be_lun->softc = softc;
	STAILQ_INIT(&be_lun->input_queue);
	STAILQ_INIT(&be_lun->config_write_queue);
	STAILQ_INIT(&be_lun->datamove_queue);
	sprintf(be_lun->lunname, "cblk%d", softc->num_luns);
	mtx_init(&be_lun->io_lock, "cblk io lock", NULL, MTX_DEF);
	mtx_init(&be_lun->queue_lock, "cblk queue lock", NULL, MTX_DEF);
	ctl_init_opts(&be_lun->ctl_be_lun.options,
	    req->num_be_args, req->kern_be_args);

	be_lun->lun_zone = uma_zcreate(be_lun->lunname, CTLBLK_MAX_SEG,
	    NULL, NULL, NULL, NULL, /*align*/ 0, /*flags*/0);

	if (be_lun->lun_zone == NULL) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "error allocating UMA zone");
		goto bailout_error;
	}

	if (params->flags & CTL_LUN_FLAG_DEV_TYPE)
		be_lun->ctl_be_lun.lun_type = params->device_type;
	else
		be_lun->ctl_be_lun.lun_type = T_DIRECT;

	if (be_lun->ctl_be_lun.lun_type == T_DIRECT) {
		value = ctl_get_opt(&be_lun->ctl_be_lun.options, "file");
		if (value == NULL) {
			snprintf(req->error_str, sizeof(req->error_str),
				 "no file argument specified");
			goto bailout_error;
		}
		be_lun->dev_path = strdup(value, M_CTLBLK);
		be_lun->blocksize = 512;
		be_lun->blocksize_shift = fls(be_lun->blocksize) - 1;

		retval = ctl_be_block_open(softc, be_lun, req);
		if (retval != 0) {
			retval = 0;
			req->status = CTL_LUN_WARNING;
		}
	} else {
		/*
		 * For processor devices, we don't have any size.
		 */
		be_lun->blocksize = 0;
		be_lun->pblockexp = 0;
		be_lun->pblockoff = 0;
		be_lun->size_blocks = 0;
		be_lun->size_bytes = 0;
		be_lun->ctl_be_lun.maxlba = 0;

		/*
		 * Default to just 1 thread for processor devices.
		 */
		num_threads = 1;
	}

	/*
	 * XXX This searching loop might be refactored to be combined with
	 * the loop above,
	 */
	value = ctl_get_opt(&be_lun->ctl_be_lun.options, "num_threads");
	if (value != NULL) {
		tmp_num_threads = strtol(value, NULL, 0);

		/*
		 * We don't let the user specify less than one
		 * thread, but hope he's clueful enough not to
		 * specify 1000 threads.
		 */
		if (tmp_num_threads < 1) {
			snprintf(req->error_str, sizeof(req->error_str),
				 "invalid number of threads %s",
				 num_thread_str);
			goto bailout_error;
		}
		num_threads = tmp_num_threads;
	}
	unmap = 0;
	value = ctl_get_opt(&be_lun->ctl_be_lun.options, "unmap");
	if (value != NULL && strcmp(value, "on") == 0)
		unmap = 1;

	be_lun->flags = CTL_BE_BLOCK_LUN_UNCONFIGURED;
	be_lun->ctl_be_lun.flags = CTL_LUN_FLAG_PRIMARY;
	if (be_lun->vn == NULL)
		be_lun->ctl_be_lun.flags |= CTL_LUN_FLAG_OFFLINE;
	if (unmap)
		be_lun->ctl_be_lun.flags |= CTL_LUN_FLAG_UNMAP;
	be_lun->ctl_be_lun.be_lun = be_lun;
	be_lun->ctl_be_lun.maxlba = (be_lun->size_blocks == 0) ?
	    0 : (be_lun->size_blocks - 1);
	be_lun->ctl_be_lun.blocksize = be_lun->blocksize;
	be_lun->ctl_be_lun.pblockexp = be_lun->pblockexp;
	be_lun->ctl_be_lun.pblockoff = be_lun->pblockoff;
	if (be_lun->dispatch == ctl_be_block_dispatch_zvol &&
	    be_lun->blocksize != 0)
		be_lun->ctl_be_lun.atomicblock = CTLBLK_MAX_IO_SIZE /
		    be_lun->blocksize;
	/* Tell the user the blocksize we ended up using */
	params->lun_size_bytes = be_lun->size_bytes;
	params->blocksize_bytes = be_lun->blocksize;
	if (params->flags & CTL_LUN_FLAG_ID_REQ) {
		be_lun->ctl_be_lun.req_lun_id = params->req_lun_id;
		be_lun->ctl_be_lun.flags |= CTL_LUN_FLAG_ID_REQ;
	} else
		be_lun->ctl_be_lun.req_lun_id = 0;

	be_lun->ctl_be_lun.lun_shutdown = ctl_be_block_lun_shutdown;
	be_lun->ctl_be_lun.lun_config_status =
		ctl_be_block_lun_config_status;
	be_lun->ctl_be_lun.be = &ctl_be_block_driver;

	if ((params->flags & CTL_LUN_FLAG_SERIAL_NUM) == 0) {
		snprintf(tmpstr, sizeof(tmpstr), "MYSERIAL%4d",
			 softc->num_luns);
		strncpy((char *)be_lun->ctl_be_lun.serial_num, tmpstr,
			ctl_min(sizeof(be_lun->ctl_be_lun.serial_num),
			sizeof(tmpstr)));

		/* Tell the user what we used for a serial number */
		strncpy((char *)params->serial_num, tmpstr,
			ctl_min(sizeof(params->serial_num), sizeof(tmpstr)));
	} else { 
		strncpy((char *)be_lun->ctl_be_lun.serial_num,
			params->serial_num,
			ctl_min(sizeof(be_lun->ctl_be_lun.serial_num),
			sizeof(params->serial_num)));
	}
	if ((params->flags & CTL_LUN_FLAG_DEVID) == 0) {
		snprintf(tmpstr, sizeof(tmpstr), "MYDEVID%4d", softc->num_luns);
		strncpy((char *)be_lun->ctl_be_lun.device_id, tmpstr,
			ctl_min(sizeof(be_lun->ctl_be_lun.device_id),
			sizeof(tmpstr)));

		/* Tell the user what we used for a device ID */
		strncpy((char *)params->device_id, tmpstr,
			ctl_min(sizeof(params->device_id), sizeof(tmpstr)));
	} else {
		strncpy((char *)be_lun->ctl_be_lun.device_id,
			params->device_id,
			ctl_min(sizeof(be_lun->ctl_be_lun.device_id),
				sizeof(params->device_id)));
	}

	TASK_INIT(&be_lun->io_task, /*priority*/0, ctl_be_block_worker, be_lun);

	be_lun->io_taskqueue = taskqueue_create(be_lun->lunname, M_WAITOK,
	    taskqueue_thread_enqueue, /*context*/&be_lun->io_taskqueue);

	if (be_lun->io_taskqueue == NULL) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "unable to create taskqueue");
		goto bailout_error;
	}

	/*
	 * Note that we start the same number of threads by default for
	 * both the file case and the block device case.  For the file
	 * case, we need multiple threads to allow concurrency, because the
	 * vnode interface is designed to be a blocking interface.  For the
	 * block device case, ZFS zvols at least will block the caller's
	 * context in many instances, and so we need multiple threads to
	 * overcome that problem.  Other block devices don't need as many
	 * threads, but they shouldn't cause too many problems.
	 *
	 * If the user wants to just have a single thread for a block
	 * device, he can specify that when the LUN is created, or change
	 * the tunable/sysctl to alter the default number of threads.
	 */
	retval = taskqueue_start_threads(&be_lun->io_taskqueue,
					 /*num threads*/num_threads,
					 /*priority*/PWAIT,
					 /*thread name*/
					 "%s taskq", be_lun->lunname);

	if (retval != 0)
		goto bailout_error;

	be_lun->num_threads = num_threads;

	mtx_lock(&softc->lock);
	softc->num_luns++;
	STAILQ_INSERT_TAIL(&softc->lun_list, be_lun, links);

	mtx_unlock(&softc->lock);

	retval = ctl_add_lun(&be_lun->ctl_be_lun);
	if (retval != 0) {
		mtx_lock(&softc->lock);
		STAILQ_REMOVE(&softc->lun_list, be_lun, ctl_be_block_lun,
			      links);
		softc->num_luns--;
		mtx_unlock(&softc->lock);
		snprintf(req->error_str, sizeof(req->error_str),
			 "ctl_add_lun() returned error %d, see dmesg for "
			 "details", retval);
		retval = 0;
		goto bailout_error;
	}

	mtx_lock(&softc->lock);

	/*
	 * Tell the config_status routine that we're waiting so it won't
	 * clean up the LUN in the event of an error.
	 */
	be_lun->flags |= CTL_BE_BLOCK_LUN_WAITING;

	while (be_lun->flags & CTL_BE_BLOCK_LUN_UNCONFIGURED) {
		retval = msleep(be_lun, &softc->lock, PCATCH, "ctlblk", 0);
		if (retval == EINTR)
			break;
	}
	be_lun->flags &= ~CTL_BE_BLOCK_LUN_WAITING;

	if (be_lun->flags & CTL_BE_BLOCK_LUN_CONFIG_ERR) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "LUN configuration error, see dmesg for details");
		STAILQ_REMOVE(&softc->lun_list, be_lun, ctl_be_block_lun,
			      links);
		softc->num_luns--;
		mtx_unlock(&softc->lock);
		goto bailout_error;
	} else {
		params->req_lun_id = be_lun->ctl_be_lun.lun_id;
	}

	mtx_unlock(&softc->lock);

	be_lun->disk_stats = devstat_new_entry("cbb", params->req_lun_id,
					       be_lun->blocksize,
					       DEVSTAT_ALL_SUPPORTED,
					       be_lun->ctl_be_lun.lun_type
					       | DEVSTAT_TYPE_IF_OTHER,
					       DEVSTAT_PRIORITY_OTHER);

	return (retval);

bailout_error:
	req->status = CTL_LUN_ERROR;

	if (be_lun->io_taskqueue != NULL)
		taskqueue_free(be_lun->io_taskqueue);
	ctl_be_block_close(be_lun);
	if (be_lun->dev_path != NULL)
		free(be_lun->dev_path, M_CTLBLK);
	if (be_lun->lun_zone != NULL)
		uma_zdestroy(be_lun->lun_zone);
	ctl_free_opts(&be_lun->ctl_be_lun.options);
	mtx_destroy(&be_lun->queue_lock);
	mtx_destroy(&be_lun->io_lock);
	free(be_lun, M_CTLBLK);

	return (retval);
}

static int
ctl_be_block_rm(struct ctl_be_block_softc *softc, struct ctl_lun_req *req)
{
	struct ctl_lun_rm_params *params;
	struct ctl_be_block_lun *be_lun;
	int retval;

	params = &req->reqdata.rm;

	mtx_lock(&softc->lock);

	be_lun = NULL;

	STAILQ_FOREACH(be_lun, &softc->lun_list, links) {
		if (be_lun->ctl_be_lun.lun_id == params->lun_id)
			break;
	}
	mtx_unlock(&softc->lock);

	if (be_lun == NULL) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "LUN %u is not managed by the block backend",
			 params->lun_id);
		goto bailout_error;
	}

	retval = ctl_disable_lun(&be_lun->ctl_be_lun);

	if (retval != 0) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "error %d returned from ctl_disable_lun() for "
			 "LUN %d", retval, params->lun_id);
		goto bailout_error;

	}

	retval = ctl_invalidate_lun(&be_lun->ctl_be_lun);
	if (retval != 0) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "error %d returned from ctl_invalidate_lun() for "
			 "LUN %d", retval, params->lun_id);
		goto bailout_error;
	}

	mtx_lock(&softc->lock);

	be_lun->flags |= CTL_BE_BLOCK_LUN_WAITING;

	while ((be_lun->flags & CTL_BE_BLOCK_LUN_UNCONFIGURED) == 0) {
                retval = msleep(be_lun, &softc->lock, PCATCH, "ctlblk", 0);
                if (retval == EINTR)
                        break;
        }

	be_lun->flags &= ~CTL_BE_BLOCK_LUN_WAITING;

	if ((be_lun->flags & CTL_BE_BLOCK_LUN_UNCONFIGURED) == 0) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "interrupted waiting for LUN to be freed");
		mtx_unlock(&softc->lock);
		goto bailout_error;
	}

	STAILQ_REMOVE(&softc->lun_list, be_lun, ctl_be_block_lun, links);

	softc->num_luns--;
	mtx_unlock(&softc->lock);

	taskqueue_drain(be_lun->io_taskqueue, &be_lun->io_task);

	taskqueue_free(be_lun->io_taskqueue);

	ctl_be_block_close(be_lun);

	if (be_lun->disk_stats != NULL)
		devstat_remove_entry(be_lun->disk_stats);

	uma_zdestroy(be_lun->lun_zone);

	ctl_free_opts(&be_lun->ctl_be_lun.options);
	free(be_lun->dev_path, M_CTLBLK);
	mtx_destroy(&be_lun->queue_lock);
	mtx_destroy(&be_lun->io_lock);
	free(be_lun, M_CTLBLK);

	req->status = CTL_LUN_OK;

	return (0);

bailout_error:

	req->status = CTL_LUN_ERROR;

	return (0);
}

static int
ctl_be_block_modify_file(struct ctl_be_block_lun *be_lun,
			 struct ctl_lun_req *req)
{
	struct vattr vattr;
	int vfs_is_locked, error;
	struct ctl_lun_create_params *params = &be_lun->params;

	if (params->lun_size_bytes != 0) {
		be_lun->size_bytes = params->lun_size_bytes;
	} else  {
		vfs_is_locked = VFS_LOCK_GIANT(be_lun->vn->v_mount);
		vn_lock(be_lun->vn, LK_SHARED | LK_RETRY);
		error = VOP_GETATTR(be_lun->vn, &vattr, curthread->td_ucred);
		VOP_UNLOCK(be_lun->vn, 0);
		VFS_UNLOCK_GIANT(vfs_is_locked);
		if (error != 0) {
			snprintf(req->error_str, sizeof(req->error_str),
				 "error calling VOP_GETATTR() for file %s",
				 be_lun->dev_path);
			return (error);
		}

		be_lun->size_bytes = vattr.va_size;
	}

	return (0);
}

static int
ctl_be_block_modify_dev(struct ctl_be_block_lun *be_lun,
			struct ctl_lun_req *req)
{
	struct ctl_be_block_devdata *dev_data;
	int error;
	struct ctl_lun_create_params *params = &be_lun->params;
	uint64_t size_bytes;

	dev_data = &be_lun->backend.dev;
	if (!dev_data->csw->d_ioctl) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "no d_ioctl for device %s!", be_lun->dev_path);
		return (ENODEV);
	}

	error = dev_data->csw->d_ioctl(dev_data->cdev, DIOCGMEDIASIZE,
			       (caddr_t)&size_bytes, FREAD,
			       curthread);
	if (error) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "error %d returned for DIOCGMEDIASIZE ioctl "
			 "on %s!", error, be_lun->dev_path);
		return (error);
	}

	if (params->lun_size_bytes != 0) {
		if (params->lun_size_bytes > size_bytes) {
			snprintf(req->error_str, sizeof(req->error_str),
				 "requested LUN size %ju > backing device "
				 "size %ju",
				 (uintmax_t)params->lun_size_bytes,
				 (uintmax_t)size_bytes);
			return (EINVAL);
		}

		be_lun->size_bytes = params->lun_size_bytes;
	} else {
		be_lun->size_bytes = size_bytes;
	}

	return (0);
}

static int
ctl_be_block_modify(struct ctl_be_block_softc *softc, struct ctl_lun_req *req)
{
	struct ctl_lun_modify_params *params;
	struct ctl_be_block_lun *be_lun;
	uint64_t oldsize;
	int error;

	params = &req->reqdata.modify;

	mtx_lock(&softc->lock);
	be_lun = NULL;
	STAILQ_FOREACH(be_lun, &softc->lun_list, links) {
		if (be_lun->ctl_be_lun.lun_id == params->lun_id)
			break;
	}
	mtx_unlock(&softc->lock);

	if (be_lun == NULL) {
		snprintf(req->error_str, sizeof(req->error_str),
			 "LUN %u is not managed by the block backend",
			 params->lun_id);
		goto bailout_error;
	}

	be_lun->params.lun_size_bytes = params->lun_size_bytes;

	oldsize = be_lun->size_blocks;
	if (be_lun->vn == NULL)
		error = ctl_be_block_open(softc, be_lun, req);
	else if (be_lun->vn->v_type == VREG)
		error = ctl_be_block_modify_file(be_lun, req);
	else
		error = ctl_be_block_modify_dev(be_lun, req);

	if (error == 0 && be_lun->size_blocks != oldsize) {
		be_lun->size_blocks = be_lun->size_bytes >>
		    be_lun->blocksize_shift;

		/*
		 * The maximum LBA is the size - 1.
		 *
		 * XXX: Note that this field is being updated without locking,
		 * 	which might cause problems on 32-bit architectures.
		 */
		be_lun->ctl_be_lun.maxlba = (be_lun->size_blocks == 0) ?
		    0 : (be_lun->size_blocks - 1);
		be_lun->ctl_be_lun.blocksize = be_lun->blocksize;
		be_lun->ctl_be_lun.pblockexp = be_lun->pblockexp;
		be_lun->ctl_be_lun.pblockoff = be_lun->pblockoff;
		if (be_lun->dispatch == ctl_be_block_dispatch_zvol &&
		    be_lun->blocksize != 0)
			be_lun->ctl_be_lun.atomicblock = CTLBLK_MAX_IO_SIZE /
			    be_lun->blocksize;
		ctl_lun_capacity_changed(&be_lun->ctl_be_lun);
		if (oldsize == 0 && be_lun->size_blocks != 0)
			ctl_lun_online(&be_lun->ctl_be_lun);
	}

	/* Tell the user the exact size we ended up using */
	params->lun_size_bytes = be_lun->size_bytes;

	req->status = error ? CTL_LUN_WARNING : CTL_LUN_OK;

	return (0);

bailout_error:
	req->status = CTL_LUN_ERROR;

	return (0);
}

static void
ctl_be_block_lun_shutdown(void *be_lun)
{
	struct ctl_be_block_lun *lun;
	struct ctl_be_block_softc *softc;

	lun = (struct ctl_be_block_lun *)be_lun;

	softc = lun->softc;

	mtx_lock(&softc->lock);
	lun->flags |= CTL_BE_BLOCK_LUN_UNCONFIGURED;
	if (lun->flags & CTL_BE_BLOCK_LUN_WAITING)
		wakeup(lun);
	mtx_unlock(&softc->lock);

}

static void
ctl_be_block_lun_config_status(void *be_lun, ctl_lun_config_status status)
{
	struct ctl_be_block_lun *lun;
	struct ctl_be_block_softc *softc;

	lun = (struct ctl_be_block_lun *)be_lun;
	softc = lun->softc;

	if (status == CTL_LUN_CONFIG_OK) {
		mtx_lock(&softc->lock);
		lun->flags &= ~CTL_BE_BLOCK_LUN_UNCONFIGURED;
		if (lun->flags & CTL_BE_BLOCK_LUN_WAITING)
			wakeup(lun);
		mtx_unlock(&softc->lock);

		/*
		 * We successfully added the LUN, attempt to enable it.
		 */
		if (ctl_enable_lun(&lun->ctl_be_lun) != 0) {
			printf("%s: ctl_enable_lun() failed!\n", __func__);
			if (ctl_invalidate_lun(&lun->ctl_be_lun) != 0) {
				printf("%s: ctl_invalidate_lun() failed!\n",
				       __func__);
			}
		}

		return;
	}


	mtx_lock(&softc->lock);
	lun->flags &= ~CTL_BE_BLOCK_LUN_UNCONFIGURED;
	lun->flags |= CTL_BE_BLOCK_LUN_CONFIG_ERR;
	wakeup(lun);
	mtx_unlock(&softc->lock);
}


static int
ctl_be_block_config_write(union ctl_io *io)
{
	struct ctl_be_block_lun *be_lun;
	struct ctl_be_lun *ctl_be_lun;
	int retval;

	retval = 0;

	DPRINTF("entered\n");

	ctl_be_lun = (struct ctl_be_lun *)io->io_hdr.ctl_private[
		CTL_PRIV_BACKEND_LUN].ptr;
	be_lun = (struct ctl_be_block_lun *)ctl_be_lun->be_lun;

	switch (io->scsiio.cdb[0]) {
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
	case WRITE_SAME_10:
	case WRITE_SAME_16:
	case UNMAP:
		/*
		 * The upper level CTL code will filter out any CDBs with
		 * the immediate bit set and return the proper error.
		 *
		 * We don't really need to worry about what LBA range the
		 * user asked to be synced out.  When they issue a sync
		 * cache command, we'll sync out the whole thing.
		 */
		mtx_lock(&be_lun->queue_lock);
		STAILQ_INSERT_TAIL(&be_lun->config_write_queue, &io->io_hdr,
				   links);
		mtx_unlock(&be_lun->queue_lock);
		taskqueue_enqueue(be_lun->io_taskqueue, &be_lun->io_task);
		break;
	case START_STOP_UNIT: {
		struct scsi_start_stop_unit *cdb;

		cdb = (struct scsi_start_stop_unit *)io->scsiio.cdb;

		if (cdb->how & SSS_START)
			retval = ctl_start_lun(ctl_be_lun);
		else {
			retval = ctl_stop_lun(ctl_be_lun);
			/*
			 * XXX KDM Copan-specific offline behavior.
			 * Figure out a reasonable way to port this?
			 */
#ifdef NEEDTOPORT
			if ((retval == 0)
			 && (cdb->byte2 & SSS_ONOFFLINE))
				retval = ctl_lun_offline(ctl_be_lun);
#endif
		}

		/*
		 * In general, the above routines should not fail.  They
		 * just set state for the LUN.  So we've got something
		 * pretty wrong here if we can't start or stop the LUN.
		 */
		if (retval != 0) {
			ctl_set_internal_failure(&io->scsiio,
						 /*sks_valid*/ 1,
						 /*retry_count*/ 0xf051);
			retval = CTL_RETVAL_COMPLETE;
		} else {
			ctl_set_success(&io->scsiio);
		}
		ctl_config_write_done(io);
		break;
	}
	default:
		ctl_set_invalid_opcode(&io->scsiio);
		ctl_config_write_done(io);
		retval = CTL_RETVAL_COMPLETE;
		break;
	}

	return (retval);

}

static int
ctl_be_block_config_read(union ctl_io *io)
{
	return (0);
}

static int
ctl_be_block_lun_info(void *be_lun, struct sbuf *sb)
{
	struct ctl_be_block_lun *lun;
	int retval;

	lun = (struct ctl_be_block_lun *)be_lun;
	retval = 0;

	retval = sbuf_printf(sb, "\t<num_threads>");

	if (retval != 0)
		goto bailout;

	retval = sbuf_printf(sb, "%d", lun->num_threads);

	if (retval != 0)
		goto bailout;

	retval = sbuf_printf(sb, "</num_threads>\n");

bailout:

	return (retval);
}

int
ctl_be_block_init(void)
{
	struct ctl_be_block_softc *softc;
	int retval;

	softc = &backend_block_softc;
	retval = 0;

	mtx_init(&softc->lock, "ctlblock", NULL, MTX_DEF);
	beio_zone = uma_zcreate("beio", sizeof(struct ctl_be_block_io),
	    NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);
	STAILQ_INIT(&softc->disk_list);
	STAILQ_INIT(&softc->lun_list);

	return (retval);
}

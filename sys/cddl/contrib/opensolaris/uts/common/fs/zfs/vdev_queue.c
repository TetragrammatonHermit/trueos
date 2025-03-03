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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Copyright (c) 2012, 2014 by Delphix. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/vdev_impl.h>
#include <sys/spa_impl.h>
#include <sys/zio.h>
#include <sys/avl.h>
#include <sys/dsl_pool.h>

/*
 * ZFS I/O Scheduler
 * ---------------
 *
 * ZFS issues I/O operations to leaf vdevs to satisfy and complete zios.  The
 * I/O scheduler determines when and in what order those operations are
 * issued.  The I/O scheduler divides operations into six I/O classes
 * prioritized in the following order: sync read, sync write, async read,
 * async write, scrub/resilver and trim.  Each queue defines the minimum and
 * maximum number of concurrent operations that may be issued to the device.
 * In addition, the device has an aggregate maximum. Note that the sum of the
 * per-queue minimums must not exceed the aggregate maximum, and if the
 * aggregate maximum is equal to or greater than the sum of the per-queue
 * maximums, the per-queue minimum has no effect.
 *
 * For many physical devices, throughput increases with the number of
 * concurrent operations, but latency typically suffers. Further, physical
 * devices typically have a limit at which more concurrent operations have no
 * effect on throughput or can actually cause it to decrease.
 *
 * The scheduler selects the next operation to issue by first looking for an
 * I/O class whose minimum has not been satisfied. Once all are satisfied and
 * the aggregate maximum has not been hit, the scheduler looks for classes
 * whose maximum has not been satisfied. Iteration through the I/O classes is
 * done in the order specified above. No further operations are issued if the
 * aggregate maximum number of concurrent operations has been hit or if there
 * are no operations queued for an I/O class that has not hit its maximum.
 * Every time an I/O is queued or an operation completes, the I/O scheduler
 * looks for new operations to issue.
 *
 * All I/O classes have a fixed maximum number of outstanding operations
 * except for the async write class. Asynchronous writes represent the data
 * that is committed to stable storage during the syncing stage for
 * transaction groups (see txg.c). Transaction groups enter the syncing state
 * periodically so the number of queued async writes will quickly burst up and
 * then bleed down to zero. Rather than servicing them as quickly as possible,
 * the I/O scheduler changes the maximum number of active async write I/Os
 * according to the amount of dirty data in the pool (see dsl_pool.c). Since
 * both throughput and latency typically increase with the number of
 * concurrent operations issued to physical devices, reducing the burstiness
 * in the number of concurrent operations also stabilizes the response time of
 * operations from other -- and in particular synchronous -- queues. In broad
 * strokes, the I/O scheduler will issue more concurrent operations from the
 * async write queue as there's more dirty data in the pool.
 *
 * Async Writes
 *
 * The number of concurrent operations issued for the async write I/O class
 * follows a piece-wise linear function defined by a few adjustable points.
 *
 *        |                   o---------| <-- zfs_vdev_async_write_max_active
 *   ^    |                  /^         |
 *   |    |                 / |         |
 * active |                /  |         |
 *  I/O   |               /   |         |
 * count  |              /    |         |
 *        |             /     |         |
 *        |------------o      |         | <-- zfs_vdev_async_write_min_active
 *       0|____________^______|_________|
 *        0%           |      |       100% of zfs_dirty_data_max
 *                     |      |
 *                     |      `-- zfs_vdev_async_write_active_max_dirty_percent
 *                     `--------- zfs_vdev_async_write_active_min_dirty_percent
 *
 * Until the amount of dirty data exceeds a minimum percentage of the dirty
 * data allowed in the pool, the I/O scheduler will limit the number of
 * concurrent operations to the minimum. As that threshold is crossed, the
 * number of concurrent operations issued increases linearly to the maximum at
 * the specified maximum percentage of the dirty data allowed in the pool.
 *
 * Ideally, the amount of dirty data on a busy pool will stay in the sloped
 * part of the function between zfs_vdev_async_write_active_min_dirty_percent
 * and zfs_vdev_async_write_active_max_dirty_percent. If it exceeds the
 * maximum percentage, this indicates that the rate of incoming data is
 * greater than the rate that the backend storage can handle. In this case, we
 * must further throttle incoming writes (see dmu_tx_delay() for details).
 */

/*
 * The maximum number of I/Os active to each device.  Ideally, this will be >=
 * the sum of each queue's max_active.  It must be at least the sum of each
 * queue's min_active.
 */
uint32_t zfs_vdev_max_active = 1000;

/*
 * Per-queue limits on the number of I/Os active to each device.  If the
 * sum of the queue's max_active is < zfs_vdev_max_active, then the
 * min_active comes into play.  We will send min_active from each queue,
 * and then select from queues in the order defined by zio_priority_t.
 *
 * In general, smaller max_active's will lead to lower latency of synchronous
 * operations.  Larger max_active's may lead to higher overall throughput,
 * depending on underlying storage.
 *
 * The ratio of the queues' max_actives determines the balance of performance
 * between reads, writes, and scrubs.  E.g., increasing
 * zfs_vdev_scrub_max_active will cause the scrub or resilver to complete
 * more quickly, but reads and writes to have higher latency and lower
 * throughput.
 */
uint32_t zfs_vdev_sync_read_min_active = 10;
uint32_t zfs_vdev_sync_read_max_active = 10;
uint32_t zfs_vdev_sync_write_min_active = 10;
uint32_t zfs_vdev_sync_write_max_active = 10;
uint32_t zfs_vdev_async_read_min_active = 1;
uint32_t zfs_vdev_async_read_max_active = 3;
uint32_t zfs_vdev_async_write_min_active = 1;
uint32_t zfs_vdev_async_write_max_active = 10;
uint32_t zfs_vdev_scrub_min_active = 1;
uint32_t zfs_vdev_scrub_max_active = 2;
uint32_t zfs_vdev_trim_min_active = 1;
/*
 * TRIM max active is large in comparison to the other values due to the fact
 * that TRIM IOs are coalesced at the device layer. This value is set such
 * that a typical SSD can process the queued IOs in a single request.
 */
uint32_t zfs_vdev_trim_max_active = 64;


/*
 * When the pool has less than zfs_vdev_async_write_active_min_dirty_percent
 * dirty data, use zfs_vdev_async_write_min_active.  When it has more than
 * zfs_vdev_async_write_active_max_dirty_percent, use
 * zfs_vdev_async_write_max_active. The value is linearly interpolated
 * between min and max.
 */
int zfs_vdev_async_write_active_min_dirty_percent = 30;
int zfs_vdev_async_write_active_max_dirty_percent = 60;

/*
 * To reduce IOPs, we aggregate small adjacent I/Os into one large I/O.
 * For read I/Os, we also aggregate across small adjacency gaps; for writes
 * we include spans of optional I/Os to aid aggregation at the disk even when
 * they aren't able to help us aggregate at this level.
 */
int zfs_vdev_aggregation_limit = SPA_MAXBLOCKSIZE;
int zfs_vdev_read_gap_limit = 32 << 10;
int zfs_vdev_write_gap_limit = 4 << 10;

#ifdef __FreeBSD__
SYSCTL_DECL(_vfs_zfs_vdev);

TUNABLE_INT("vfs.zfs.vdev.async_write_active_min_dirty_percent",
    &zfs_vdev_async_write_active_min_dirty_percent);
static int sysctl_zfs_async_write_active_min_dirty_percent(SYSCTL_HANDLER_ARGS);
SYSCTL_PROC(_vfs_zfs_vdev, OID_AUTO, async_write_active_min_dirty_percent,
    CTLTYPE_UINT | CTLFLAG_MPSAFE | CTLFLAG_RWTUN, 0, sizeof(int),
    sysctl_zfs_async_write_active_min_dirty_percent, "I",
    "Percentage of async write dirty data below which "
    "async_write_min_active is used.");

TUNABLE_INT("vfs.zfs.vdev.async_write_active_max_dirty_percent",
    &zfs_vdev_async_write_active_max_dirty_percent);
static int sysctl_zfs_async_write_active_max_dirty_percent(SYSCTL_HANDLER_ARGS);
SYSCTL_PROC(_vfs_zfs_vdev, OID_AUTO, async_write_active_max_dirty_percent,
    CTLTYPE_UINT | CTLFLAG_MPSAFE | CTLFLAG_RWTUN, 0, sizeof(int),
    sysctl_zfs_async_write_active_max_dirty_percent, "I",
    "Percentage of async write dirty data above which "
    "async_write_max_active is used.");

TUNABLE_INT("vfs.zfs.vdev.max_active", &zfs_vdev_max_active);
SYSCTL_UINT(_vfs_zfs_vdev, OID_AUTO, max_active, CTLFLAG_RWTUN,
    &zfs_vdev_max_active, 0,
    "The maximum number of I/Os of all types active for each device.");

#define ZFS_VDEV_QUEUE_KNOB_MIN(name)					\
TUNABLE_INT("vfs.zfs.vdev." #name "_min_active",			\
    &zfs_vdev_ ## name ## _min_active);					\
SYSCTL_UINT(_vfs_zfs_vdev, OID_AUTO, name ## _min_active,		\
    CTLFLAG_RWTUN, &zfs_vdev_ ## name ## _min_active, 0,		\
    "Initial number of I/O requests of type " #name			\
    " active for each device");

#define ZFS_VDEV_QUEUE_KNOB_MAX(name)					\
TUNABLE_INT("vfs.zfs.vdev." #name "_max_active",			\
    &zfs_vdev_ ## name ## _max_active);					\
SYSCTL_UINT(_vfs_zfs_vdev, OID_AUTO, name ## _max_active,		\
    CTLFLAG_RWTUN, &zfs_vdev_ ## name ## _max_active, 0,		\
    "Maximum number of I/O requests of type " #name			\
    " active for each device");

ZFS_VDEV_QUEUE_KNOB_MIN(sync_read);
ZFS_VDEV_QUEUE_KNOB_MAX(sync_read);
ZFS_VDEV_QUEUE_KNOB_MIN(sync_write);
ZFS_VDEV_QUEUE_KNOB_MAX(sync_write);
ZFS_VDEV_QUEUE_KNOB_MIN(async_read);
ZFS_VDEV_QUEUE_KNOB_MAX(async_read);
ZFS_VDEV_QUEUE_KNOB_MIN(async_write);
ZFS_VDEV_QUEUE_KNOB_MAX(async_write);
ZFS_VDEV_QUEUE_KNOB_MIN(scrub);
ZFS_VDEV_QUEUE_KNOB_MAX(scrub);
ZFS_VDEV_QUEUE_KNOB_MIN(trim);
ZFS_VDEV_QUEUE_KNOB_MAX(trim);

#undef ZFS_VDEV_QUEUE_KNOB

TUNABLE_INT("vfs.zfs.vdev.aggregation_limit", &zfs_vdev_aggregation_limit);
SYSCTL_INT(_vfs_zfs_vdev, OID_AUTO, aggregation_limit, CTLFLAG_RWTUN,
    &zfs_vdev_aggregation_limit, 0,
    "I/O requests are aggregated up to this size");
TUNABLE_INT("vfs.zfs.vdev.read_gap_limit", &zfs_vdev_read_gap_limit);
SYSCTL_INT(_vfs_zfs_vdev, OID_AUTO, read_gap_limit, CTLFLAG_RWTUN,
    &zfs_vdev_read_gap_limit, 0,
    "Acceptable gap between two reads being aggregated");
TUNABLE_INT("vfs.zfs.vdev.write_gap_limit", &zfs_vdev_write_gap_limit);
SYSCTL_INT(_vfs_zfs_vdev, OID_AUTO, write_gap_limit, CTLFLAG_RWTUN,
    &zfs_vdev_write_gap_limit, 0,
    "Acceptable gap between two writes being aggregated");

static int
sysctl_zfs_async_write_active_min_dirty_percent(SYSCTL_HANDLER_ARGS)
{
	int val, err;

	val = zfs_vdev_async_write_active_min_dirty_percent;
	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);
	
	if (val < 0 || val > 100 ||
	    val >= zfs_vdev_async_write_active_max_dirty_percent)
		return (EINVAL);

	zfs_vdev_async_write_active_min_dirty_percent = val;

	return (0);
}

static int
sysctl_zfs_async_write_active_max_dirty_percent(SYSCTL_HANDLER_ARGS)
{
	int val, err;

	val = zfs_vdev_async_write_active_max_dirty_percent;
	err = sysctl_handle_int(oidp, &val, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);

	if (val < 0 || val > 100 ||
	    val <= zfs_vdev_async_write_active_min_dirty_percent)
		return (EINVAL);

	zfs_vdev_async_write_active_max_dirty_percent = val;

	return (0);
}
#endif

int
vdev_queue_offset_compare(const void *x1, const void *x2)
{
	const zio_t *z1 = x1;
	const zio_t *z2 = x2;

	if (z1->io_offset < z2->io_offset)
		return (-1);
	if (z1->io_offset > z2->io_offset)
		return (1);

	if (z1 < z2)
		return (-1);
	if (z1 > z2)
		return (1);

	return (0);
}

int
vdev_queue_timestamp_compare(const void *x1, const void *x2)
{
	const zio_t *z1 = x1;
	const zio_t *z2 = x2;

	if (z1->io_timestamp < z2->io_timestamp)
		return (-1);
	if (z1->io_timestamp > z2->io_timestamp)
		return (1);

	if (z1 < z2)
		return (-1);
	if (z1 > z2)
		return (1);

	return (0);
}

void
vdev_queue_init(vdev_t *vd)
{
	vdev_queue_t *vq = &vd->vdev_queue;

	mutex_init(&vq->vq_lock, NULL, MUTEX_DEFAULT, NULL);
	vq->vq_vdev = vd;

	avl_create(&vq->vq_active_tree, vdev_queue_offset_compare,
	    sizeof (zio_t), offsetof(struct zio, io_queue_node));

	for (zio_priority_t p = 0; p < ZIO_PRIORITY_NUM_QUEUEABLE; p++) {
		/*
		 * The synchronous i/o queues are FIFO rather than LBA ordered.
		 * This provides more consistent latency for these i/os, and
		 * they tend to not be tightly clustered anyway so there is
		 * little to no throughput loss.
		 */
		boolean_t fifo = (p == ZIO_PRIORITY_SYNC_READ ||
		    p == ZIO_PRIORITY_SYNC_WRITE);
		avl_create(&vq->vq_class[p].vqc_queued_tree,
		    fifo ? vdev_queue_timestamp_compare :
		    vdev_queue_offset_compare,
		    sizeof (zio_t), offsetof(struct zio, io_queue_node));
	}

	vq->vq_lastoffset = 0;
}

void
vdev_queue_fini(vdev_t *vd)
{
	vdev_queue_t *vq = &vd->vdev_queue;

	for (zio_priority_t p = 0; p < ZIO_PRIORITY_NUM_QUEUEABLE; p++)
		avl_destroy(&vq->vq_class[p].vqc_queued_tree);
	avl_destroy(&vq->vq_active_tree);

	mutex_destroy(&vq->vq_lock);
}

static void
vdev_queue_io_add(vdev_queue_t *vq, zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	ASSERT(MUTEX_HELD(&vq->vq_lock));
	ASSERT3U(zio->io_priority, <, ZIO_PRIORITY_NUM_QUEUEABLE);
	avl_add(&vq->vq_class[zio->io_priority].vqc_queued_tree, zio);

#ifdef illumos
	mutex_enter(&spa->spa_iokstat_lock);
	spa->spa_queue_stats[zio->io_priority].spa_queued++;
	if (spa->spa_iokstat != NULL)
		kstat_waitq_enter(spa->spa_iokstat->ks_data);
	mutex_exit(&spa->spa_iokstat_lock);
#endif
}

static void
vdev_queue_io_remove(vdev_queue_t *vq, zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	ASSERT(MUTEX_HELD(&vq->vq_lock));
	ASSERT3U(zio->io_priority, <, ZIO_PRIORITY_NUM_QUEUEABLE);
	avl_remove(&vq->vq_class[zio->io_priority].vqc_queued_tree, zio);

#ifdef illumos
	mutex_enter(&spa->spa_iokstat_lock);
	ASSERT3U(spa->spa_queue_stats[zio->io_priority].spa_queued, >, 0);
	spa->spa_queue_stats[zio->io_priority].spa_queued--;
	if (spa->spa_iokstat != NULL)
		kstat_waitq_exit(spa->spa_iokstat->ks_data);
	mutex_exit(&spa->spa_iokstat_lock);
#endif
}

static void
vdev_queue_pending_add(vdev_queue_t *vq, zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	ASSERT(MUTEX_HELD(&vq->vq_lock));
	ASSERT3U(zio->io_priority, <, ZIO_PRIORITY_NUM_QUEUEABLE);
	vq->vq_class[zio->io_priority].vqc_active++;
	avl_add(&vq->vq_active_tree, zio);

#ifdef illumos
	mutex_enter(&spa->spa_iokstat_lock);
	spa->spa_queue_stats[zio->io_priority].spa_active++;
	if (spa->spa_iokstat != NULL)
		kstat_runq_enter(spa->spa_iokstat->ks_data);
	mutex_exit(&spa->spa_iokstat_lock);
#endif
}

static void
vdev_queue_pending_remove(vdev_queue_t *vq, zio_t *zio)
{
	spa_t *spa = zio->io_spa;
	ASSERT(MUTEX_HELD(&vq->vq_lock));
	ASSERT3U(zio->io_priority, <, ZIO_PRIORITY_NUM_QUEUEABLE);
	vq->vq_class[zio->io_priority].vqc_active--;
	avl_remove(&vq->vq_active_tree, zio);

#ifdef illumos
	mutex_enter(&spa->spa_iokstat_lock);
	ASSERT3U(spa->spa_queue_stats[zio->io_priority].spa_active, >, 0);
	spa->spa_queue_stats[zio->io_priority].spa_active--;
	if (spa->spa_iokstat != NULL) {
		kstat_io_t *ksio = spa->spa_iokstat->ks_data;

		kstat_runq_exit(spa->spa_iokstat->ks_data);
		if (zio->io_type == ZIO_TYPE_READ) {
			ksio->reads++;
			ksio->nread += zio->io_size;
		} else if (zio->io_type == ZIO_TYPE_WRITE) {
			ksio->writes++;
			ksio->nwritten += zio->io_size;
		}
	}
	mutex_exit(&spa->spa_iokstat_lock);
#endif
}

static void
vdev_queue_agg_io_done(zio_t *aio)
{
	if (aio->io_type == ZIO_TYPE_READ) {
		zio_t *pio;
		while ((pio = zio_walk_parents(aio)) != NULL) {
			bcopy((char *)aio->io_data + (pio->io_offset -
			    aio->io_offset), pio->io_data, pio->io_size);
		}
	}

	zio_buf_free(aio->io_data, aio->io_size);
}

static int
vdev_queue_class_min_active(zio_priority_t p)
{
	switch (p) {
	case ZIO_PRIORITY_SYNC_READ:
		return (zfs_vdev_sync_read_min_active);
	case ZIO_PRIORITY_SYNC_WRITE:
		return (zfs_vdev_sync_write_min_active);
	case ZIO_PRIORITY_ASYNC_READ:
		return (zfs_vdev_async_read_min_active);
	case ZIO_PRIORITY_ASYNC_WRITE:
		return (zfs_vdev_async_write_min_active);
	case ZIO_PRIORITY_SCRUB:
		return (zfs_vdev_scrub_min_active);
	case ZIO_PRIORITY_TRIM:
		return (zfs_vdev_trim_min_active);
	default:
		panic("invalid priority %u", p);
		return (0);
	}
}

static int
vdev_queue_max_async_writes(spa_t *spa)
{
	int writes;
	uint64_t dirty = spa->spa_dsl_pool->dp_dirty_total;
	uint64_t min_bytes = zfs_dirty_data_max *
	    zfs_vdev_async_write_active_min_dirty_percent / 100;
	uint64_t max_bytes = zfs_dirty_data_max *
	    zfs_vdev_async_write_active_max_dirty_percent / 100;

	/*
	 * Sync tasks correspond to interactive user actions. To reduce the
	 * execution time of those actions we push data out as fast as possible.
	 */
	if (spa_has_pending_synctask(spa)) {
		return (zfs_vdev_async_write_max_active);
	}

	if (dirty < min_bytes)
		return (zfs_vdev_async_write_min_active);
	if (dirty > max_bytes)
		return (zfs_vdev_async_write_max_active);

	/*
	 * linear interpolation:
	 * slope = (max_writes - min_writes) / (max_bytes - min_bytes)
	 * move right by min_bytes
	 * move up by min_writes
	 */
	writes = (dirty - min_bytes) *
	    (zfs_vdev_async_write_max_active -
	    zfs_vdev_async_write_min_active) /
	    (max_bytes - min_bytes) +
	    zfs_vdev_async_write_min_active;
	ASSERT3U(writes, >=, zfs_vdev_async_write_min_active);
	ASSERT3U(writes, <=, zfs_vdev_async_write_max_active);
	return (writes);
}

static int
vdev_queue_class_max_active(spa_t *spa, zio_priority_t p)
{
	switch (p) {
	case ZIO_PRIORITY_SYNC_READ:
		return (zfs_vdev_sync_read_max_active);
	case ZIO_PRIORITY_SYNC_WRITE:
		return (zfs_vdev_sync_write_max_active);
	case ZIO_PRIORITY_ASYNC_READ:
		return (zfs_vdev_async_read_max_active);
	case ZIO_PRIORITY_ASYNC_WRITE:
		return (vdev_queue_max_async_writes(spa));
	case ZIO_PRIORITY_SCRUB:
		return (zfs_vdev_scrub_max_active);
	case ZIO_PRIORITY_TRIM:
		return (zfs_vdev_trim_max_active);
	default:
		panic("invalid priority %u", p);
		return (0);
	}
}

/*
 * Return the i/o class to issue from, or ZIO_PRIORITY_MAX_QUEUEABLE if
 * there is no eligible class.
 */
static zio_priority_t
vdev_queue_class_to_issue(vdev_queue_t *vq)
{
	spa_t *spa = vq->vq_vdev->vdev_spa;
	zio_priority_t p;

	ASSERT(MUTEX_HELD(&vq->vq_lock));

	if (avl_numnodes(&vq->vq_active_tree) >= zfs_vdev_max_active)
		return (ZIO_PRIORITY_NUM_QUEUEABLE);

	/* find a queue that has not reached its minimum # outstanding i/os */
	for (p = 0; p < ZIO_PRIORITY_NUM_QUEUEABLE; p++) {
		if (avl_numnodes(&vq->vq_class[p].vqc_queued_tree) > 0 &&
		    vq->vq_class[p].vqc_active <
		    vdev_queue_class_min_active(p))
			return (p);
	}

	/*
	 * If we haven't found a queue, look for one that hasn't reached its
	 * maximum # outstanding i/os.
	 */
	for (p = 0; p < ZIO_PRIORITY_NUM_QUEUEABLE; p++) {
		if (avl_numnodes(&vq->vq_class[p].vqc_queued_tree) > 0 &&
		    vq->vq_class[p].vqc_active <
		    vdev_queue_class_max_active(spa, p))
			return (p);
	}

	/* No eligible queued i/os */
	return (ZIO_PRIORITY_NUM_QUEUEABLE);
}

/*
 * Compute the range spanned by two i/os, which is the endpoint of the last
 * (lio->io_offset + lio->io_size) minus start of the first (fio->io_offset).
 * Conveniently, the gap between fio and lio is given by -IO_SPAN(lio, fio);
 * thus fio and lio are adjacent if and only if IO_SPAN(lio, fio) == 0.
 */
#define	IO_SPAN(fio, lio) ((lio)->io_offset + (lio)->io_size - (fio)->io_offset)
#define	IO_GAP(fio, lio) (-IO_SPAN(lio, fio))

static zio_t *
vdev_queue_aggregate(vdev_queue_t *vq, zio_t *zio)
{
	zio_t *first, *last, *aio, *dio, *mandatory, *nio;
	uint64_t maxgap = 0;
	uint64_t size;
	boolean_t stretch;
	avl_tree_t *t;
	enum zio_flag flags;

	ASSERT(MUTEX_HELD(&vq->vq_lock));

	if (zio->io_flags & ZIO_FLAG_DONT_AGGREGATE)
		return (NULL);

	/*
	 * The synchronous i/o queues are not sorted by LBA, so we can't
	 * find adjacent i/os.  These i/os tend to not be tightly clustered,
	 * or too large to aggregate, so this has little impact on performance.
	 */
	if (zio->io_priority == ZIO_PRIORITY_SYNC_READ ||
	    zio->io_priority == ZIO_PRIORITY_SYNC_WRITE)
		return (NULL);

	first = last = zio;

	if (zio->io_type == ZIO_TYPE_READ)
		maxgap = zfs_vdev_read_gap_limit;

	/*
	 * We can aggregate I/Os that are sufficiently adjacent and of
	 * the same flavor, as expressed by the AGG_INHERIT flags.
	 * The latter requirement is necessary so that certain
	 * attributes of the I/O, such as whether it's a normal I/O
	 * or a scrub/resilver, can be preserved in the aggregate.
	 * We can include optional I/Os, but don't allow them
	 * to begin a range as they add no benefit in that situation.
	 */

	/*
	 * We keep track of the last non-optional I/O.
	 */
	mandatory = (first->io_flags & ZIO_FLAG_OPTIONAL) ? NULL : first;

	/*
	 * Walk backwards through sufficiently contiguous I/Os
	 * recording the last non-option I/O.
	 */
	flags = zio->io_flags & ZIO_FLAG_AGG_INHERIT;
	t = &vq->vq_class[zio->io_priority].vqc_queued_tree;
	while ((dio = AVL_PREV(t, first)) != NULL &&
	    (dio->io_flags & ZIO_FLAG_AGG_INHERIT) == flags &&
	    IO_SPAN(dio, last) <= zfs_vdev_aggregation_limit &&
	    IO_GAP(dio, first) <= maxgap) {
		first = dio;
		if (mandatory == NULL && !(first->io_flags & ZIO_FLAG_OPTIONAL))
			mandatory = first;
	}

	/*
	 * Skip any initial optional I/Os.
	 */
	while ((first->io_flags & ZIO_FLAG_OPTIONAL) && first != last) {
		first = AVL_NEXT(t, first);
		ASSERT(first != NULL);
	}

	/*
	 * Walk forward through sufficiently contiguous I/Os.
	 */
	while ((dio = AVL_NEXT(t, last)) != NULL &&
	    (dio->io_flags & ZIO_FLAG_AGG_INHERIT) == flags &&
	    IO_SPAN(first, dio) <= zfs_vdev_aggregation_limit &&
	    IO_GAP(last, dio) <= maxgap) {
		last = dio;
		if (!(last->io_flags & ZIO_FLAG_OPTIONAL))
			mandatory = last;
	}

	/*
	 * Now that we've established the range of the I/O aggregation
	 * we must decide what to do with trailing optional I/Os.
	 * For reads, there's nothing to do. While we are unable to
	 * aggregate further, it's possible that a trailing optional
	 * I/O would allow the underlying device to aggregate with
	 * subsequent I/Os. We must therefore determine if the next
	 * non-optional I/O is close enough to make aggregation
	 * worthwhile.
	 */
	stretch = B_FALSE;
	if (zio->io_type == ZIO_TYPE_WRITE && mandatory != NULL) {
		zio_t *nio = last;
		while ((dio = AVL_NEXT(t, nio)) != NULL &&
		    IO_GAP(nio, dio) == 0 &&
		    IO_GAP(mandatory, dio) <= zfs_vdev_write_gap_limit) {
			nio = dio;
			if (!(nio->io_flags & ZIO_FLAG_OPTIONAL)) {
				stretch = B_TRUE;
				break;
			}
		}
	}

	if (stretch) {
		/* This may be a no-op. */
		dio = AVL_NEXT(t, last);
		dio->io_flags &= ~ZIO_FLAG_OPTIONAL;
	} else {
		while (last != mandatory && last != first) {
			ASSERT(last->io_flags & ZIO_FLAG_OPTIONAL);
			last = AVL_PREV(t, last);
			ASSERT(last != NULL);
		}
	}

	if (first == last)
		return (NULL);

	size = IO_SPAN(first, last);
	ASSERT3U(size, <=, zfs_vdev_aggregation_limit);

	aio = zio_vdev_delegated_io(first->io_vd, first->io_offset,
	    zio_buf_alloc(size), size, first->io_type, zio->io_priority,
	    flags | ZIO_FLAG_DONT_CACHE | ZIO_FLAG_DONT_QUEUE,
	    vdev_queue_agg_io_done, NULL);
	aio->io_timestamp = first->io_timestamp;

	nio = first;
	do {
		dio = nio;
		nio = AVL_NEXT(t, dio);
		ASSERT3U(dio->io_type, ==, aio->io_type);

		if (dio->io_flags & ZIO_FLAG_NODATA) {
			ASSERT3U(dio->io_type, ==, ZIO_TYPE_WRITE);
			bzero((char *)aio->io_data + (dio->io_offset -
			    aio->io_offset), dio->io_size);
		} else if (dio->io_type == ZIO_TYPE_WRITE) {
			bcopy(dio->io_data, (char *)aio->io_data +
			    (dio->io_offset - aio->io_offset),
			    dio->io_size);
		}

		zio_add_child(dio, aio);
		vdev_queue_io_remove(vq, dio);
		zio_vdev_io_bypass(dio);
		zio_execute(dio);
	} while (dio != last);

	return (aio);
}

static zio_t *
vdev_queue_io_to_issue(vdev_queue_t *vq)
{
	zio_t *zio, *aio;
	zio_priority_t p;
	avl_index_t idx;
	vdev_queue_class_t *vqc;
	zio_t search;

again:
	ASSERT(MUTEX_HELD(&vq->vq_lock));

	p = vdev_queue_class_to_issue(vq);

	if (p == ZIO_PRIORITY_NUM_QUEUEABLE) {
		/* No eligible queued i/os */
		return (NULL);
	}

	/*
	 * For LBA-ordered queues (async / scrub), issue the i/o which follows
	 * the most recently issued i/o in LBA (offset) order.
	 *
	 * For FIFO queues (sync), issue the i/o with the lowest timestamp.
	 */
	vqc = &vq->vq_class[p];
	search.io_timestamp = 0;
	search.io_offset = vq->vq_last_offset + 1;
	VERIFY3P(avl_find(&vqc->vqc_queued_tree, &search, &idx), ==, NULL);
	zio = avl_nearest(&vqc->vqc_queued_tree, idx, AVL_AFTER);
	if (zio == NULL)
		zio = avl_first(&vqc->vqc_queued_tree);
	ASSERT3U(zio->io_priority, ==, p);

	aio = vdev_queue_aggregate(vq, zio);
	if (aio != NULL)
		zio = aio;
	else
		vdev_queue_io_remove(vq, zio);

	/*
	 * If the I/O is or was optional and therefore has no data, we need to
	 * simply discard it. We need to drop the vdev queue's lock to avoid a
	 * deadlock that we could encounter since this I/O will complete
	 * immediately.
	 */
	if (zio->io_flags & ZIO_FLAG_NODATA) {
		mutex_exit(&vq->vq_lock);
		zio_vdev_io_bypass(zio);
		zio_execute(zio);
		mutex_enter(&vq->vq_lock);
		goto again;
	}

	vdev_queue_pending_add(vq, zio);
	vq->vq_last_offset = zio->io_offset;

	return (zio);
}

zio_t *
vdev_queue_io(zio_t *zio)
{
	vdev_queue_t *vq = &zio->io_vd->vdev_queue;
	zio_t *nio;

	if (zio->io_flags & ZIO_FLAG_DONT_QUEUE)
		return (zio);

	/*
	 * Children i/os inherent their parent's priority, which might
	 * not match the child's i/o type.  Fix it up here.
	 */
	if (zio->io_type == ZIO_TYPE_READ) {
		if (zio->io_priority != ZIO_PRIORITY_SYNC_READ &&
		    zio->io_priority != ZIO_PRIORITY_ASYNC_READ &&
		    zio->io_priority != ZIO_PRIORITY_SCRUB)
			zio->io_priority = ZIO_PRIORITY_ASYNC_READ;
	} else if (zio->io_type == ZIO_TYPE_WRITE) {
		if (zio->io_priority != ZIO_PRIORITY_SYNC_WRITE &&
		    zio->io_priority != ZIO_PRIORITY_ASYNC_WRITE)
			zio->io_priority = ZIO_PRIORITY_ASYNC_WRITE;
	} else {
		ASSERT(zio->io_type == ZIO_TYPE_FREE);
		zio->io_priority = ZIO_PRIORITY_TRIM;
	}

	zio->io_flags |= ZIO_FLAG_DONT_CACHE | ZIO_FLAG_DONT_QUEUE;

	mutex_enter(&vq->vq_lock);
	zio->io_timestamp = gethrtime();
	vdev_queue_io_add(vq, zio);
	nio = vdev_queue_io_to_issue(vq);
	mutex_exit(&vq->vq_lock);

	if (nio == NULL)
		return (NULL);

	if (nio->io_done == vdev_queue_agg_io_done) {
		zio_nowait(nio);
		return (NULL);
	}

	return (nio);
}

void
vdev_queue_io_done(zio_t *zio)
{
	vdev_queue_t *vq = &zio->io_vd->vdev_queue;
	zio_t *nio;

	if (zio_injection_enabled)
		delay(SEC_TO_TICK(zio_handle_io_delay(zio)));

	mutex_enter(&vq->vq_lock);

	vdev_queue_pending_remove(vq, zio);

	vq->vq_io_complete_ts = gethrtime();

	while ((nio = vdev_queue_io_to_issue(vq)) != NULL) {
		mutex_exit(&vq->vq_lock);
		if (nio->io_done == vdev_queue_agg_io_done) {
			zio_nowait(nio);
		} else {
			zio_vdev_io_reissue(nio);
			zio_execute(nio);
		}
		mutex_enter(&vq->vq_lock);
	}

	mutex_exit(&vq->vq_lock);
}

/*
 * As these three methods are only used for load calculations we're not concerned
 * if we get an incorrect value on 32bit platforms due to lack of vq_lock mutex
 * use here, instead we prefer to keep it lock free for performance.
 */ 
int
vdev_queue_length(vdev_t *vd)
{
	return (avl_numnodes(&vd->vdev_queue.vq_active_tree));
}

uint64_t
vdev_queue_lastoffset(vdev_t *vd)
{
	return (vd->vdev_queue.vq_lastoffset);
}

void
vdev_queue_register_lastoffset(vdev_t *vd, zio_t *zio)
{
	vd->vdev_queue.vq_lastoffset = zio->io_offset + zio->io_size;
}

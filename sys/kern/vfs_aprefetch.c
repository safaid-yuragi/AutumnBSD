/*	$NetBSD$	*/

/*-
 * Copyright (c) 2026 yuragi
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * aprefetch: record the file reads of a boot, and replay them on the
 * next boot as one sweep of asynchronous read-ahead in disk order.
 *
 * Recording.  While a recording window is open, genfs_getpages() calls
 * aprefetch_getpages() for each read of a regular file on FFS.  Each
 * CPU appends access entries to its own ring without locks, with
 * preemption disabled; a read that continues the last one of the same
 * file by the same process extends that entry instead.  When a ring is
 * full, new entries are dropped and counted.  The boot window opens once
 * the root file system is mounted and closes at the first login
 * (APREFIOC_BOOTDONE), on APREFIOC_RECSTOP, or after
 * kern.aprefetch.record_timeout seconds.  aprefd(8) collects the entries
 * with read(2), which returns end-of-file once the window has closed and
 * the rings are empty.
 *
 * Replay.  A kernel thread reads the boot profile, keeps the entries
 * that fit in the budget (in profile order, which is the order in which
 * the data was first read), looks each file up with VFS_FHTOVP (which
 * also rejects a wrong generation number), maps the ranges to disk
 * blocks with VOP_BMAP, sorts them by disk and block number, and issues
 * asynchronous VOP_GETPAGES in that order with at most
 * kern.aprefetch.max_io i/os in flight.  Entries of a file system that
 * is not mounted yet are kept until it is mounted.
 *
 * The pager looks up and busies the pages under the object lock, so a
 * page that a process is reading already is skipped (EBUSY), and a
 * process that wants a page the replay has issued waits for that i/o.
 * If the i/o fails, the pager frees the pages and the waiting process
 * reads them itself; the replay does not issue them again.
 *
 * The budget is charged when the i/o is issued, not when it completes,
 * so the i/o in flight counts against it too.  The replay stops when
 * everything has been issued, when the budget is used up, on timeout,
 * or when free memory drops below twice the pagedaemon target.  However
 * it stops, it waits for its i/o to complete before it releases the
 * vnodes.
 *
 * Unmount.  dounmount() calls aprefetch_unmount() before it suspends
 * the file system.  The replay drops what it has not done there, waits
 * for its i/o, releases its vnodes on that file system and only then
 * lets the unmount go on.  The order matters: on a suspended file
 * system vfs_busy() and VOP_GETPAGES sleep, and a replay asleep there
 * could never answer.
 *
 * Marks.  write(2) stores a timestamped label together with the read
 * counters of the root disk; /etc/rc writes one around each rc.d
 * script.  Marks work even when recording and replay are off, so the
 * same timing is available for a baseline boot.
 *
 * The ordering rules above are checked by the TLA+ model in
 * tla/aprefetch.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/types.h>
#include <sys/aprefetch.h>
#include <sys/atomic.h>
#include <sys/callout.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/cpu.h>
#include <sys/device.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/iostat.h>
#include <sys/kauth.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/reboot.h>
#include <sys/rwlock.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/xcall.h>

#include <lib/libkern/libkern.h>

#include <miscfs/specfs/specdev.h>	/* v_rdev */

#include <ufs/ufs/inode.h>		/* struct ufid */

#include <uvm/uvm.h>

#include "ioconf.h"

#define	APREF_RING	16384		/* access entries per CPU */
#define	APREF_NMARKS	1024
#define	APREF_READMAX	256		/* entries copied per read(2) round */
#define	APREF_BMAPMAX	4096		/* extents per APREFIOC_BMAP */

/*
 * On x86 the boot flag -4 (RB_MD4) turns aprefetch off for one boot, so
 * that a broken profile or a bug here can be bypassed from the boot
 * menu.  Other ports have no such flag yet.
 */
#if defined(__i386__) || defined(__x86_64__)
#define	APREFETCH_BOOT_DISABLED()	((boothowto & RB_MD4) != 0)
#else
#define	APREFETCH_BOOT_DISABLED()	false
#endif

static int	aprefetch_enable = 1;
static uint64_t	aprefetch_boot_budget = 256 * 1024 * 1024;
static uint64_t	aprefetch_app_budget = 64 * 1024 * 1024;
static int	aprefetch_max_io = 8;
static int	aprefetch_replay_timeout = 60;		/* seconds */
static int	aprefetch_record_timeout = 120;		/* seconds */

/*
 * Recording.  ac_head is written only by the CPU itself, with preemption
 * disabled; ac_tail only by the reader, under aprefetch_lock.
 */
struct aprefetch_cpu {
	struct aprefetch_rec	*ac_ring;
	unsigned int		ac_head;
	unsigned int		ac_tail;
	bool			ac_pending;	/* ac_cur not in the ring yet */
	struct aprefetch_rec	ac_cur;
	uint64_t		ac_records;
	uint64_t		ac_dropped;
	uint64_t		ac_hits;
	uint64_t		ac_misses;
} __aligned(COHERENCY_UNIT);

int				aprefetch_recording;

static struct aprefetch_cpu	aprefetch_cpus[MAXCPUS];
static int			aprefetch_rings_ready;

static kmutex_t			aprefetch_lock;
static kcondvar_t		aprefetch_cv;
static callout_t		aprefetch_window_ch;
static int			aprefetch_window;	/* APREF_WIN_* */
static bool			aprefetch_closing;	/* rings not settled */
static unsigned int		aprefetch_window_gen;
static struct timespec		aprefetch_window_start;
static unsigned int		aprefetch_rotor;

static struct aprefetch_mark	aprefetch_marks[APREF_NMARKS];
static unsigned int		aprefetch_nmarks;
static uint64_t			aprefetch_marks_dropped;

/*
 * Replay.  A replay runs in one kernel thread; only that thread touches
 * a struct aprefetch_run, except for the fields marked (L), which are
 * protected by aprefetch_lock.
 */
#define	APREF_FILE_NEW		0	/* not looked up yet */
#define	APREF_FILE_OPEN		1	/* af_vp is referenced */
#define	APREF_FILE_DONE		2	/* released, or could not be used */

struct aprefetch_file {
	uint64_t		af_fsid;
	uint64_t		af_ino;
	uint32_t		af_gen;
	int			af_state;
	uint32_t		af_next;	/* hash chain */
	uint32_t		af_pass;	/* last pass that looked */
	struct vnode		*af_vp;
	off_t			af_size;
};

#define	APREF_ENT_TODO		0
#define	APREF_ENT_DEFERRED	1	/* file system not mounted yet */
#define	APREF_ENT_DONE		2	/* mapped, or dropped */

struct aprefetch_ent {
	struct aprefetch_file	*e_file;
	uint64_t		e_off;
	uint32_t		e_npages;
	int			e_state;
};

struct aprefetch_ext {
	struct aprefetch_file	*x_file;
	uint64_t		x_dev;
	int64_t			x_blkno;
	off_t			x_off;
	uint32_t		x_npages;
};

struct aprefetch_io {
	struct aprefetch_file	*io_file;
	off_t			io_off;
	int			io_npages;
};

struct aprefetch_umreq {
	TAILQ_ENTRY(aprefetch_umreq) ur_entry;
	struct mount		*ur_mp;
	bool			ur_done;
};

struct aprefetch_run {
	bool			r_boot;
	uint64_t		r_budget;	/* pages */
	uint64_t		r_used;		/* pages charged */
	uint64_t		r_deadline;	/* msec of uptime */
	int			r_end;		/* APREF_END_* */
	uint32_t		r_pass;

	struct aprefetch_ent	*r_ents;
	uint32_t		r_nents;
	struct aprefetch_file	*r_files;	/* room for r_nents */
	uint32_t		r_nfiles;
	uint32_t		*r_hash;
	uint32_t		r_hashmask;
	uint32_t		r_ndeferred;

	struct aprefetch_ext	*r_exts;
	uint32_t		r_nexts;
	uint32_t		r_maxexts;
	uint32_t		r_passext;	/* first extent of this pass */

	struct aprefetch_io	*r_io;		/* ring of r_maxio */
	uint32_t		r_maxio;
	uint32_t		r_iotail;
	uint32_t		r_niow;		/* i/os in flight */

	TAILQ_HEAD(, aprefetch_umreq) r_umreqs;	/* (L) */
	bool			r_mounted;	/* (L) a mount happened */
};

static struct aprefetch_run	*aprefetch_running;	/* (L) */
static struct aprefetch_stats	aprefetch_st;	/* replay part */

typedef void (*aprefetch_extfn_t)(void *, uint64_t, int64_t, off_t,
    uint32_t);

extern struct iostatlist_head	iostatlist;
extern krwlock_t		iostatlist_lock;

static dev_type_open(aprefetch_open);
static dev_type_close(aprefetch_close);
static dev_type_read(aprefetch_read);
static dev_type_write(aprefetch_write);
static dev_type_ioctl(aprefetch_ioctl);

const struct cdevsw aprefetch_cdevsw = {
	.d_open = aprefetch_open,
	.d_close = aprefetch_close,
	.d_read = aprefetch_read,
	.d_write = aprefetch_write,
	.d_ioctl = aprefetch_ioctl,
	.d_stop = nostop,
	.d_tty = notty,
	.d_poll = nopoll,
	.d_mmap = nommap,
	.d_kqfilter = nokqfilter,
	.d_discard = nodiscard,
	.d_flag = D_OTHER | D_MPSAFE
};

static uint32_t
aprefetch_msec(const struct timespec *since)
{
	struct timespec now, d;

	getnanouptime(&now);
	timespecsub(&now, since, &d);
	return (uint32_t)(d.tv_sec * 1000 + d.tv_nsec / 1000000);
}

/* Uptime in milliseconds, for deadlines finer than time_uptime. */
static uint64_t
aprefetch_uptime_ms(void)
{
	struct timespec now;

	getnanouptime(&now);
	return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/*
 * Marks.
 */

/* Read counters of the disk that holds the root file system. */
static void
aprefetch_disk_reads(uint64_t *xferp, uint64_t *bytesp)
{
	struct io_stats *stats;
	const char *name;

	*xferp = *bytesp = 0;
	if (root_device == NULL)
		return;
	name = device_xname(root_device);
	rw_enter(&iostatlist_lock, RW_READER);
	TAILQ_FOREACH(stats, &iostatlist, io_link) {
		if (stats->io_type == IOSTAT_DISK &&
		    strcmp(stats->io_name, name) == 0) {
			*xferp = stats->io_rxfer;
			*bytesp = stats->io_rbytes;
			break;
		}
	}
	rw_exit(&iostatlist_lock);
}

static void
aprefetch_mark(const char *label)
{
	struct aprefetch_mark *am;
	struct timespec ts;
	uint64_t xfer, bytes;

	nanouptime(&ts);
	aprefetch_disk_reads(&xfer, &bytes);

	mutex_enter(&aprefetch_lock);
	if (aprefetch_nmarks < APREF_NMARKS) {
		am = &aprefetch_marks[aprefetch_nmarks++];
		am->am_nsec = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
		am->am_rxfer = xfer;
		am->am_rbytes = bytes;
		strlcpy(am->am_label, label, sizeof(am->am_label));
	} else
		aprefetch_marks_dropped++;
	mutex_exit(&aprefetch_lock);
}

void
aprefetch_kmark(const char *label)
{

	aprefetch_mark(label);
}

/*
 * Recording.
 */

static void
aprefetch_publish(struct aprefetch_cpu *ac)
{
	const unsigned int head = ac->ac_head;

	ac->ac_pending = false;
	if (head - atomic_load_acquire(&ac->ac_tail) >= APREF_RING) {
		ac->ac_dropped++;
		return;
	}
	ac->ac_ring[head % APREF_RING] = ac->ac_cur;
	atomic_store_release(&ac->ac_head, head + 1);
	ac->ac_records++;
}

static void
aprefetch_add(struct aprefetch_cpu *ac, const struct aprefetch_rec *rec)
{
	struct aprefetch_rec * const cur = &ac->ac_cur;

	if (rec->ar_flags & APREF_REC_MISS)
		ac->ac_misses++;
	else
		ac->ac_hits++;

	if (ac->ac_pending && cur->ar_fsid == rec->ar_fsid &&
	    cur->ar_ino == rec->ar_ino && cur->ar_gen == rec->ar_gen &&
	    cur->ar_pid == rec->ar_pid) {
		const uint64_t cend = cur->ar_off +
		    ((uint64_t)cur->ar_npages << PAGE_SHIFT);
		const uint64_t rend = rec->ar_off +
		    ((uint64_t)rec->ar_npages << PAGE_SHIFT);
		const uint64_t start = MIN(cur->ar_off, rec->ar_off);
		const uint64_t end = MAX(cend, rend);

		if (rec->ar_off <= cend && rend >= cur->ar_off &&
		    ((end - start) >> PAGE_SHIFT) <= UINT32_MAX) {
			cur->ar_off = start;
			cur->ar_npages = (uint32_t)((end - start) >> PAGE_SHIFT);
			cur->ar_flags |= rec->ar_flags;
			return;
		}
	}
	if (ac->ac_pending)
		aprefetch_publish(ac);
	*cur = *rec;
	ac->ac_pending = true;
}

/*
 * Called by genfs_getpages() with the object locked, while a window is
 * open.  Requests that want no pages back (readahead) are read-ahead,
 * our own replay included, and are not recorded.
 */
void
aprefetch_getpages(struct vnode *vp, off_t off, int npages, int centeridx,
    int flags, bool memwrite, bool readahead)
{
	struct mount * const mp = vp->v_mount;
	struct aprefetch_rec rec;
	struct vm_page *pg;
	struct ufid ufh;
	size_t fhsize;

	if (memwrite || readahead || vp->v_type != VREG || mp == NULL ||
	    strcmp(mp->mnt_stat.f_fstypename, MOUNT_FFS) != 0)
		return;
	if (flags & PGO_LOCKED) {
		/* Fault fast path: the neighbours are mapped if resident. */
		off += (off_t)centeridx << PAGE_SHIFT;
		npages = 1;
	}
	fhsize = sizeof(ufh);
	if (VFS_VPTOFH(vp, (struct fid *)&ufh, &fhsize) != 0)
		return;
	pg = uvm_pagelookup(&vp->v_uobj, off);

	rec.ar_fsid = APREFETCH_FSID(mp->mnt_stat.f_fsidx);
	rec.ar_ino = ufh.ufid_ino;
	rec.ar_off = off;
	rec.ar_gen = ufh.ufid_gen;
	rec.ar_npages = npages;
	rec.ar_pid = curproc->p_pid;
	rec.ar_flags = (pg == NULL || (pg->flags & PG_FAKE) != 0) ?
	    APREF_REC_MISS : 0;
	rec.ar_pad = 0;

	kpreempt_disable();
	/* The window may have closed while we got here. */
	if (atomic_load_acquire(&aprefetch_recording)) {
		rec.ar_msec = aprefetch_msec(&aprefetch_window_start);
		aprefetch_add(&aprefetch_cpus[cpu_index(curcpu())], &rec);
	}
	kpreempt_enable();
}

static void
aprefetch_alloc_rings(void)
{
	const size_t size = APREF_RING * sizeof(struct aprefetch_rec);
	struct aprefetch_rec **ring;
	unsigned int i;

	if (atomic_load_acquire(&aprefetch_rings_ready))
		return;
	ring = kmem_alloc(ncpu * sizeof(*ring), KM_SLEEP);
	for (i = 0; i < ncpu; i++)
		ring[i] = kmem_zalloc(size, KM_SLEEP);

	mutex_enter(&aprefetch_lock);
	if (!aprefetch_rings_ready) {
		for (i = 0; i < ncpu; i++) {
			aprefetch_cpus[i].ac_ring = ring[i];
			ring[i] = NULL;
		}
		atomic_store_release(&aprefetch_rings_ready, 1);
	}
	mutex_exit(&aprefetch_lock);

	for (i = 0; i < ncpu; i++) {
		if (ring[i] != NULL)
			kmem_free(ring[i], size);
	}
	kmem_free(ring, ncpu * sizeof(*ring));
}

static void
aprefetch_window_timeout(void *arg)
{

	mutex_enter(&aprefetch_lock);
	if (aprefetch_window != APREF_WIN_CLOSED &&
	    aprefetch_window_gen == (unsigned int)(uintptr_t)arg) {
		atomic_store_relaxed(&aprefetch_recording, 0);
		aprefetch_window = APREF_WIN_CLOSED;
		aprefetch_closing = true;
		cv_broadcast(&aprefetch_cv);
	}
	mutex_exit(&aprefetch_lock);
}

static int
aprefetch_window_open(int kind)
{
	unsigned int i;

	KASSERT(mutex_owned(&aprefetch_lock));

	if (aprefetch_window != APREF_WIN_CLOSED || aprefetch_closing)
		return EBUSY;
	if (!aprefetch_rings_ready)
		return ENOMEM;

	/* No CPU can be in aprefetch_add() now: start from empty rings. */
	for (i = 0; i < ncpu; i++) {
		struct aprefetch_cpu * const ac = &aprefetch_cpus[i];

		ac->ac_head = ac->ac_tail = 0;
		ac->ac_pending = false;
	}
	getnanouptime(&aprefetch_window_start);
	aprefetch_window = kind;
	aprefetch_window_gen++;
	callout_reset(&aprefetch_window_ch,
	    mstohz((unsigned int)aprefetch_record_timeout * 1000),
	    aprefetch_window_timeout, (void *)(uintptr_t)aprefetch_window_gen);
	atomic_store_release(&aprefetch_recording, 1);
	return 0;
}

static void
aprefetch_window_close(void)
{

	KASSERT(mutex_owned(&aprefetch_lock));

	if (aprefetch_window == APREF_WIN_CLOSED)
		return;
	atomic_store_relaxed(&aprefetch_recording, 0);
	aprefetch_window = APREF_WIN_CLOSED;
	aprefetch_closing = true;
	callout_stop(&aprefetch_window_ch);
	cv_broadcast(&aprefetch_cv);
}

static void
aprefetch_flush_cpu(void *arg1, void *arg2)
{
	struct aprefetch_cpu *ac;

	kpreempt_disable();
	ac = &aprefetch_cpus[cpu_index(curcpu())];
	if (ac->ac_ring != NULL && ac->ac_pending)
		aprefetch_publish(ac);
	kpreempt_enable();
}

/*
 * Publish the entries being merged.  The cross call runs on each CPU
 * between its calls to aprefetch_add(), so it also waits out any CPU
 * that saw the window still open.
 */
static void
aprefetch_flush(void)
{

	xc_wait(xc_broadcast(0, aprefetch_flush_cpu, NULL, NULL));
}

/* After a window has closed, flush it.  Called without locks. */
static void
aprefetch_settle(void)
{
	bool closing;

	mutex_enter(&aprefetch_lock);
	closing = aprefetch_closing;
	mutex_exit(&aprefetch_lock);
	if (!closing)
		return;

	aprefetch_flush();

	mutex_enter(&aprefetch_lock);
	aprefetch_closing = false;
	cv_broadcast(&aprefetch_cv);
	mutex_exit(&aprefetch_lock);
}

static unsigned int
aprefetch_collect(struct aprefetch_rec *buf, unsigned int max)
{
	unsigned int i, n = 0;

	KASSERT(mutex_owned(&aprefetch_lock));

	if (!aprefetch_rings_ready)
		return 0;
	for (i = 0; i < ncpu && n < max; i++) {
		struct aprefetch_cpu * const ac =
		    &aprefetch_cpus[(aprefetch_rotor + i) % ncpu];
		const unsigned int head = atomic_load_acquire(&ac->ac_head);
		unsigned int tail = ac->ac_tail;

		while (tail != head && n < max)
			buf[n++] = ac->ac_ring[tail++ % APREF_RING];
		atomic_store_release(&ac->ac_tail, tail);
	}
	aprefetch_rotor++;
	return n;
}

/*
 * Profiles.
 */

static int
aprefetch_readprof(const char *path, void **bufp, size_t *lenp)
{
	const kauth_cred_t cred = kauth_cred_get();
	struct pathbuf *pb;
	struct vnode *vp;
	struct vattr va;
	void *buf = NULL;
	size_t len = 0, resid;
	int error;

	pb = pathbuf_create(path);
	if (pb == NULL)
		return ENOMEM;
	error = vn_open(NULL, pb, 0, FREAD | O_NOFOLLOW, 0, &vp, NULL, NULL);
	pathbuf_destroy(pb);
	if (error)
		return error;

	error = VOP_GETATTR(vp, &va, cred);
	/* Only root may decide what the kernel reads at boot. */
	if (error == 0 && (va.va_type != VREG || va.va_uid != 0 ||
	    (va.va_mode & (S_IWGRP | S_IWOTH)) != 0))
		error = EPERM;
	if (error == 0 && (va.va_size < sizeof(struct aprefetch_prof_hdr) ||
	    va.va_size > APREF_PROF_MAXSIZE))
		error = EFTYPE;
	if (error == 0) {
		len = va.va_size;
		buf = kmem_alloc(len, KM_SLEEP);
		error = vn_rdwr(UIO_READ, vp, buf, len, 0, UIO_SYSSPACE,
		    IO_NODELOCKED, cred, &resid, NULL);
		if (error == 0 && resid != 0)
			error = EIO;
		if (error)
			kmem_free(buf, len);
	}
	VOP_UNLOCK(vp);
	(void)vn_close(vp, FREAD, cred);
	if (error == 0) {
		*bufp = buf;
		*lenp = len;
	}
	return error;
}

static struct aprefetch_file *
aprefetch_file_get(struct aprefetch_run *run, uint64_t fsid, uint64_t ino,
    uint32_t gen)
{
	struct aprefetch_file *af;
	uint64_t x;
	uint32_t h, i;

	x = (ino ^ (fsid << 7)) * 0x9e3779b97f4a7c15ULL;
	h = (uint32_t)(x >> 32) & run->r_hashmask;
	for (i = run->r_hash[h]; i != UINT32_MAX; i = af->af_next) {
		af = &run->r_files[i];
		if (af->af_fsid == fsid && af->af_ino == ino && af->af_gen == gen)
			return af;
	}
	i = run->r_nfiles++;
	af = &run->r_files[i];
	af->af_fsid = fsid;
	af->af_ino = ino;
	af->af_gen = gen;
	af->af_state = APREF_FILE_NEW;
	af->af_next = run->r_hash[h];
	run->r_hash[h] = i;
	return af;
}

/*
 * Check a profile image and take the entries that fit in the budget, in
 * profile order.
 */
static int
aprefetch_run_parse(struct aprefetch_run *run, const void *buf, size_t len)
{
	const struct aprefetch_prof_ent *pe;
	struct aprefetch_prof_hdr ph;
	uint64_t left;
	uint32_t i, n, nb, crc;

	if (len < sizeof(ph) || len > APREF_PROF_MAXSIZE)
		return EFTYPE;
	memcpy(&ph, buf, sizeof(ph));
	if (ph.ph_magic != APREF_PROF_MAGIC ||
	    ph.ph_version != APREF_PROF_VERSION ||
	    ph.ph_nfs > APREF_PROF_MAXFS ||
	    (len - sizeof(ph)) % sizeof(*pe) != 0 ||
	    (len - sizeof(ph)) / sizeof(*pe) != ph.ph_nentries)
		return EFTYPE;
	crc = ph.ph_crc;
	ph.ph_crc = 0;
	if (crc32(crc32(0, (const uint8_t *)&ph, sizeof(ph)),
	    (const uint8_t *)buf + sizeof(ph), len - sizeof(ph)) != crc)
		return EFTYPE;

	pe = (const struct aprefetch_prof_ent *)
	    ((const uint8_t *)buf + sizeof(ph));
	for (i = 0; i < ph.ph_nentries; i++) {
		if (pe[i].pe_npages == 0 || (pe[i].pe_off & PAGE_MASK) != 0 ||
		    pe[i].pe_off > (uint64_t)INT64_MAX -
		    ((uint64_t)pe[i].pe_npages << PAGE_SHIFT))
			return EFTYPE;
	}

	/* Earlier reads first: what does not fit in the budget is dropped. */
	left = run->r_budget;
	for (n = 0; n < ph.ph_nentries && left > 0; n++)
		left -= MIN(left, pe[n].pe_npages);
	if (n == 0)
		return 0;

	for (nb = 1; nb < n; nb <<= 1)
		continue;
	run->r_ents = kmem_zalloc(n * sizeof(*run->r_ents), KM_SLEEP);
	run->r_files = kmem_zalloc(n * sizeof(*run->r_files), KM_SLEEP);
	run->r_hash = kmem_alloc(nb * sizeof(*run->r_hash), KM_SLEEP);
	run->r_hashmask = nb - 1;
	for (i = 0; i < nb; i++)
		run->r_hash[i] = UINT32_MAX;
	run->r_nents = n;

	left = run->r_budget;
	for (i = 0; i < n; i++) {
		struct aprefetch_ent * const e = &run->r_ents[i];

		e->e_file = aprefetch_file_get(run, pe[i].pe_fsid,
		    pe[i].pe_ino, pe[i].pe_gen);
		e->e_off = pe[i].pe_off;
		e->e_npages = (uint32_t)MIN(left, pe[i].pe_npages);
		e->e_state = APREF_ENT_TODO;
		left -= e->e_npages;
	}
	aprefetch_st.st_entries += n;
	return 0;
}

/*
 * Files.
 */

/*
 * Get a referenced vnode for a recorded file.  ENODEV means that its
 * file system is not mounted.
 */
static int
aprefetch_vget(uint64_t fsid, uint64_t ino, uint32_t gen, struct vnode **vpp,
    off_t *sizep)
{
	mount_iterator_t *iter;
	struct mount *mp;
	struct vnode *vp = NULL;
	struct vattr va;
	struct ufid ufh;
	int error = ENODEV;

	mountlist_iterator_init(&iter);
	while ((mp = mountlist_iterator_next(iter)) != NULL) {
		if (APREFETCH_FSID(mp->mnt_stat.f_fsidx) != fsid)
			continue;
		if (strcmp(mp->mnt_stat.f_fstypename, MOUNT_FFS) != 0) {
			error = EOPNOTSUPP;
			break;
		}
		memset(&ufh, 0, sizeof(ufh));
		ufh.ufid_len = sizeof(ufh);
		ufh.ufid_ino = ino;
		ufh.ufid_gen = gen;
		/* Checks the inode number, mode and generation number. */
		error = VFS_FHTOVP(mp, (struct fid *)&ufh, LK_SHARED, &vp);
		break;
	}
	mountlist_iterator_destroy(iter);
	if (error)
		return error;

	if (vp->v_type != VREG) {
		vput(vp);
		return EFTYPE;
	}
	error = VOP_GETATTR(vp, &va, kauth_cred_get());
	VOP_UNLOCK(vp);
	if (error) {
		vrele(vp);
		return error;
	}
	*vpp = vp;
	*sizep = va.va_size;
	return 0;
}

/*
 * Call fn for each physically contiguous piece of [off, end) of vp.
 * A hole is reported with block -1.
 */
static int
aprefetch_bmap(struct vnode *vp, off_t off, off_t end, aprefetch_extfn_t fn,
    void *arg)
{
	const int bshift = vp->v_mount->mnt_fs_bshift;
	struct vnode *devvp;
	daddr_t lbn, blkno;
	off_t next;
	int run, error = 0;

	vn_lock(vp, LK_SHARED | LK_RETRY);
	while (off < end) {
		lbn = off >> bshift;
		error = VOP_BMAP(vp, lbn, &devvp, &blkno, &run);
		if (error)
			break;
		next = MIN(round_page((off_t)(lbn + 1 + run) << bshift), end);
		if (blkno == (daddr_t)-1)
			(*fn)(arg, 0, -1, off, (next - off) >> PAGE_SHIFT);
		else
			(*fn)(arg, devvp->v_rdev,
			    blkno + ((off - ((off_t)lbn << bshift)) >> DEV_BSHIFT),
			    off, (next - off) >> PAGE_SHIFT);
		off = next;
	}
	VOP_UNLOCK(vp);
	return error;
}

/*
 * Replay.
 */

struct aprefetch_mapctx {
	struct aprefetch_run	*mc_run;
	struct aprefetch_file	*mc_file;
};

static void
aprefetch_add_ext(void *arg, uint64_t dev, int64_t blkno, off_t off,
    uint32_t npages)
{
	struct aprefetch_mapctx * const mc = arg;
	struct aprefetch_run * const run = mc->mc_run;
	struct aprefetch_ext *x, *nx;
	uint32_t nmax;

	if (blkno < 0)
		return;		/* a hole: nothing to read */

	/* Physically contiguous with the last piece: make it one. */
	if (run->r_nexts > run->r_passext) {
		x = &run->r_exts[run->r_nexts - 1];
		if (x->x_file == mc->mc_file && x->x_dev == dev &&
		    x->x_off + ((off_t)x->x_npages << PAGE_SHIFT) == off &&
		    x->x_blkno + ((off_t)x->x_npages << (PAGE_SHIFT -
		    DEV_BSHIFT)) == blkno &&
		    x->x_npages + npages > x->x_npages) {
			x->x_npages += npages;
			return;
		}
	}
	if (run->r_nexts == run->r_maxexts) {
		nmax = MAX(run->r_maxexts * 2, 256);
		nx = kmem_alloc(nmax * sizeof(*nx), KM_SLEEP);
		if (run->r_maxexts > 0) {
			memcpy(nx, run->r_exts, run->r_nexts * sizeof(*nx));
			kmem_free(run->r_exts, run->r_maxexts * sizeof(*nx));
		}
		run->r_exts = nx;
		run->r_maxexts = nmax;
	}
	x = &run->r_exts[run->r_nexts++];
	x->x_file = mc->mc_file;
	x->x_dev = dev;
	x->x_blkno = blkno;
	x->x_off = off;
	x->x_npages = npages;
}

static int
aprefetch_ext_cmp(const void *a, const void *b)
{
	const struct aprefetch_ext *x = a, *y = b;

	if (x->x_dev != y->x_dev)
		return x->x_dev < y->x_dev ? -1 : 1;
	if (x->x_blkno != y->x_blkno)
		return x->x_blkno < y->x_blkno ? -1 : 1;
	return 0;
}

/* Number of pages of [off, off + npages) that are not resident. */
static int
aprefetch_absent(struct vnode *vp, off_t off, int npages)
{
	struct uvm_object * const uobj = &vp->v_uobj;
	int i, n = 0;

	rw_enter(uobj->vmobjlock, RW_READER);
	for (i = 0; i < npages; i++) {
		if (uvm_pagelookup(uobj, off + ((off_t)i << PAGE_SHIFT)) == NULL)
			n++;
	}
	rw_exit(uobj->vmobjlock);
	return n;
}

/*
 * Start asynchronous read-ahead.  The pager finds or allocates the pages
 * busy under the object lock and fails with EBUSY if one is busy, that
 * is, if someone is reading it already.
 */
static int
aprefetch_start_io(struct vnode *vp, off_t off, int npages)
{
	struct uvm_object * const uobj = &vp->v_uobj;
	int n = npages;

	rw_enter(uobj->vmobjlock, RW_WRITER);
	/* pgo_get() unlocks the object. */
	return (*uobj->pgops->pgo_get)(uobj, off, NULL, &n, 0, VM_PROT_READ,
	    UVM_ADV_RANDOM, PGO_NOTIMESTAMP);
}

/*
 * Wait until no page of the range is busy.  Returns the number of pages
 * missing then; a failed read frees its pages.
 */
static int
aprefetch_wait_io(struct vnode *vp, off_t off, int npages)
{
	struct uvm_object * const uobj = &vp->v_uobj;
	struct vm_page *pg;
	int i = 0, missing = 0;

	rw_enter(uobj->vmobjlock, RW_READER);
	while (i < npages) {
		pg = uvm_pagelookup(uobj, off + ((off_t)i << PAGE_SHIFT));
		if (pg != NULL && (pg->flags & PG_BUSY) != 0) {
			uvm_pagewait(pg, uobj->vmobjlock, "aprefio");
			rw_enter(uobj->vmobjlock, RW_READER);
			continue;
		}
		if (pg == NULL)
			missing++;
		i++;
	}
	rw_exit(uobj->vmobjlock);
	return missing;
}

/* Wait for the oldest i/o in flight. */
static void
aprefetch_reap(struct aprefetch_run *run)
{
	struct aprefetch_io *io;

	KASSERT(run->r_niow > 0);
	io = &run->r_io[run->r_iotail % run->r_maxio];
	aprefetch_st.st_errors += aprefetch_wait_io(io->io_file->af_vp,
	    io->io_off, io->io_npages);
	run->r_iotail++;
	run->r_niow--;
}

static void
aprefetch_drain(struct aprefetch_run *run)
{

	while (run->r_niow > 0)
		aprefetch_reap(run);
}

/* Release the vnodes, of mp only if mp is not NULL.  Drain first. */
static void
aprefetch_release(struct aprefetch_run *run, struct mount *mp)
{
	struct aprefetch_file *af;
	uint32_t i;

	KASSERT(run->r_niow == 0);
	for (i = 0; i < run->r_nfiles; i++) {
		af = &run->r_files[i];
		if (af->af_state != APREF_FILE_OPEN)
			continue;
		if (mp != NULL && af->af_vp->v_mount != mp)
			continue;
		vrele(af->af_vp);
		af->af_vp = NULL;
		af->af_state = APREF_FILE_DONE;
	}
}

/* Give up everything on mp, which is about to be unmounted. */
static void
aprefetch_detach(struct aprefetch_run *run, struct mount *mp)
{
	const uint64_t fsid = APREFETCH_FSID(mp->mnt_stat.f_fsidx);
	struct aprefetch_ent *e;
	uint32_t i;

	aprefetch_drain(run);
	aprefetch_release(run, mp);
	for (i = 0; i < run->r_nfiles; i++) {
		if (run->r_files[i].af_fsid == fsid)
			run->r_files[i].af_state = APREF_FILE_DONE;
	}
	for (i = 0; i < run->r_nents; i++) {
		e = &run->r_ents[i];
		if (e->e_state == APREF_ENT_DONE ||
		    e->e_file->af_state != APREF_FILE_DONE)
			continue;
		if (e->e_state == APREF_ENT_DEFERRED)
			run->r_ndeferred--;
		e->e_state = APREF_ENT_DONE;
	}
}

/*
 * Called between the steps of a replay.  Answers unmount requests and
 * returns true if the replay must stop now.
 */
static bool
aprefetch_stop(struct aprefetch_run *run)
{
	struct aprefetch_umreq *ur;

	mutex_enter(&aprefetch_lock);
	while ((ur = TAILQ_FIRST(&run->r_umreqs)) != NULL) {
		mutex_exit(&aprefetch_lock);
		aprefetch_detach(run, ur->ur_mp);
		mutex_enter(&aprefetch_lock);
		/* Answer only now that the vnodes are released. */
		TAILQ_REMOVE(&run->r_umreqs, ur, ur_entry);
		ur->ur_done = true;
		cv_broadcast(&aprefetch_cv);
	}
	mutex_exit(&aprefetch_lock);

	if (run->r_end == APREF_END_NONE) {
		if (!atomic_load_relaxed(&aprefetch_enable))
			run->r_end = APREF_END_DISABLED;
		else if (aprefetch_uptime_ms() >= run->r_deadline)
			run->r_end = APREF_END_TIMEOUT;
		else if (uvm_availmem(false) < 2 * uvmexp.freetarg)
			run->r_end = APREF_END_MEMORY;
	}
	return run->r_end != APREF_END_NONE;
}

static void
aprefetch_map_ent(struct aprefetch_run *run, struct aprefetch_ent *e)
{
	struct aprefetch_file * const af = e->e_file;
	struct aprefetch_mapctx mc = { .mc_run = run, .mc_file = af };
	const off_t off = e->e_off;
	off_t end;

	/* The file may have shrunk since it was recorded. */
	end = MIN(off + ((off_t)e->e_npages << PAGE_SHIFT),
	    round_page(af->af_size));
	if (off >= end) {
		aprefetch_st.st_stale++;
		return;
	}
	(void)aprefetch_bmap(af->af_vp, off, end, aprefetch_add_ext, &mc);
}

/* Issue the extents of this pass in sorted order. */
static void
aprefetch_issue(struct aprefetch_run *run)
{
	struct aprefetch_ext *x;
	struct aprefetch_file *af;
	struct aprefetch_io *io;
	off_t off, end;
	uint32_t i;
	int npages, absent, error;

	for (i = run->r_passext; i < run->r_nexts; i++) {
		x = &run->r_exts[i];
		af = x->x_file;
		off = x->x_off;
		end = off + ((off_t)x->x_npages << PAGE_SHIFT);
		while (off < end) {
			if (aprefetch_stop(run))
				return;
			if (af->af_state != APREF_FILE_OPEN)
				break;		/* its file system went away */
			if (run->r_used >= run->r_budget) {
				run->r_end = APREF_END_BUDGET;
				return;
			}
			/*
			 * Stay inside one MAXPHYS-aligned window, as
			 * uvm_readahead() does: the pager widens the
			 * request to whole file system blocks, and an
			 * asynchronous read may not exceed MAXPHYS.
			 */
			npages = (int)((MIN(end, (off + MAXPHYS) &
			    ~(off_t)(MAXPHYS - 1)) - off) >> PAGE_SHIFT);
			npages = (int)MIN((uint64_t)npages,
			    run->r_budget - run->r_used);
			while (run->r_niow >= run->r_maxio)
				aprefetch_reap(run);

			absent = aprefetch_absent(af->af_vp, off, npages);
			if (absent == 0) {
				aprefetch_st.st_cached += npages;
			} else if ((error = aprefetch_start_io(af->af_vp, off,
			    npages)) == 0) {
				/* Charge now, while the i/o is in flight. */
				run->r_used += absent;
				aprefetch_st.st_pages += absent;
				aprefetch_st.st_cached += npages - absent;
				aprefetch_st.st_ios++;
				io = &run->r_io[(run->r_iotail + run->r_niow) %
				    run->r_maxio];
				io->io_file = af;
				io->io_off = off;
				io->io_npages = npages;
				run->r_niow++;
			} else if (error == EBUSY) {
				/* Being read already; not issued again. */
				aprefetch_st.st_cached += npages;
			} else
				aprefetch_st.st_errors += absent;
			off += (off_t)npages << PAGE_SHIFT;
		}
	}
}

/*
 * One pass: look up, map, sort and issue every entry whose file system
 * is mounted, then wait for the i/o and release the vnodes.
 */
static void
aprefetch_pass(struct aprefetch_run *run)
{
	struct aprefetch_ent *e;
	struct aprefetch_file *af;
	struct aprefetch_ext tmp;
	uint32_t i;
	int error;

	run->r_pass++;
	run->r_passext = run->r_nexts;
	for (i = 0; i < run->r_nents; i++) {
		e = &run->r_ents[i];
		if (e->e_state == APREF_ENT_DONE)
			continue;
		if (aprefetch_stop(run))
			goto out;
		af = e->e_file;
		if (af->af_state == APREF_FILE_NEW) {
			error = ENODEV;
			if (af->af_pass != run->r_pass) {
				af->af_pass = run->r_pass;
				error = aprefetch_vget(af->af_fsid, af->af_ino,
				    af->af_gen, &af->af_vp, &af->af_size);
			}
			if (error == ENODEV) {
				/* Not mounted yet: keep it for the mount. */
				if (e->e_state != APREF_ENT_DEFERRED) {
					e->e_state = APREF_ENT_DEFERRED;
					run->r_ndeferred++;
					aprefetch_st.st_deferred++;
				}
				continue;
			}
			af->af_state = error ? APREF_FILE_DONE :
			    APREF_FILE_OPEN;
		}
		if (e->e_state == APREF_ENT_DEFERRED)
			run->r_ndeferred--;
		e->e_state = APREF_ENT_DONE;
		if (af->af_state != APREF_FILE_OPEN) {
			aprefetch_st.st_stale++;
			continue;
		}
		aprefetch_map_ent(run, e);
	}
	if (run->r_nexts > run->r_passext)
		kheapsort(&run->r_exts[run->r_passext],
		    run->r_nexts - run->r_passext, sizeof(tmp),
		    aprefetch_ext_cmp, &tmp);
	aprefetch_issue(run);
out:
	aprefetch_drain(run);
	aprefetch_release(run, NULL);
}

/* Wait for a mount; returns true if one happened and we may go on. */
static bool
aprefetch_wait_mount(struct aprefetch_run *run)
{
	uint64_t now;
	bool mounted;

	mutex_enter(&aprefetch_lock);
	while (!run->r_mounted && TAILQ_EMPTY(&run->r_umreqs) &&
	    atomic_load_relaxed(&aprefetch_enable) &&
	    (now = aprefetch_uptime_ms()) < run->r_deadline) {
		/* No more than replay_timeout, at most 3600 s, is left. */
		(void)cv_timedwait(&aprefetch_cv, &aprefetch_lock,
		    MAX(1, mstohz((unsigned int)(run->r_deadline - now))));
	}
	mounted = run->r_mounted;
	run->r_mounted = false;
	mutex_exit(&aprefetch_lock);
	return !aprefetch_stop(run) && mounted;
}

static struct aprefetch_run *
aprefetch_run_alloc(uint64_t budget, bool boot)
{
	struct aprefetch_run *run;

	run = kmem_zalloc(sizeof(*run), KM_SLEEP);
	run->r_boot = boot;
	run->r_budget = budget >> PAGE_SHIFT;
	run->r_end = APREF_END_NONE;
	run->r_maxio = atomic_load_relaxed(&aprefetch_max_io);
	run->r_io = kmem_zalloc(run->r_maxio * sizeof(*run->r_io), KM_SLEEP);
	TAILQ_INIT(&run->r_umreqs);
	return run;
}

static void
aprefetch_run_free(struct aprefetch_run *run)
{

	if (run->r_nents > 0) {
		kmem_free(run->r_ents, run->r_nents * sizeof(*run->r_ents));
		kmem_free(run->r_files, run->r_nents * sizeof(*run->r_files));
		kmem_free(run->r_hash,
		    (run->r_hashmask + 1) * sizeof(*run->r_hash));
	}
	if (run->r_maxexts > 0)
		kmem_free(run->r_exts, run->r_maxexts * sizeof(*run->r_exts));
	kmem_free(run->r_io, run->r_maxio * sizeof(*run->r_io));
	kmem_free(run, sizeof(*run));
}

static void
aprefetch_run_load(struct aprefetch_run *run)
{
	void *buf;
	size_t len;
	int error;

	error = aprefetch_readprof(APREFETCH_BOOTPROF, &buf, &len);
	if (error == 0) {
		error = aprefetch_run_parse(run, buf, len);
		kmem_free(buf, len);
	}
	if (error == ENOENT)
		run->r_end = APREF_END_NOPROFILE;
	else if (error) {
		run->r_end = APREF_END_BADPROFILE;
		aprint_verbose("aprefetch: ignoring %s: error %d\n",
		    APREFETCH_BOOTPROF, error);
	}
}

static void
aprefetch_run_end(struct aprefetch_run *run, uint32_t msec)
{
	struct aprefetch_umreq *ur;

	aprefetch_drain(run);
	aprefetch_release(run, NULL);

	mutex_enter(&aprefetch_lock);
	/* Every vnode is released: let waiting unmounts go on. */
	while ((ur = TAILQ_FIRST(&run->r_umreqs)) != NULL) {
		TAILQ_REMOVE(&run->r_umreqs, ur, ur_entry);
		ur->ur_done = true;
	}
	aprefetch_running = NULL;
	aprefetch_st.st_last_end = run->r_end;
	aprefetch_st.st_last_msec = msec;
	if (run->r_end == APREF_END_TIMEOUT ||
	    run->r_end == APREF_END_MEMORY ||
	    run->r_end == APREF_END_DISABLED)
		aprefetch_st.st_aborts++;
	cv_broadcast(&aprefetch_cv);
	mutex_exit(&aprefetch_lock);

	aprefetch_run_free(run);
}

static void
aprefetch_run_thread(void *arg)
{
	struct aprefetch_run * const run = arg;
	struct timespec t0;

	getnanouptime(&t0);
	aprefetch_mark("replay: start");
	if (run->r_boot)
		aprefetch_run_load(run);
	if (run->r_end == APREF_END_NONE && run->r_nents > 0) {
		aprefetch_pass(run);
		while (run->r_end == APREF_END_NONE && run->r_ndeferred > 0) {
			if (aprefetch_wait_mount(run))
				aprefetch_pass(run);
		}
	}
	if (run->r_end == APREF_END_NONE)
		run->r_end = APREF_END_DONE;
	aprefetch_run_end(run, aprefetch_msec(&t0));
	aprefetch_mark("replay: end");
	kthread_exit(0);
}

static int
aprefetch_run_launch(struct aprefetch_run *run)
{
	int error;

	run->r_deadline = aprefetch_uptime_ms() +
	    (uint64_t)atomic_load_relaxed(&aprefetch_replay_timeout) * 1000;

	mutex_enter(&aprefetch_lock);
	if (aprefetch_running != NULL) {
		mutex_exit(&aprefetch_lock);
		return EBUSY;
	}
	aprefetch_running = run;
	aprefetch_st.st_replays++;
	mutex_exit(&aprefetch_lock);

	error = kthread_create(PRI_NONE, KTHREAD_MPSAFE | KTHREAD_TS, NULL,
	    aprefetch_run_thread, run, NULL, "aprefetch");
	if (error) {
		mutex_enter(&aprefetch_lock);
		aprefetch_running = NULL;
		cv_broadcast(&aprefetch_cv);
		mutex_exit(&aprefetch_lock);
	}
	return error;
}

/*
 * Hooks.
 */

/* Called by mount_domount() after a file system has been mounted. */
void
aprefetch_mount(struct mount *mp)
{

	mutex_enter(&aprefetch_lock);
	if (aprefetch_running != NULL) {
		aprefetch_running->r_mounted = true;
		cv_broadcast(&aprefetch_cv);
	}
	mutex_exit(&aprefetch_lock);
}

/*
 * Called by dounmount() before it suspends mp.  Returns once the replay
 * holds no vnode on mp and has no i/o there.
 */
void
aprefetch_unmount(struct mount *mp)
{
	struct aprefetch_umreq ur;

	ur.ur_mp = mp;
	ur.ur_done = false;
	mutex_enter(&aprefetch_lock);
	if (aprefetch_running != NULL) {
		TAILQ_INSERT_TAIL(&aprefetch_running->r_umreqs, &ur, ur_entry);
		cv_broadcast(&aprefetch_cv);
		while (!ur.ur_done)
			cv_wait(&aprefetch_cv, &aprefetch_lock);
	}
	mutex_exit(&aprefetch_lock);
}

/*
 * Called by main() once the root file system is mounted, before init
 * starts: open the boot window and start the boot replay.
 */
void
aprefetch_boot(void)
{
	struct aprefetch_run *run;
	int error;

	aprefetch_mark("kernel: root mounted");
	if (!atomic_load_relaxed(&aprefetch_enable))
		return;

	aprefetch_alloc_rings();
	mutex_enter(&aprefetch_lock);
	error = aprefetch_window_open(APREF_WIN_BOOT);
	mutex_exit(&aprefetch_lock);
	if (error)
		aprint_error("aprefetch: cannot record: error %d\n", error);

	run = aprefetch_run_alloc(aprefetch_boot_budget, true);
	error = aprefetch_run_launch(run);
	if (error) {
		aprint_error("aprefetch: cannot replay: error %d\n", error);
		aprefetch_run_free(run);
	}
}

/*
 * Device.
 */

static int
aprefetch_open(dev_t dev, int flag, int mode, struct lwp *l)
{

	return minor(dev) == 0 ? 0 : ENXIO;
}

static int
aprefetch_close(dev_t dev, int flag, int mode, struct lwp *l)
{

	return 0;
}

static int
aprefetch_read(dev_t dev, struct uio *uio, int ioflag)
{
	struct aprefetch_rec *buf;
	unsigned int max, n = 0;
	int error = 0;

	if (uio->uio_resid < sizeof(struct aprefetch_rec))
		return EINVAL;
	max = MIN(uio->uio_resid / sizeof(struct aprefetch_rec),
	    APREF_READMAX);
	buf = kmem_alloc(max * sizeof(*buf), KM_SLEEP);

	for (;;) {
		aprefetch_settle();
		mutex_enter(&aprefetch_lock);
		n = aprefetch_collect(buf, max);
		if (n > 0 || (aprefetch_window == APREF_WIN_CLOSED &&
		    !aprefetch_closing)) {
			mutex_exit(&aprefetch_lock);
			break;
		}
		if (aprefetch_closing) {
			mutex_exit(&aprefetch_lock);
			continue;
		}
		if (ioflag & IO_NDELAY) {
			mutex_exit(&aprefetch_lock);
			error = EWOULDBLOCK;
			break;
		}
		/* Recording CPUs do not wake us: look again every second. */
		error = cv_timedwait_sig(&aprefetch_cv, &aprefetch_lock, hz);
		mutex_exit(&aprefetch_lock);
		if (error == EWOULDBLOCK)
			error = 0;
		else if (error)
			break;
	}
	if (error == 0 && n > 0)
		error = uiomove(buf, n * sizeof(*buf), uio);
	kmem_free(buf, max * sizeof(*buf));
	return error;
}

/* A write stores a mark; the label is cut to fit, without its newline. */
static int
aprefetch_write(dev_t dev, struct uio *uio, int ioflag)
{
	char label[APREF_MARK_LABELLEN], junk[64];
	size_t len;
	int error;

	len = MIN(uio->uio_resid, sizeof(label) - 1);
	error = uiomove(label, len, uio);
	if (error)
		return error;
	label[len] = '\0';
	while (uio->uio_resid > 0) {
		error = uiomove(junk, MIN(uio->uio_resid, sizeof(junk)), uio);
		if (error)
			return error;
	}
	while (len > 0 && (label[len - 1] == '\n' || label[len - 1] == '\r'))
		label[--len] = '\0';
	if (len > 0)
		aprefetch_mark(label);
	return 0;
}

static int
aprefetch_getmarks(struct aprefetch_marks *am)
{
	struct aprefetch_mark *buf = NULL;
	const uint32_t room = MIN(am->am_count, APREF_NMARKS);
	uint32_t n;
	int error = 0;

	if (room > 0)
		buf = kmem_alloc(room * sizeof(*buf), KM_SLEEP);
	mutex_enter(&aprefetch_lock);
	n = MIN(room, aprefetch_nmarks);
	if (n > 0)
		memcpy(buf, aprefetch_marks, n * sizeof(*buf));
	am->am_total = aprefetch_nmarks;
	am->am_dropped = aprefetch_marks_dropped;
	mutex_exit(&aprefetch_lock);
	if (n > 0)
		error = copyout(buf, am->am_buf, n * sizeof(*buf));
	am->am_count = n;
	if (room > 0)
		kmem_free(buf, room * sizeof(*buf));
	return error;
}

struct aprefetch_bmapctx {
	struct aprefetch_extent	*bc_ext;
	uint32_t		bc_max;
	uint32_t		bc_n;
};

static void
aprefetch_bmap_add(void *arg, uint64_t dev, int64_t blkno, off_t off,
    uint32_t npages)
{
	struct aprefetch_bmapctx * const bc = arg;
	struct aprefetch_extent *ae;

	if (bc->bc_n == bc->bc_max)
		return;
	ae = &bc->bc_ext[bc->bc_n++];
	ae->ae_dev = dev;
	ae->ae_blkno = blkno;
	ae->ae_off = off;
	ae->ae_npages = npages;
	ae->ae_pad = 0;
}

static int
aprefetch_ioc_bmap(struct aprefetch_bmap *ab)
{
	struct aprefetch_bmapctx bc;
	struct vnode *vp;
	off_t size, end;
	int error;

	if ((ab->ab_off & PAGE_MASK) != 0 || ab->ab_npages == 0 ||
	    ab->ab_next == 0 || ab->ab_off > (uint64_t)INT64_MAX -
	    ((uint64_t)ab->ab_npages << PAGE_SHIFT))
		return EINVAL;
	error = aprefetch_vget(ab->ab_fsid, ab->ab_ino, ab->ab_gen, &vp, &size);
	if (error) {
		ab->ab_error = error;
		ab->ab_next = 0;
		return 0;
	}
	bc.bc_max = MIN(ab->ab_next, APREF_BMAPMAX);
	bc.bc_n = 0;
	bc.bc_ext = kmem_zalloc(bc.bc_max * sizeof(*bc.bc_ext), KM_SLEEP);
	end = MIN((off_t)(ab->ab_off + ((uint64_t)ab->ab_npages << PAGE_SHIFT)),
	    round_page(size));
	ab->ab_error = 0;
	if ((off_t)ab->ab_off < end)
		ab->ab_error = aprefetch_bmap(vp, ab->ab_off, end,
		    aprefetch_bmap_add, &bc);
	vrele(vp);
	error = copyout(bc.bc_ext, ab->ab_ext, bc.bc_n * sizeof(*bc.bc_ext));
	ab->ab_next = bc.bc_n;
	kmem_free(bc.bc_ext, bc.bc_max * sizeof(*bc.bc_ext));
	return error;
}

static int
aprefetch_ioc_replay(const struct aprefetch_replay *ar)
{
	struct aprefetch_run *run;
	void *buf;
	int error;

	if (!atomic_load_relaxed(&aprefetch_enable))
		return EPERM;
	if (ar->ar_len < sizeof(struct aprefetch_prof_hdr) ||
	    ar->ar_len > APREF_PROF_MAXSIZE)
		return EINVAL;
	buf = kmem_alloc(ar->ar_len, KM_SLEEP);
	error = copyin(ar->ar_prof, buf, ar->ar_len);
	if (error == 0) {
		run = aprefetch_run_alloc(ar->ar_budget != 0 ? ar->ar_budget :
		    aprefetch_app_budget, false);
		error = aprefetch_run_parse(run, buf, ar->ar_len);
		if (error == 0)
			error = aprefetch_run_launch(run);
		if (error)
			aprefetch_run_free(run);
	}
	kmem_free(buf, ar->ar_len);
	return error;
}

static int
aprefetch_ioctl(dev_t dev, u_long cmd, void *data, int flag, struct lwp *l)
{
	struct aprefetch_status *as;
	bool closed;
	int error = 0;

	switch (cmd) {
	case APREFIOC_STATUS:
		as = data;
		memset(as, 0, sizeof(*as));
		mutex_enter(&aprefetch_lock);
		as->as_window = aprefetch_window;
		as->as_replaying = aprefetch_running != NULL;
		if (aprefetch_window != APREF_WIN_CLOSED)
			as->as_window_msec =
			    aprefetch_msec(&aprefetch_window_start);
		mutex_exit(&aprefetch_lock);
		break;
	case APREFIOC_RECSTART:
		if (!atomic_load_relaxed(&aprefetch_enable))
			return EPERM;
		aprefetch_settle();
		aprefetch_alloc_rings();
		mutex_enter(&aprefetch_lock);
		error = aprefetch_window_open(APREF_WIN_MANUAL);
		mutex_exit(&aprefetch_lock);
		break;
	case APREFIOC_RECSTOP:
	case APREFIOC_BOOTDONE:
		mutex_enter(&aprefetch_lock);
		closed = cmd == APREFIOC_RECSTOP ?
		    aprefetch_window != APREF_WIN_CLOSED :
		    aprefetch_window == APREF_WIN_BOOT;
		if (closed)
			aprefetch_window_close();
		mutex_exit(&aprefetch_lock);
		aprefetch_settle();
		if (closed && cmd == APREFIOC_BOOTDONE)
			aprefetch_mark("boot-done");
		break;
	case APREFIOC_FLUSH:
		aprefetch_flush();
		break;
	case APREFIOC_GETMARKS:
		error = aprefetch_getmarks(data);
		break;
	case APREFIOC_BMAP:
		error = aprefetch_ioc_bmap(data);
		break;
	case APREFIOC_REPLAY:
		error = aprefetch_ioc_replay(data);
		break;
	default:
		error = ENOTTY;
		break;
	}
	return error;
}

/*
 * sysctl.
 */

static int
sysctl_aprefetch_enable(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int val = aprefetch_enable, error;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	atomic_store_relaxed(&aprefetch_enable, val != 0);
	if (val == 0) {
		/* A replay notices at its next step. */
		mutex_enter(&aprefetch_lock);
		aprefetch_window_close();
		mutex_exit(&aprefetch_lock);
		aprefetch_settle();
	}
	return 0;
}

static int
sysctl_aprefetch_int(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	int * const p = rnode->sysctl_data;
	const int max = (p == &aprefetch_max_io) ? 64 : 3600;
	int val = *p, error;

	node.sysctl_data = &val;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;
	if (val < 1 || val > max)
		return EINVAL;
	atomic_store_relaxed(p, val);
	return 0;
}

static int
sysctl_aprefetch_stats(SYSCTLFN_ARGS)
{
	struct sysctlnode node = *rnode;
	struct aprefetch_stats st;
	unsigned int i;

	mutex_enter(&aprefetch_lock);
	st = aprefetch_st;
	mutex_exit(&aprefetch_lock);
	for (i = 0; i < ncpu; i++) {
		const struct aprefetch_cpu * const ac = &aprefetch_cpus[i];

		st.st_records += ac->ac_records;
		st.st_dropped += ac->ac_dropped;
		st.st_hits += ac->ac_hits;
		st.st_misses += ac->ac_misses;
	}
	node.sysctl_data = &st;
	node.sysctl_size = sizeof(st);
	return sysctl_lookup(SYSCTLFN_CALL(&node));
}

SYSCTL_SETUP(sysctl_kern_aprefetch_setup, "sysctl kern.aprefetch setup")
{
	const struct sysctlnode *rnode;

	if (sysctl_createv(clog, 0, NULL, &rnode,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_NODE, "aprefetch",
	    SYSCTL_DESCR("Recording and prefetch of boot-time file reads"),
	    NULL, 0, NULL, 0,
	    CTL_KERN, CTL_CREATE, CTL_EOL) != 0)
		return;

	sysctl_createv(clog, 0, &rnode, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE,
	    CTLTYPE_INT, "enable",
	    SYSCTL_DESCR("Record and replay file reads"),
	    sysctl_aprefetch_enable, 0, &aprefetch_enable, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &rnode, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE,
	    CTLTYPE_QUAD, "boot_budget",
	    SYSCTL_DESCR("Bytes the boot replay may read"),
	    NULL, 0, &aprefetch_boot_budget, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &rnode, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE,
	    CTLTYPE_QUAD, "app_budget",
	    SYSCTL_DESCR("Bytes another replay may read by default"),
	    NULL, 0, &aprefetch_app_budget, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &rnode, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE,
	    CTLTYPE_INT, "max_io",
	    SYSCTL_DESCR("Read-ahead i/os a replay keeps in flight"),
	    sysctl_aprefetch_int, 0, &aprefetch_max_io, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &rnode, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE,
	    CTLTYPE_INT, "replay_timeout",
	    SYSCTL_DESCR("Seconds after which a replay stops"),
	    sysctl_aprefetch_int, 0, &aprefetch_replay_timeout, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &rnode, NULL,
	    CTLFLAG_PERMANENT | CTLFLAG_READWRITE,
	    CTLTYPE_INT, "record_timeout",
	    SYSCTL_DESCR("Seconds after which a recording window closes"),
	    sysctl_aprefetch_int, 0, &aprefetch_record_timeout, 0,
	    CTL_CREATE, CTL_EOL);
	sysctl_createv(clog, 0, &rnode, NULL,
	    CTLFLAG_PERMANENT,
	    CTLTYPE_STRUCT, "stats",
	    SYSCTL_DESCR("Statistics, struct aprefetch_stats"),
	    sysctl_aprefetch_stats, 0, NULL, sizeof(struct aprefetch_stats),
	    CTL_CREATE, CTL_EOL);
}

void
aprefetchattach(int n)
{

	mutex_init(&aprefetch_lock, MUTEX_DEFAULT, IPL_NONE);
	cv_init(&aprefetch_cv, "aprefetch");
	callout_init(&aprefetch_window_ch, CALLOUT_MPSAFE);
	if (APREFETCH_BOOT_DISABLED()) {
		aprefetch_enable = 0;
		aprint_normal("aprefetch: disabled by boot flag\n");
	}
}

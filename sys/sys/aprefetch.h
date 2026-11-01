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
 * Interface to aprefetch(4): recording file reads and replaying them
 * as sorted read-ahead.  See aprefetch(4) for the semantics.
 */

#ifndef _SYS_APREFETCH_H_
#define _SYS_APREFETCH_H_

#include <sys/types.h>
#include <sys/ioccom.h>

#define	APREFETCH_DEV		"/dev/aprefetch"
#define	APREFETCH_BOOTPROF	"/etc/aprefetch/boot.prof"
#define	APREFETCH_DBDIR		"/var/db/aprefetch"

/*
 * File system identifier as recorded: the two words of the mount's
 * f_fsidx (see statvfs(5)) packed into 64 bits.
 */
#define	APREFETCH_FSID(fsidx)						\
	((uint64_t)(uint32_t)(fsidx).__fsid_val[0] |			\
	 ((uint64_t)(uint32_t)(fsidx).__fsid_val[1] << 32))

/*
 * What an access entry or a profile entry reads.  File data goes through
 * the page cache.  FFS metadata goes through the buffer cache, which
 * finds a block by vnode and block number: directories and symbolic
 * links by their own vnode, inodes and the rest by the device's.
 */
#define	APREF_KIND_DATA		0x00	/* pages of a regular file */
#define	APREF_KIND_VBLK		0x10	/* block of a directory or link */
#define	APREF_KIND_DBLK		0x20	/* block of the device */

/*
 * Access entry, returned by read(2): one or more consecutive reads of a
 * file by one process, or one read of a metadata block.
 *
 * For APREF_KIND_DATA, ar_off is the offset in the file and ar_npages
 * the length.  For APREF_KIND_VBLK, ar_off is the logical block number.
 * For APREF_KIND_DBLK, ar_off is the DEV_BSIZE block number on the
 * device, and ar_ino the identity of the file system, which newfs(8)
 * changes (FFS fs_id).  Metadata entries have the block size in ar_size
 * and the pages it covers in ar_npages.
 */
struct aprefetch_rec {
	uint64_t	ar_fsid;	/* file system, APREFETCH_FSID() */
	uint64_t	ar_ino;		/* inode number */
	uint64_t	ar_off;		/* offset in the file, page aligned */
	uint32_t	ar_gen;		/* inode generation number */
	uint32_t	ar_npages;	/* length in pages */
	uint32_t	ar_pid;		/* process that read */
	uint32_t	ar_msec;	/* msec since the window opened */
	uint32_t	ar_flags;	/* APREF_REC_*, APREF_KIND_* */
	uint32_t	ar_size;	/* metadata: block size in bytes */
};

#define	APREF_REC_MISS	0x00000001	/* the data was not in memory */
#define	APREF_REC_KIND(flags)	((flags) & 0x30)

/*
 * Mark: a timestamped label written with write(2), or by the kernel.
 * Marks also snapshot the total reads of all disks.
 */
#define	APREF_MARK_LABELLEN	48

struct aprefetch_mark {
	uint64_t	am_nsec;	/* nanoseconds since boot */
	uint64_t	am_rxfer;	/* disk read transfers so far */
	uint64_t	am_rbytes;	/* disk bytes read so far */
	char		am_label[APREF_MARK_LABELLEN];
};

/*
 * Profile file: a header followed by ph_nentries entries, in the order
 * in which the data was first read.  ph_crc is the crc32 (as in zlib)
 * of the header with ph_crc set to zero, followed by the entries.
 * Version 1 had file data only, in entries without pe_kind and pe_size.
 */
#define	APREF_PROF_MAGIC	0x46525041	/* "APRF" */
#define	APREF_PROF_VERSION	2
#define	APREF_PROF_MAXFS	8
#define	APREF_PROF_MAXSIZE	(16 * 1024 * 1024)

struct aprefetch_prof_hdr {
	uint32_t	ph_magic;
	uint32_t	ph_version;
	uint64_t	ph_ctime;	/* creation time, seconds since epoch */
	uint32_t	ph_nentries;
	uint32_t	ph_npages;	/* pages over all entries */
	uint32_t	ph_nfs;		/* used slots in ph_fsid */
	uint32_t	ph_crc;
	uint64_t	ph_fsid[APREF_PROF_MAXFS];
};

/* The fields are those of struct aprefetch_rec. */
struct aprefetch_prof_ent {
	uint64_t	pe_fsid;
	uint64_t	pe_ino;
	uint64_t	pe_off;
	uint32_t	pe_gen;
	uint32_t	pe_npages;
	uint32_t	pe_kind;	/* APREF_KIND_* */
	uint32_t	pe_size;
};

/*
 * Trace file, written by aprefd(8): a header followed by th_nrecs
 * access entries as read from the device.  Version 1 traces have file
 * data only and read as version 2.
 */
#define	APREF_TRACE_MAGIC	0x52545041	/* "APTR" */
#define	APREF_TRACE_VERSION	2

struct aprefetch_trace_hdr {
	uint32_t	th_magic;
	uint32_t	th_version;
	uint64_t	th_ctime;	/* time the window closed */
	uint32_t	th_nrecs;
	uint32_t	th_recsize;	/* sizeof(struct aprefetch_rec) */
	uint64_t	th_dropped;	/* entries the kernel dropped */
};

/* Window and replay states, in struct aprefetch_status. */
#define	APREF_WIN_CLOSED	0
#define	APREF_WIN_BOOT		1	/* opened at boot */
#define	APREF_WIN_MANUAL	2	/* opened with APREFIOC_RECSTART */

/* How the last replay ended, in struct aprefetch_stats. */
#define	APREF_END_NONE		0	/* no replay yet */
#define	APREF_END_DONE		1	/* everything was issued */
#define	APREF_END_BUDGET	2	/* the budget was used up */
#define	APREF_END_TIMEOUT	3
#define	APREF_END_MEMORY	4	/* free memory below the threshold */
#define	APREF_END_DISABLED	5	/* kern.aprefetch.enable was cleared */
#define	APREF_END_NOPROFILE	6	/* no boot profile */
#define	APREF_END_BADPROFILE	7	/* the profile failed validation */

struct aprefetch_status {
	int32_t		as_window;	/* APREF_WIN_* */
	int32_t		as_replaying;	/* a replay is running */
	uint64_t	as_window_msec;	/* msec since the window opened */
};

/* kern.aprefetch.stats */
struct aprefetch_stats {
	uint64_t	st_records;	/* access entries recorded */
	uint64_t	st_dropped;	/* entries dropped, ring full */
	uint64_t	st_hits;	/* recorded data reads served from memory */
	uint64_t	st_misses;	/* recorded data reads that were not */
	uint64_t	st_replays;	/* replays started */
	uint64_t	st_entries;	/* profile entries taken */
	uint64_t	st_stale;	/* entries dropped: gone, regenerated, shrunk */
	uint64_t	st_deferred;	/* entries deferred until their mount */
	uint64_t	st_ios;		/* read-ahead i/os issued */
	uint64_t	st_pages;	/* pages charged to the budget */
	uint64_t	st_cached;	/* pages skipped: resident or busy */
	uint64_t	st_errors;	/* pages lost to i/o errors */
	uint64_t	st_aborts;	/* replays stopped early */
	uint32_t	st_last_end;	/* APREF_END_* of the last replay */
	uint32_t	st_last_msec;	/* duration of the last replay */
	uint64_t	st_meta_hits;	/* recorded metadata reads from memory */
	uint64_t	st_meta_misses;	/* recorded metadata reads that were not */
	uint64_t	st_meta_ios;	/* metadata blocks read ahead */
	uint64_t	st_meta_cached;	/* metadata blocks skipped: in memory */
};

struct aprefetch_marks {
	struct aprefetch_mark *am_buf;	/* user buffer */
	uint32_t	am_count;	/* in: size of am_buf; out: marks copied */
	uint32_t	am_total;	/* out: marks stored */
	uint64_t	am_dropped;	/* out: marks lost, table full */
};

/* One physically contiguous piece of a file range. */
struct aprefetch_extent {
	uint64_t	ae_dev;		/* disk, dev_t */
	int64_t		ae_blkno;	/* DEV_BSIZE block, -1 for a hole */
	uint64_t	ae_off;		/* offset in the file */
	uint32_t	ae_npages;
	uint32_t	ae_pad;
};

struct aprefetch_bmap {
	uint64_t	ab_fsid;
	uint64_t	ab_ino;
	uint64_t	ab_off;		/* page aligned */
	uint32_t	ab_gen;
	uint32_t	ab_npages;
	struct aprefetch_extent *ab_ext; /* user buffer */
	uint32_t	ab_next;	/* in: size of ab_ext; out: extents */
	int32_t		ab_error;	/* out: why the file was not mapped */
};

struct aprefetch_replay {
	const void	*ar_prof;	/* profile image: header and entries */
	size_t		ar_len;
	uint64_t	ar_budget;	/* bytes, 0 for kern.aprefetch.app_budget */
};

#define	APREFIOC_STATUS		_IOR('y', 0, struct aprefetch_status)
#define	APREFIOC_RECSTART	_IO('y', 1)
#define	APREFIOC_RECSTOP	_IO('y', 2)
#define	APREFIOC_BOOTDONE	_IO('y', 3)
#define	APREFIOC_FLUSH		_IO('y', 4)
#define	APREFIOC_GETMARKS	_IOWR('y', 5, struct aprefetch_marks)
#define	APREFIOC_BMAP		_IOWR('y', 6, struct aprefetch_bmap)
#define	APREFIOC_REPLAY		_IOW('y', 7, struct aprefetch_replay)

#ifdef _KERNEL
struct buf;
struct mount;
struct vnode;

extern int	aprefetch_recording;

void	aprefetch_getpages(struct vnode *, off_t, int, int, int, bool, bool);
void	aprefetch_bread(struct vnode *, daddr_t, int, bool);
bool	aprefetch_bio_read(struct vnode *, daddr_t, int,	/* vfs_bio.c */
	    void (*)(struct buf *));
void	aprefetch_boot(void);
void	aprefetch_kmark(const char *);
void	aprefetch_mount(struct mount *);
void	aprefetch_unmount(struct mount *);
#endif /* _KERNEL */

#endif /* !_SYS_APREFETCH_H_ */

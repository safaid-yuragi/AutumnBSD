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
 * aprefd: collect the access entries of the boot recording window from
 * /dev/aprefetch, save them as a trace, and rebuild the boot profile
 * from the last few traces.
 */

#include <sys/cdefs.h>
__RCSID("$NetBSD$");

#include <sys/param.h>
#include <sys/sysctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <util.h>

#include "aprefio.h"

#define	MAXKEEP		9		/* traces kept at most */
#define	MAXPAGES	(1024 * 1024)	/* distinct pages in a profile */
#define	MAXBLOCKS	(256 * 1024)	/* distinct metadata blocks */

/* A page seen in the traces. */
struct page {
	uint64_t	pg_fsid;
	uint64_t	pg_ino;
	uint32_t	pg_gen;
	uint32_t	pg_pgno;
	uint32_t	pg_msec;	/* first read, in the newest trace */
	uint8_t		pg_count;	/* traces that have it */
	int8_t		pg_last;	/* last trace that counted it */
	bool		pg_used;
};

/* A metadata block seen in the traces; the rest is as for a page. */
struct mblk {
	uint64_t	mb_fsid;
	uint64_t	mb_ino;
	uint64_t	mb_off;		/* block number */
	uint32_t	mb_gen;
	uint32_t	mb_kind;	/* APREF_KIND_* */
	uint32_t	mb_size;	/* in the newest trace that has it */
	uint32_t	mb_msec;
	uint8_t		mb_count;
	int8_t		mb_last;
	bool		mb_used;
};

struct fscount {
	uint64_t	fs_fsid;
	uint64_t	fs_pages;
};

/* A profile entry to be: a run of pages of one file, or a metadata block. */
struct run {
	uint64_t	r_fsid;
	uint64_t	r_ino;
	uint64_t	r_off;		/* first page, or block number */
	uint32_t	r_gen;
	uint32_t	r_npages;
	uint32_t	r_msec;
	uint32_t	r_kind;
	uint32_t	r_size;
};

static bool		dflag;
static bool		Mflag;		/* no metadata in the profile */
static volatile sig_atomic_t quit;

static struct page	*table;
static size_t		tablemask;
static size_t		npages;
static bool		full;

static struct mblk	*mtable;
static size_t		mtablemask;
static size_t		nblocks;
static bool		mfull;

__dead static void	usage(void);
static void		logmsg(int, const char *, ...) __printflike(2, 3);

static void
logmsg(int pri, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (dflag) {
		(void)vfprintf(stderr, fmt, ap);
		(void)fputc('\n', stderr);
	} else
		vsyslog(pri, fmt, ap);
	va_end(ap);
}

static void
onsignal(int sig)
{

	quit = 1;
}

/* Read access entries until the window closes and the device says EOF. */
static int
collect(int fd, struct aprefetch_rec **recsp, uint32_t *np)
{
	struct aprefetch_rec buf[256], *recs = NULL, *nrecs;
	size_t n = 0, max = 0, got, lost = 0;
	ssize_t len;

	for (;;) {
		if ((len = read(fd, buf, sizeof(buf))) == -1) {
			if (errno == EINTR && !quit)
				continue;
			if (errno != EINTR)
				logmsg(LOG_ERR, "read %s: %s", APREFETCH_DEV,
				    strerror(errno));
			free(recs);
			return -1;
		}
		if (len == 0)
			break;
		got = (size_t)len / sizeof(buf[0]);
		if (n + got > APREF_TRACEMAX) {
			lost += got;
			continue;
		}
		if (n + got > max) {
			max = MAX(max, 4096);
			while (max < n + got)
				max *= 2;
			max = MIN(max, APREF_TRACEMAX);
			if ((nrecs = realloc(recs, max * sizeof(*recs))) == NULL) {
				logmsg(LOG_ERR, "out of memory");
				free(recs);
				return -1;
			}
			recs = nrecs;
		}
		memcpy(&recs[n], buf, got * sizeof(buf[0]));
		n += got;
	}
	if (lost > 0)
		logmsg(LOG_WARNING, "dropped %zu entries past %d", lost,
		    APREF_TRACEMAX);
	*recsp = recs;
	*np = (uint32_t)n;
	return 0;
}

static void
tracename(char *buf, size_t len, int i)
{

	if (i == 0)
		(void)snprintf(buf, len, "%s", APREF_TRACEFILE);
	else
		(void)snprintf(buf, len, "%s.%d", APREF_TRACEFILE, i);
}

/* Make room for a new trace: boot.trace becomes boot.trace.1, and so on. */
static void
rotate(int keep)
{
	char from[PATH_MAX], to[PATH_MAX];
	int i;

	for (i = MAXKEEP; i >= keep; i--) {
		tracename(from, sizeof(from), i);
		(void)unlink(from);
	}
	for (i = keep - 1; i >= 1; i--) {
		tracename(from, sizeof(from), i - 1);
		tracename(to, sizeof(to), i);
		if (rename(from, to) == -1 && errno != ENOENT)
			logmsg(LOG_WARNING, "rename %s: %s", from,
			    strerror(errno));
	}
}

static size_t
pagehash(uint64_t fsid, uint64_t ino, uint32_t gen, uint32_t pgno)
{
	uint64_t x;

	x = (ino ^ ((uint64_t)gen << 32) ^ (fsid * 0x9e3779b97f4a7c15ULL)) *
	    0xff51afd7ed558ccdULL;
	x ^= (uint64_t)pgno * 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return (size_t)x & tablemask;
}

/* Count a page read in trace t at msec. */
static void
addpage(int t, uint64_t fsid, uint64_t ino, uint32_t gen, uint32_t pgno,
    uint32_t msec)
{
	struct page *pg;
	size_t h;

	for (h = pagehash(fsid, ino, gen, pgno);; h = (h + 1) & tablemask) {
		pg = &table[h];
		if (!pg->pg_used)
			break;
		if (pg->pg_fsid == fsid && pg->pg_ino == ino &&
		    pg->pg_gen == gen && pg->pg_pgno == pgno) {
			if (pg->pg_last != t) {
				pg->pg_last = (int8_t)t;
				pg->pg_count++;
			} else if (pg->pg_count == 1 && msec < pg->pg_msec) {
				/* Still in the newest trace that has it. */
				pg->pg_msec = msec;
			}
			return;
		}
	}
	if (npages >= MAXPAGES) {
		full = true;
		return;
	}
	pg->pg_used = true;
	pg->pg_fsid = fsid;
	pg->pg_ino = ino;
	pg->pg_gen = gen;
	pg->pg_pgno = pgno;
	pg->pg_msec = msec;
	pg->pg_count = 1;
	pg->pg_last = (int8_t)t;
	npages++;
}

static size_t
blockhash(uint32_t kind, uint64_t fsid, uint64_t ino, uint32_t gen,
    uint64_t off)
{
	uint64_t x;

	x = (ino ^ ((uint64_t)gen << 32) ^ (fsid * 0x9e3779b97f4a7c15ULL) ^
	    kind) * 0xff51afd7ed558ccdULL;
	x ^= off * 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return (size_t)x & mtablemask;
}

/* Count a metadata block read in trace t, as addpage() does. */
static void
addblock(int t, const struct aprefetch_rec *r)
{
	const uint32_t kind = APREF_REC_KIND(r->ar_flags);
	struct mblk *mb;
	size_t h;

	for (h = blockhash(kind, r->ar_fsid, r->ar_ino, r->ar_gen, r->ar_off);;
	    h = (h + 1) & mtablemask) {
		mb = &mtable[h];
		if (!mb->mb_used)
			break;
		if (mb->mb_kind == kind && mb->mb_fsid == r->ar_fsid &&
		    mb->mb_ino == r->ar_ino && mb->mb_gen == r->ar_gen &&
		    mb->mb_off == r->ar_off) {
			if (mb->mb_last != t) {
				mb->mb_last = (int8_t)t;
				mb->mb_count++;
			} else if (mb->mb_count == 1 &&
			    r->ar_msec < mb->mb_msec) {
				mb->mb_msec = r->ar_msec;
			}
			return;
		}
	}
	if (nblocks >= MAXBLOCKS) {
		mfull = true;
		return;
	}
	mb->mb_used = true;
	mb->mb_fsid = r->ar_fsid;
	mb->mb_ino = r->ar_ino;
	mb->mb_off = r->ar_off;
	mb->mb_gen = r->ar_gen;
	mb->mb_kind = kind;
	mb->mb_size = r->ar_size;
	mb->mb_msec = r->ar_msec;
	mb->mb_count = 1;
	mb->mb_last = (int8_t)t;
	nblocks++;
}

static int
page_cmp(const void *a, const void *b)
{
	const struct page *p = a, *q = b;

	if (p->pg_fsid != q->pg_fsid)
		return p->pg_fsid < q->pg_fsid ? -1 : 1;
	if (p->pg_ino != q->pg_ino)
		return p->pg_ino < q->pg_ino ? -1 : 1;
	if (p->pg_gen != q->pg_gen)
		return p->pg_gen < q->pg_gen ? -1 : 1;
	if (p->pg_pgno != q->pg_pgno)
		return p->pg_pgno < q->pg_pgno ? -1 : 1;
	return 0;
}

static int
run_cmp(const void *a, const void *b)
{
	const struct run *r = a, *s = b;

	if (r->r_msec != s->r_msec)
		return r->r_msec < s->r_msec ? -1 : 1;
	if (r->r_kind != s->r_kind)
		return r->r_kind < s->r_kind ? -1 : 1;
	if (r->r_fsid != s->r_fsid)
		return r->r_fsid < s->r_fsid ? -1 : 1;
	if (r->r_ino != s->r_ino)
		return r->r_ino < s->r_ino ? -1 : 1;
	return r->r_off < s->r_off ? -1 : r->r_off > s->r_off;
}

static int
fs_cmp(const void *a, const void *b)
{
	const struct fscount *f = a, *g = b;

	return f->fs_pages > g->fs_pages ? -1 : f->fs_pages < g->fs_pages;
}

/* A hash table with room for twice want slots, a power of two. */
static void *
newtable(size_t want, size_t size, size_t *maskp)
{
	size_t n;
	void *p;

	for (n = 1024; n < 2 * want; n <<= 1)
		continue;
	if ((p = calloc(n, size)) == NULL)
		return NULL;
	*maskp = n - 1;
	return p;
}

/*
 * Rebuild the boot profile from the newest keep traces: keep the pages
 * and metadata blocks read in at least min of them, in the order in
 * which the newest trace that has them read them first.
 */
static int
buildprof(int keep, int min)
{
	struct aprefio_trace tr;
	struct aprefetch_prof_ent *pe;
	struct page *pg;
	struct mblk *mb;
	struct run *runs, *r;
	struct fscount fsc[64];
	char path[PATH_MAX];
	uint64_t fsid[APREF_PROF_MAXFS];
	uint64_t total = 0, mtotal = 0;
	size_t i, j, n, nmeta, nruns, nent, maxent, nfsc = 0, nmetaent = 0;
	uint32_t k, p;
	int t, ntraces = 0, need, nfs = 0, error;
	const long pgsz = sysconf(_SC_PAGESIZE);
	int pgshift;

	for (pgshift = 0; (1L << pgshift) < pgsz; pgshift++)
		continue;

	/* Size the tables for the newest trace, with room for the others. */
	for (t = 0; t < keep; t++) {
		tracename(path, sizeof(path), t);
		if (aprefio_load_trace(path, &tr) == -1)
			continue;
		for (k = 0; k < tr.t_hdr.th_nrecs; k++) {
			if (APREF_REC_KIND(tr.t_recs[k].ar_flags) ==
			    APREF_KIND_DATA)
				total += tr.t_recs[k].ar_npages;
			else
				mtotal++;
		}
		aprefio_free_trace(&tr);
	}
	table = newtable(MIN(total, MAXPAGES), sizeof(*table), &tablemask);
	mtable = newtable(MIN(mtotal, MAXBLOCKS), sizeof(*mtable),
	    &mtablemask);
	if (table == NULL || mtable == NULL) {
		logmsg(LOG_ERR, "out of memory");
		free(table);
		free(mtable);
		return -1;
	}

	for (t = 0; t < keep; t++) {
		tracename(path, sizeof(path), t);
		if (aprefio_load_trace(path, &tr) == -1) {
			if (errno != ENOENT)
				logmsg(LOG_WARNING, "%s: %s", path,
				    strerror(errno));
			continue;
		}
		ntraces++;
		for (k = 0; k < tr.t_hdr.th_nrecs; k++) {
			const struct aprefetch_rec * const rec = &tr.t_recs[k];
			uint64_t first;

			if (APREF_REC_KIND(rec->ar_flags) != APREF_KIND_DATA) {
				if (!Mflag && rec->ar_size != 0 &&
				    rec->ar_size <= MAXBSIZE)
					addblock(t, rec);
				continue;
			}
			first = rec->ar_off >> pgshift;
			if (first + rec->ar_npages > UINT32_MAX)
				continue;
			for (p = 0; p < rec->ar_npages; p++)
				addpage(t, rec->ar_fsid, rec->ar_ino,
				    rec->ar_gen, (uint32_t)(first + p),
				    rec->ar_msec);
		}
		aprefio_free_trace(&tr);
	}
	if (full)
		logmsg(LOG_WARNING, "more than %d distinct pages; ignored "
		    "the rest", MAXPAGES);
	if (mfull)
		logmsg(LOG_WARNING, "more than %d distinct metadata blocks; "
		    "ignored the rest", MAXBLOCKS);
	need = MIN(min, ntraces);

	/* The pages and blocks read often enough. */
	for (i = j = 0; i <= tablemask; i++) {
		if (table[i].pg_used && table[i].pg_count >= need)
			table[j++] = table[i];
	}
	n = j;
	qsort(table, n, sizeof(*table), page_cmp);
	for (i = j = 0; i <= mtablemask; i++) {
		if (mtable[i].mb_used && mtable[i].mb_count >= need)
			mtable[j++] = mtable[i];
	}
	nmeta = j;
	if ((runs = calloc(MAX(n + nmeta, 1), sizeof(*runs))) == NULL) {
		logmsg(LOG_ERR, "out of memory");
		free(table);
		free(mtable);
		return -1;
	}

	/* Pages grouped into runs per file, and the blocks as they are. */
	for (i = 0, nruns = 0; i < n; i++) {
		pg = &table[i];
		if (nruns > 0) {
			r = &runs[nruns - 1];
			if (r->r_fsid == pg->pg_fsid && r->r_ino == pg->pg_ino &&
			    r->r_gen == pg->pg_gen &&
			    r->r_off + r->r_npages == pg->pg_pgno) {
				r->r_npages++;
				r->r_msec = MIN(r->r_msec, pg->pg_msec);
				continue;
			}
		}
		r = &runs[nruns++];
		r->r_fsid = pg->pg_fsid;
		r->r_ino = pg->pg_ino;
		r->r_gen = pg->pg_gen;
		r->r_off = pg->pg_pgno;
		r->r_npages = 1;
		r->r_msec = pg->pg_msec;
		r->r_kind = APREF_KIND_DATA;
		r->r_size = 0;
	}
	for (i = 0; i < nmeta; i++) {
		mb = &mtable[i];
		r = &runs[nruns++];
		r->r_fsid = mb->mb_fsid;
		r->r_ino = mb->mb_ino;
		r->r_gen = mb->mb_gen;
		r->r_off = mb->mb_off;
		r->r_npages = (uint32_t)howmany(mb->mb_size, (uint32_t)pgsz);
		r->r_msec = mb->mb_msec;
		r->r_kind = mb->mb_kind;
		r->r_size = mb->mb_size;
	}
	free(table);
	table = NULL;
	free(mtable);
	mtable = NULL;
	qsort(runs, nruns, sizeof(*runs), run_cmp);

	/* At most APREF_PROF_MAXFS file systems, those with the most pages. */
	for (i = 0; i < nruns; i++) {
		for (j = 0; j < nfsc; j++)
			if (fsc[j].fs_fsid == runs[i].r_fsid)
				break;
		if (j == nfsc) {
			if (nfsc == __arraycount(fsc))
				continue;
			fsc[nfsc].fs_fsid = runs[i].r_fsid;
			fsc[nfsc].fs_pages = 0;
			nfsc++;
		}
		fsc[j].fs_pages += runs[i].r_npages;
	}
	qsort(fsc, nfsc, sizeof(fsc[0]), fs_cmp);
	nfs = (int)MIN(nfsc, APREF_PROF_MAXFS);
	for (j = 0; j < (size_t)nfs; j++)
		fsid[j] = fsc[j].fs_fsid;

	maxent = (APREF_PROF_MAXSIZE - sizeof(struct aprefetch_prof_hdr)) /
	    sizeof(*pe);
	if ((pe = calloc(MAX(MIN(nruns, maxent), 1), sizeof(*pe))) == NULL) {
		logmsg(LOG_ERR, "out of memory");
		free(runs);
		return -1;
	}
	for (i = nent = 0; i < nruns && nent < maxent; i++) {
		r = &runs[i];
		for (j = 0; j < (size_t)nfs; j++)
			if (fsid[j] == r->r_fsid)
				break;
		if (j == (size_t)nfs)
			continue;
		pe[nent].pe_fsid = r->r_fsid;
		pe[nent].pe_ino = r->r_ino;
		pe[nent].pe_gen = r->r_gen;
		pe[nent].pe_off = r->r_kind == APREF_KIND_DATA ?
		    r->r_off << pgshift : r->r_off;
		pe[nent].pe_npages = r->r_npages;
		pe[nent].pe_kind = r->r_kind;
		pe[nent].pe_size = r->r_size;
		if (r->r_kind != APREF_KIND_DATA)
			nmetaent++;
		nent++;
	}
	free(runs);

	error = aprefio_save_prof(APREFETCH_BOOTPROF, pe, (uint32_t)nent, fsid,
	    (uint32_t)nfs);
	if (error == -1)
		logmsg(LOG_ERR, "%s: %s", APREFETCH_BOOTPROF, strerror(errno));
	else
		logmsg(LOG_INFO, "%s: %zu entries, %zu of metadata, from %d "
		    "traces", APREFETCH_BOOTPROF, nent, nmetaent, ntraces);
	free(pe);
	return error;
}

static uint64_t
dropped(void)
{
	struct aprefetch_stats st;
	size_t len = sizeof(st);

	if (sysctlbyname("kern.aprefetch.stats", &st, &len, NULL, 0) == -1)
		return 0;
	return st.st_dropped;
}

__dead static void
usage(void)
{

	(void)fprintf(stderr, "usage: %s [-dMn] [-k keep] [-m min]\n",
	    getprogname());
	exit(1);
}

int
main(int argc, char **argv)
{
	struct sigaction sa;
	struct aprefetch_rec *recs = NULL;
	uint32_t nrecs = 0;
	bool nflag = false;
	int ch, fd, keep = 3, min = 2, e;

	while ((ch = getopt(argc, argv, "dk:Mm:n")) != -1) {
		switch (ch) {
		case 'd':
			dflag = true;
			break;
		case 'k':
			keep = (int)strtoi(optarg, NULL, 10, 1, MAXKEEP, &e);
			if (e)
				errx(1, "-k: number of traces from 1 to %d",
				    MAXKEEP);
			break;
		case 'm':
			min = (int)strtoi(optarg, NULL, 10, 1, MAXKEEP, &e);
			if (e)
				errx(1, "-m: number of traces from 1 to %d",
				    MAXKEEP);
			break;
		case 'M':
			Mflag = true;
			break;
		case 'n':
			nflag = true;
			break;
		default:
			usage();
		}
	}
	if (argc != optind)
		usage();
	if (min > keep)
		errx(1, "-m %d is more than -k %d", min, keep);

	if ((fd = open(APREFETCH_DEV, O_RDONLY)) == -1)
		err(1, "%s", APREFETCH_DEV);
	if (!dflag) {
		openlog(getprogname(), LOG_PID, LOG_DAEMON);
		if (daemon(0, 0) == -1)
			err(1, "daemon");
		(void)pidfile(NULL);
	}

	/* No SA_RESTART: a signal ends a read(2) that waits for entries. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = onsignal;
	(void)sigemptyset(&sa.sa_mask);
	(void)sigaction(SIGTERM, &sa, NULL);
	(void)sigaction(SIGINT, &sa, NULL);
	(void)sigaction(SIGHUP, &sa, NULL);

	if (collect(fd, &recs, &nrecs) == -1)
		return quit ? 0 : 1;
	(void)close(fd);
	if (nrecs == 0) {
		logmsg(LOG_INFO, "nothing was recorded");
		return 0;
	}

	rotate(keep);
	if (aprefio_save_trace(APREF_TRACEFILE, recs, nrecs, dropped()) == -1) {
		logmsg(LOG_ERR, "%s: %s", APREF_TRACEFILE, strerror(errno));
		return 1;
	}
	logmsg(LOG_INFO, "%s: %" PRIu32 " entries", APREF_TRACEFILE, nrecs);
	free(recs);

	if (!nflag && buildprof(keep, min) == -1)
		return 1;
	return 0;
}

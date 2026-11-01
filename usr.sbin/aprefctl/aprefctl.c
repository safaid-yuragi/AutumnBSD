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

/* aprefctl: control aprefetch(4) and look at what it recorded. */

#include <sys/cdefs.h>
__RCSID("$NetBSD$");

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <util.h>

#include "aprefio.h"

#define	MB(bytes)	((double)(bytes) / (1024 * 1024))
#define	MAXMARKS	4096
#define	MAXEXT		256		/* extents per APREFIOC_BMAP */
#define	PLOTW		72
#define	PLOTH		20

struct cmd {
	const char	*c_name;
	int		(*c_func)(int, char **);
	const char	*c_args;
};

/* A piece of the disk read by a recorded entry. */
struct point {
	uint64_t	pt_dev;
	int64_t		pt_blkno;
	uint32_t	pt_nblks;	/* DEV_BSIZE blocks */
	uint32_t	pt_msec;
	const struct aprefetch_rec *pt_rec;
};

static int	cmd_status(int, char **);
static int	cmd_record(int, char **);
static int	cmd_bootdone(int, char **);
static int	cmd_mark(int, char **);
static int	cmd_marks(int, char **);
static int	cmd_list(int, char **);
static int	cmd_dump(int, char **);
static int	cmd_delete(int, char **);
static int	cmd_trace(int, char **);
static int	cmd_scatter(int, char **);
static int	cmd_replay(int, char **);

static const struct cmd cmds[] = {
	{ "status",	cmd_status,	"" },
	{ "record",	cmd_record,	"start | stop" },
	{ "boot-done",	cmd_bootdone,	"" },
	{ "mark",	cmd_mark,	"label ..." },
	{ "marks",	cmd_marks,	"[-s]" },
	{ "list",	cmd_list,	"" },
	{ "dump",	cmd_dump,	"boot | file" },
	{ "delete",	cmd_delete,	"boot | file" },
	{ "trace",	cmd_trace,	"[file]" },
	{ "scatter",	cmd_scatter,	"[-a] [-c csvfile] [-s svgfile] [file]" },
	{ "replay",	cmd_replay,	"[-b bytes] boot | file" },
};

static long	pagesize;

__dead static void
usage(void)
{
	size_t i;

	for (i = 0; i < __arraycount(cmds); i++)
		(void)fprintf(stderr, "%s %s %s%s%s\n",
		    i == 0 ? "usage:" : "      ", getprogname(),
		    cmds[i].c_name, cmds[i].c_args[0] ? " " : "",
		    cmds[i].c_args);
	exit(1);
}

static int
devopen(int flags)
{
	int fd;

	if ((fd = open(APREFETCH_DEV, flags)) == -1)
		err(1, "%s", APREFETCH_DEV);
	return fd;
}

static const char *
profpath(const char *name)
{

	return strcmp(name, "boot") == 0 ? APREFETCH_BOOTPROF : name;
}

static void
getsysctl(const char *name, void *p, size_t len)
{
	size_t l = len;

	if (sysctlbyname(name, p, &l, NULL, 0) == -1)
		err(1, "%s", name);
}

static const char *
endname(uint32_t end)
{
	static const char *const names[] = {
		[APREF_END_NONE] = "none yet",
		[APREF_END_DONE] = "done",
		[APREF_END_BUDGET] = "budget used up",
		[APREF_END_TIMEOUT] = "timed out",
		[APREF_END_MEMORY] = "stopped, low on memory",
		[APREF_END_DISABLED] = "stopped, disabled",
		[APREF_END_NOPROFILE] = "no profile",
		[APREF_END_BADPROFILE] = "profile rejected",
	};

	return end < __arraycount(names) && names[end] != NULL ?
	    names[end] : "?";
}

static const char *
timestr(uint64_t t)
{
	static char buf[32];
	const time_t tt = (time_t)t;
	struct tm *tm;

	if ((tm = localtime(&tt)) == NULL ||
	    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm) == 0)
		(void)snprintf(buf, sizeof(buf), "%" PRIu64, t);
	return buf;
}

static const char *
kindname(uint32_t kind)
{

	switch (kind) {
	case APREF_KIND_DATA:
		return "data";
	case APREF_KIND_VBLK:
		return "dir";
	case APREF_KIND_DBLK:
		return "dev";
	default:
		return "?";
	}
}

static int
cmd_status(int argc, char **argv)
{
	struct aprefetch_status as;
	struct aprefetch_stats st;
	int64_t bootb, appb;
	int enable, maxio, rto, wto, fd;

	if (argc != 1)
		usage();
	getsysctl("kern.aprefetch.enable", &enable, sizeof(enable));
	getsysctl("kern.aprefetch.boot_budget", &bootb, sizeof(bootb));
	getsysctl("kern.aprefetch.app_budget", &appb, sizeof(appb));
	getsysctl("kern.aprefetch.max_io", &maxio, sizeof(maxio));
	getsysctl("kern.aprefetch.replay_timeout", &rto, sizeof(rto));
	getsysctl("kern.aprefetch.record_timeout", &wto, sizeof(wto));
	getsysctl("kern.aprefetch.stats", &st, sizeof(st));
	fd = devopen(O_RDONLY);
	if (ioctl(fd, APREFIOC_STATUS, &as) == -1)
		err(1, "APREFIOC_STATUS");
	(void)close(fd);

	printf("enabled:     %s\n", enable ? "yes" : "no");
	if (as.as_window == APREF_WIN_CLOSED)
		printf("recording:   no\n");
	else
		printf("recording:   %s window, open for %.1f s\n",
		    as.as_window == APREF_WIN_BOOT ? "boot" : "manual",
		    as.as_window_msec / 1000.0);
	printf("replay:      %s\n", as.as_replaying ? "running" : "idle");
	printf("budgets:     boot %.0f MB, other %.0f MB\n", MB(bootb),
	    MB(appb));
	printf("limits:      %d i/os in flight, replay %d s, recording %d s\n",
	    maxio, rto, wto);
	printf("recorded:    %" PRIu64 " entries, %" PRIu64 " dropped; "
	    "%" PRIu64 " reads from memory, %" PRIu64 " not\n",
	    st.st_records, st.st_dropped, st.st_hits, st.st_misses);
	printf("replays:     %" PRIu64 "; last: %s, %" PRIu32 " ms\n",
	    st.st_replays, endname(st.st_last_end), st.st_last_msec);
	printf("entries:     %" PRIu64 " taken, %" PRIu64 " stale, "
	    "%" PRIu64 " deferred\n", st.st_entries, st.st_stale,
	    st.st_deferred);
	printf("read-ahead:  %" PRIu64 " i/os, %" PRIu64 " pages "
	    "(%.1f MB); %" PRIu64 " pages already in memory, "
	    "%" PRIu64 " lost to errors\n", st.st_ios, st.st_pages,
	    MB(st.st_pages * (uint64_t)pagesize), st.st_cached, st.st_errors);
	printf("metadata:    %" PRIu64 " reads from memory, %" PRIu64 " not; "
	    "%" PRIu64 " blocks read ahead, %" PRIu64 " in memory already\n",
	    st.st_meta_hits, st.st_meta_misses, st.st_meta_ios,
	    st.st_meta_cached);
	printf("aborted:     %" PRIu64 "\n", st.st_aborts);
	return 0;
}

static int
cmd_record(int argc, char **argv)
{
	u_long cmd;
	int fd;

	if (argc != 2)
		usage();
	if (strcmp(argv[1], "start") == 0)
		cmd = APREFIOC_RECSTART;
	else if (strcmp(argv[1], "stop") == 0)
		cmd = APREFIOC_RECSTOP;
	else
		usage();
	fd = devopen(O_RDONLY);
	if (ioctl(fd, cmd, NULL) == -1)
		err(1, "record %s", argv[1]);
	(void)close(fd);
	return 0;
}

/* Run at every login by pam_exec(8): quiet, and idempotent. */
static int
cmd_bootdone(int argc, char **argv)
{
	int fd;

	if (argc != 1)
		usage();
	if ((fd = open(APREFETCH_DEV, O_RDONLY)) == -1) {
		if (errno == ENOENT || errno == ENXIO || errno == ENODEV)
			return 0;
		err(1, "%s", APREFETCH_DEV);
	}
	if (ioctl(fd, APREFIOC_BOOTDONE, NULL) == -1)
		err(1, "boot-done");
	(void)close(fd);
	return 0;
}

static int
cmd_mark(int argc, char **argv)
{
	char label[APREF_MARK_LABELLEN];
	size_t len = 0;
	int i, fd;

	if (argc < 2)
		usage();
	label[0] = '\0';
	for (i = 1; i < argc; i++) {
		if (i > 1)
			(void)strlcat(label, " ", sizeof(label));
		(void)strlcat(label, argv[i], sizeof(label));
	}
	len = strlen(label);
	fd = devopen(O_WRONLY);
	if (write(fd, label, len) == -1)
		err(1, "%s", APREFETCH_DEV);
	(void)close(fd);
	return 0;
}

static double
marksec(const struct aprefetch_mark *m)
{

	return (double)m->am_nsec / 1e9;
}

static const struct aprefetch_mark *
findmark(const struct aprefetch_mark *m, uint32_t n, uint32_t from,
    const char *label)
{
	uint32_t i;

	for (i = from; i < n; i++)
		if (strcmp(m[i].am_label, label) == 0)
			return &m[i];
	return NULL;
}

struct span {
	const char	*sp_name;
	double		sp_sec;
	uint64_t	sp_reads;
};

static int
span_cmp(const void *a, const void *b)
{
	const struct span *s = a, *t = b;

	return s->sp_sec > t->sp_sec ? -1 : s->sp_sec < t->sp_sec;
}

/* The intervals that the measurements are about. */
static void
summarize(const struct aprefetch_mark *m, uint32_t n)
{
	const struct aprefetch_mark *root, *init, *rcdone, *done;
	struct span *sp;
	char name[APREF_MARK_LABELLEN];
	uint32_t i, ns = 0;

	root = findmark(m, n, 0, "kernel: root mounted");
	init = findmark(m, n, 0, "kernel: init");
	rcdone = findmark(m, n, 0, "rc: done");
	if (root != NULL)
		printf("kernel:     %8.3f s from boot to the root mount\n",
		    marksec(root));
	if (init != NULL && rcdone != NULL)
		printf("userland:   %8.3f s from init to the end of /etc/rc; "
		    "%" PRIu64 " disk reads, %.1f MB\n",
		    marksec(rcdone) - marksec(init),
		    rcdone->am_rxfer - init->am_rxfer,
		    MB(rcdone->am_rbytes - init->am_rbytes));

	if ((sp = calloc(MAX(n, 1), sizeof(*sp))) == NULL)
		err(1, "calloc");
	for (i = 0; i < n; i++) {
		if (strncmp(m[i].am_label, "start ", 6) != 0)
			continue;
		(void)snprintf(name, sizeof(name), "done %s",
		    m[i].am_label + 6);
		if ((done = findmark(m, n, i + 1, name)) == NULL)
			continue;
		sp[ns].sp_name = m[i].am_label + 6;
		sp[ns].sp_sec = marksec(done) - marksec(&m[i]);
		sp[ns].sp_reads = done->am_rxfer - m[i].am_rxfer;
		ns++;
	}
	if (ns > 0) {
		qsort(sp, ns, sizeof(*sp), span_cmp);
		printf("slowest rc.d scripts:\n");
		for (i = 0; i < MIN(ns, 10); i++)
			printf("  %8.3f s %6" PRIu64 " reads  %s\n",
			    sp[i].sp_sec, sp[i].sp_reads, sp[i].sp_name);
	}
	free(sp);
}

static int
cmd_marks(int argc, char **argv)
{
	struct aprefetch_mark *m;
	struct aprefetch_marks am;
	bool sflag = false;
	uint32_t i;
	int ch, fd;

	while ((ch = getopt(argc, argv, "s")) != -1) {
		switch (ch) {
		case 's':
			sflag = true;
			break;
		default:
			usage();
		}
	}
	if (argc != optind)
		usage();

	if ((m = calloc(MAXMARKS, sizeof(*m))) == NULL)
		err(1, "calloc");
	memset(&am, 0, sizeof(am));
	am.am_buf = m;
	am.am_count = MAXMARKS;
	fd = devopen(O_RDONLY);
	if (ioctl(fd, APREFIOC_GETMARKS, &am) == -1)
		err(1, "APREFIOC_GETMARKS");
	(void)close(fd);

	if (!sflag) {
		printf("%10s %9s %7s %8s  %s\n", "seconds", "delta", "reads",
		    "MB read", "label");
		for (i = 0; i < am.am_count; i++) {
			const struct aprefetch_mark * const p =
			    i > 0 ? &m[i - 1] : NULL;

			printf("%10.3f %+9.3f %7" PRIu64 " %8.1f  %s\n",
			    marksec(&m[i]),
			    marksec(&m[i]) - (p ? marksec(p) : 0),
			    m[i].am_rxfer - (p ? p->am_rxfer : 0),
			    MB(m[i].am_rbytes - (p ? p->am_rbytes : 0)),
			    m[i].am_label);
		}
		printf("\n");
	}
	summarize(m, am.am_count);
	if (am.am_dropped > 0)
		printf("%" PRIu64 " marks were dropped: the table is full\n",
		    am.am_dropped);
	free(m);
	return 0;
}

static void
listprof(const char *path)
{
	struct aprefio_prof p;
	uint32_t i, nmeta;

	if (aprefio_load_prof(path, &p) == -1) {
		printf("%-34s %s\n", path, strerror(errno));
		return;
	}
	for (i = 0, nmeta = 0; i < p.p_hdr.ph_nentries; i++)
		nmeta += p.p_ents[i].pe_kind != APREF_KIND_DATA;
	printf("%-34s %s %8" PRIu32 " entries (%" PRIu32 " metadata) %8"
	    PRIu32 " pages %8.1f MB\n", path, timestr(p.p_hdr.ph_ctime),
	    p.p_hdr.ph_nentries, nmeta, p.p_hdr.ph_npages,
	    MB((uint64_t)p.p_hdr.ph_npages * pagesize));
	aprefio_free_prof(&p);
}

static void
listtrace(const char *path)
{
	struct aprefio_trace t;

	if (aprefio_load_trace(path, &t) == -1) {
		printf("%-34s %s\n", path, strerror(errno));
		return;
	}
	printf("%-34s %s %8" PRIu32 " entries %8" PRIu64 " dropped\n",
	    path, timestr(t.t_hdr.th_ctime), t.t_hdr.th_nrecs,
	    t.t_hdr.th_dropped);
	aprefio_free_trace(&t);
}

static int
cmd_list(int argc, char **argv)
{
	glob_t g;
	size_t i, len;

	if (argc != 1)
		usage();
	memset(&g, 0, sizeof(g));
	if (glob("/etc/aprefetch/*.prof", 0, NULL, &g) == 0) {
		for (i = 0; i < g.gl_pathc; i++)
			listprof(g.gl_pathv[i]);
	}
	globfree(&g);
	memset(&g, 0, sizeof(g));
	if (glob(APREF_TRACEFILE "*", 0, NULL, &g) == 0) {
		for (i = 0; i < g.gl_pathc; i++) {
			len = strlen(g.gl_pathv[i]);
			if (len >= 4 && strcmp(g.gl_pathv[i] + len - 4,
			    ".tmp") == 0)
				continue;
			listtrace(g.gl_pathv[i]);
		}
	}
	globfree(&g);
	return 0;
}

static int
cmd_dump(int argc, char **argv)
{
	struct aprefio_prof p;
	const char *path;
	uint32_t i;

	if (argc != 2)
		usage();
	path = profpath(argv[1]);
	if (aprefio_load_prof(path, &p) == -1)
		err(1, "%s", path);
	printf("%s: version %" PRIu32 ", created %s\n", path,
	    p.p_hdr.ph_version, timestr(p.p_hdr.ph_ctime));
	printf("%" PRIu32 " entries, %" PRIu32 " pages (%.1f MB)\n",
	    p.p_hdr.ph_nentries, p.p_hdr.ph_npages,
	    MB((uint64_t)p.p_hdr.ph_npages * pagesize));
	for (i = 0; i < p.p_hdr.ph_nfs; i++)
		printf("file system %#018" PRIx64 "\n", p.p_hdr.ph_fsid[i]);
	printf("%-18s %-4s %10s %10s %14s %8s %6s\n", "fsid", "kind", "inode",
	    "generation", "offset/block", "pages", "size");
	for (i = 0; i < p.p_hdr.ph_nentries; i++) {
		const struct aprefetch_prof_ent * const e = &p.p_ents[i];

		printf("%#018" PRIx64 " %-4s ", e->pe_fsid,
		    kindname(e->pe_kind));
		if (e->pe_kind == APREF_KIND_DBLK)
			printf("%10s %10s", "-", "-");
		else
			printf("%10" PRIu64 " %10" PRIu32, e->pe_ino,
			    e->pe_gen);
		printf(" %14" PRIu64 " %8" PRIu32, e->pe_off, e->pe_npages);
		if (e->pe_kind == APREF_KIND_DATA)
			printf(" %6s\n", "-");
		else
			printf(" %6" PRIu32 "\n", e->pe_size);
	}
	aprefio_free_prof(&p);
	return 0;
}

static int
cmd_delete(int argc, char **argv)
{
	const char *path;

	if (argc != 2)
		usage();
	path = profpath(argv[1]);
	if (unlink(path) == -1)
		err(1, "%s", path);
	return 0;
}

static int
rec_cmp(const void *a, const void *b)
{
	const struct aprefetch_rec *r = a, *s = b;

	return r->ar_msec < s->ar_msec ? -1 : r->ar_msec > s->ar_msec;
}

static void
loadtrace(const char *path, struct aprefio_trace *t)
{

	if (aprefio_load_trace(path, t) == -1)
		err(1, "%s", path);
	/* Entries come from several CPUs: put them in time order. */
	qsort(t->t_recs, t->t_hdr.th_nrecs, sizeof(*t->t_recs), rec_cmp);
}

static int
cmd_trace(int argc, char **argv)
{
	struct aprefio_trace t;
	const char *path = APREF_TRACEFILE;
	uint32_t i;

	if (argc > 2)
		usage();
	if (argc == 2)
		path = argv[1];
	loadtrace(path, &t);
	printf("%s: saved %s, %" PRIu32 " entries, %" PRIu64 " dropped\n",
	    path, timestr(t.t_hdr.th_ctime), t.t_hdr.th_nrecs,
	    t.t_hdr.th_dropped);
	printf("%8s %6s %-18s %-4s %10s %10s %14s %8s %6s %s\n", "msec",
	    "pid", "fsid", "kind", "inode", "generation", "offset/block",
	    "pages", "size", "flags");
	for (i = 0; i < t.t_hdr.th_nrecs; i++) {
		const struct aprefetch_rec * const r = &t.t_recs[i];
		const uint32_t kind = APREF_REC_KIND(r->ar_flags);

		printf("%8" PRIu32 " %6" PRIu32 " %#018" PRIx64 " %-4s ",
		    r->ar_msec, r->ar_pid, r->ar_fsid, kindname(kind));
		if (kind == APREF_KIND_DBLK)
			printf("%10s %10s", "-", "-");
		else
			printf("%10" PRIu64 " %10" PRIu32, r->ar_ino,
			    r->ar_gen);
		printf(" %14" PRIu64 " %8" PRIu32, r->ar_off, r->ar_npages);
		if (kind == APREF_KIND_DATA)
			printf(" %6s", "-");
		else
			printf(" %6" PRIu32, r->ar_size);
		printf(" %s\n", (r->ar_flags & APREF_REC_MISS) ? "miss" : "-");
	}
	aprefio_free_trace(&t);
	return 0;
}

static int
point_cmp(const void *a, const void *b)
{
	const struct point *p = a, *q = b;

	if (p->pt_dev != q->pt_dev)
		return p->pt_dev < q->pt_dev ? -1 : 1;
	return p->pt_blkno < q->pt_blkno ? -1 : p->pt_blkno > q->pt_blkno;
}

/* Jumps between consecutive pieces on the same disk, and their size. */
static void
seekstats(const struct point *pt, size_t n, const char *what)
{
	uint64_t jumps = 0, dist = 0;
	int64_t end, d;
	size_t i;

	for (i = 1; i < n; i++) {
		if (pt[i].pt_dev != pt[i - 1].pt_dev)
			continue;
		end = pt[i - 1].pt_blkno + pt[i - 1].pt_nblks;
		d = pt[i].pt_blkno - end;
		if (d == 0)
			continue;
		jumps++;
		dist += (uint64_t)(d < 0 ? -d : d);
	}
	printf("in %s order: %8" PRIu64 " jumps, %10.1f MB apart on average\n",
	    what, jumps, jumps ? MB(dist * DEV_BSIZE) / (double)jumps : 0.0);
}

static void
plot(const struct point *pt, size_t n, uint64_t dev, uint32_t tmax,
    int64_t bmin, int64_t bmax)
{
	unsigned char grid[PLOTH][PLOTW];
	size_t i;
	int x, y;

	memset(grid, 0, sizeof(grid));
	for (i = 0; i < n; i++) {
		if (pt[i].pt_dev != dev)
			continue;
		x = (int)((double)pt[i].pt_msec / (tmax + 1) * PLOTW);
		y = (int)((double)(pt[i].pt_blkno - bmin) /
		    (double)(bmax - bmin + 1) * PLOTH);
		if (grid[y][x] < 255)
			grid[y][x]++;
	}
	printf("\ndisk %#" PRIx64 ", GB from the start (up), seconds (right)\n",
	    dev);
	for (y = PLOTH - 1; y >= 0; y--) {
		printf("%8.2f |", (double)(bmin + (bmax - bmin) *
		    (y + 1) / PLOTH) * DEV_BSIZE / (1024.0 * 1024 * 1024));
		for (x = 0; x < PLOTW; x++)
			putchar(grid[y][x] == 0 ? ' ' : grid[y][x] < 4 ? '.' :
			    grid[y][x] < 16 ? 'o' : '#');
		putchar('\n');
	}
	printf("%8s +", "");
	for (x = 0; x < PLOTW; x++)
		putchar('-');
	printf("\n%9s0%*.1f\n", "", PLOTW, tmax / 1000.0);
}

static void
writecsv(const char *path, const struct point *pt, size_t n)
{
	FILE *f;
	size_t i;

	if ((f = fopen(path, "w")) == NULL)
		err(1, "%s", path);
	fprintf(f, "msec,dev,blkno,blocks,kind,ino,offset,pages,miss\n");
	for (i = 0; i < n; i++)
		fprintf(f, "%" PRIu32 ",%#" PRIx64 ",%" PRId64 ",%" PRIu32
		    ",%s,%" PRIu64 ",%" PRIu64 ",%" PRIu32 ",%d\n",
		    pt[i].pt_msec, pt[i].pt_dev, pt[i].pt_blkno,
		    pt[i].pt_nblks,
		    kindname(APREF_REC_KIND(pt[i].pt_rec->ar_flags)),
		    pt[i].pt_rec->ar_ino, pt[i].pt_rec->ar_off,
		    pt[i].pt_rec->ar_npages,
		    (pt[i].pt_rec->ar_flags & APREF_REC_MISS) != 0);
	if (fclose(f) == EOF)
		err(1, "%s", path);
}

static void
writesvg(const char *path, const struct point *pt, size_t n, uint64_t dev,
    uint32_t tmax, int64_t bmin, int64_t bmax)
{
	const int w = 960, h = 540, l = 90, r = 20, t = 20, b = 50;
	const double pw = w - l - r, ph = h - t - b;
	FILE *f;
	size_t i;
	double x, y, hh;

	if ((f = fopen(path, "w")) == NULL)
		err(1, "%s", path);
	fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" "
	    "height=\"%d\" font-family=\"sans-serif\" font-size=\"12\">\n"
	    "<rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", w, h, w, h);
	fprintf(f, "<polyline points=\"%d,%d %d,%d %d,%d\" fill=\"none\" "
	    "stroke=\"black\"/>\n", l, t, l, h - b, w - r, h - b);
	fprintf(f, "<text x=\"%d\" y=\"%d\">0 s</text>\n", l, h - b + 18);
	fprintf(f, "<text x=\"%d\" y=\"%d\" text-anchor=\"end\">%.1f s</text>\n",
	    w - r, h - b + 18, tmax / 1000.0);
	fprintf(f, "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\">time since "
	    "the recording window opened</text>\n", l + (w - l - r) / 2,
	    h - 12);
	fprintf(f, "<text x=\"%d\" y=\"%d\" text-anchor=\"end\">%.2f GB</text>\n",
	    l - 6, t + 4, (double)bmax * DEV_BSIZE / (1024.0 * 1024 * 1024));
	fprintf(f, "<text x=\"%d\" y=\"%d\" text-anchor=\"end\">%.2f GB</text>\n",
	    l - 6, h - b, (double)bmin * DEV_BSIZE / (1024.0 * 1024 * 1024));
	fprintf(f, "<g fill=\"#2a6fb0\">\n");
	for (i = 0; i < n; i++) {
		if (pt[i].pt_dev != dev)
			continue;
		x = l + pw * pt[i].pt_msec / (tmax + 1);
		y = (h - b) - ph * (double)(pt[i].pt_blkno - bmin) /
		    (double)(bmax - bmin + 1);
		hh = MAX(1.0, ph * pt[i].pt_nblks / (double)(bmax - bmin + 1));
		fprintf(f, "<rect x=\"%.1f\" y=\"%.1f\" width=\"2\" "
		    "height=\"%.1f\"/>\n", x, y - hh, hh);
	}
	fprintf(f, "</g>\n</svg>\n");
	if (fclose(f) == EOF)
		err(1, "%s", path);
}

/*
 * Map the recorded reads to disk blocks and show how scattered they
 * are: in the order they were read, against the order of the disk.
 */
/* Points being collected, with their total size and latest time. */
struct points {
	struct point	*ps_pt;
	size_t		ps_n;
	size_t		ps_max;
	uint64_t	ps_bytes;
	uint32_t	ps_tmax;
};

static void
addpoint(struct points *ps, uint64_t dev, int64_t blkno, uint32_t nblks,
    const struct aprefetch_rec *r)
{
	struct point *pt;

	if (ps->ps_n == ps->ps_max) {
		ps->ps_max = MAX(ps->ps_max * 2, 1024);
		ps->ps_pt = realloc(ps->ps_pt, ps->ps_max * sizeof(*pt));
		if (ps->ps_pt == NULL)
			err(1, "realloc");
	}
	pt = &ps->ps_pt[ps->ps_n++];
	pt->pt_dev = dev;
	pt->pt_blkno = blkno;
	pt->pt_nblks = nblks;
	pt->pt_msec = r->ar_msec;
	pt->pt_rec = r;
	ps->ps_bytes += (uint64_t)nblks * DEV_BSIZE;
	ps->ps_tmax = MAX(ps->ps_tmax, r->ar_msec);
}

/* Block size of the mounted file system fsid, or 0. */
static uint64_t
fsiosize(uint64_t fsid)
{
	static struct statvfs *mnt;
	static int nmnt = -1;
	int i;

	if (nmnt == -1 && (nmnt = getmntinfo(&mnt, ST_NOWAIT)) == 0)
		err(1, "getmntinfo");
	for (i = 0; i < nmnt; i++) {
		if (APREFETCH_FSID(mnt[i].f_fsidx) == fsid)
			return mnt[i].f_iosize;
	}
	return 0;
}

static int
cmd_scatter(int argc, char **argv)
{
	struct aprefio_trace t;
	struct aprefetch_extent ext[MAXEXT];
	struct aprefetch_bmap ab;
	struct points ps = { .ps_pt = NULL };
	struct point *pt, *sorted;
	const char *path = APREF_TRACEFILE, *csv = NULL, *svg = NULL;
	uint64_t dev = 0;
	size_t n, best = 0, cnt;
	uint32_t i, j, used = 0, nmeta = 0, stale = 0, tmax;
	int64_t bmin = INT64_MAX, bmax = 0;
	bool aflag = false;
	int ch, fd;

	while ((ch = getopt(argc, argv, "ac:s:")) != -1) {
		switch (ch) {
		case 'a':
			aflag = true;
			break;
		case 'c':
			csv = optarg;
			break;
		case 's':
			svg = optarg;
			break;
		default:
			usage();
		}
	}
	if (argc - optind > 1)
		usage();
	if (argc - optind == 1)
		path = argv[optind];
	loadtrace(path, &t);

	fd = devopen(O_RDONLY);
	for (i = 0; i < t.t_hdr.th_nrecs; i++) {
		const struct aprefetch_rec * const r = &t.t_recs[i];
		const uint32_t kind = APREF_REC_KIND(r->ar_flags);
		uint64_t bsize;

		if (!aflag && (r->ar_flags & APREF_REC_MISS) == 0)
			continue;
		used++;
		if (kind != APREF_KIND_DATA)
			nmeta++;
		if (kind == APREF_KIND_DBLK) {
			/* On the disk already; FFS has its dev_t in fsid. */
			addpoint(&ps, (uint32_t)r->ar_fsid, (int64_t)r->ar_off,
			    r->ar_size / DEV_BSIZE, r);
			continue;
		}
		memset(&ab, 0, sizeof(ab));
		ab.ab_fsid = r->ar_fsid;
		ab.ab_ino = r->ar_ino;
		ab.ab_gen = r->ar_gen;
		ab.ab_off = r->ar_off;
		ab.ab_npages = r->ar_npages;
		ab.ab_ext = ext;
		ab.ab_next = MAXEXT;
		if (kind == APREF_KIND_VBLK) {
			/* A block of a directory: map it as a range. */
			if ((bsize = fsiosize(r->ar_fsid)) == 0) {
				stale++;
				continue;
			}
			ab.ab_off = r->ar_off * bsize;
		}
		if (ioctl(fd, APREFIOC_BMAP, &ab) == -1)
			err(1, "APREFIOC_BMAP");
		if (ab.ab_error != 0) {
			stale++;
			continue;
		}
		for (j = 0; j < ab.ab_next; j++) {
			if (ext[j].ae_blkno < 0)
				continue;
			addpoint(&ps, ext[j].ae_dev, ext[j].ae_blkno,
			    (uint32_t)(ext[j].ae_npages *
			    (pagesize / DEV_BSIZE)), r);
		}
	}
	(void)close(fd);

	pt = ps.ps_pt;
	n = ps.ps_n;
	tmax = ps.ps_tmax;
	printf("%s: %" PRIu32 " %s (%" PRIu32 " of metadata), %zu pieces of "
	    "disk, %.1f MB\n", path, used,
	    aflag ? "entries" : "entries that missed the cache", nmeta, n,
	    MB(ps.ps_bytes));
	if (stale > 0)
		printf("not mapped: %" PRIu32 " entries whose file is gone or "
		    "changed\n", stale);
	if (n == 0) {
		aprefio_free_trace(&t);
		return 0;
	}
	seekstats(pt, n, "time");
	if ((sorted = malloc(n * sizeof(*sorted))) == NULL)
		err(1, "malloc");
	memcpy(sorted, pt, n * sizeof(*sorted));
	qsort(sorted, n, sizeof(*sorted), point_cmp);
	seekstats(sorted, n, "disk");

	/* Plot the disk with the most pieces. */
	for (i = 0, cnt = 0; i < n; i++) {
		cnt = (i > 0 && sorted[i].pt_dev == sorted[i - 1].pt_dev) ?
		    cnt + 1 : 1;
		if (cnt > best) {
			best = cnt;
			dev = sorted[i].pt_dev;
		}
	}
	for (i = 0; i < n; i++) {
		if (pt[i].pt_dev != dev)
			continue;
		bmin = MIN(bmin, pt[i].pt_blkno);
		bmax = MAX(bmax, pt[i].pt_blkno + pt[i].pt_nblks);
	}
	plot(pt, n, dev, tmax, bmin, bmax);
	if (csv != NULL)
		writecsv(csv, pt, n);
	if (svg != NULL)
		writesvg(svg, pt, n, dev, tmax, bmin, bmax);

	free(sorted);
	free(pt);
	aprefio_free_trace(&t);
	return 0;
}

static int
cmd_replay(int argc, char **argv)
{
	struct aprefetch_replay ar;
	const char *path;
	int64_t budget = 0;
	size_t len;
	void *buf;
	int ch, fd;

	while ((ch = getopt(argc, argv, "b:")) != -1) {
		switch (ch) {
		case 'b':
			if (dehumanize_number(optarg, &budget) == -1 ||
			    budget <= 0)
				errx(1, "-b: %s: not a size", optarg);
			break;
		default:
			usage();
		}
	}
	if (argc - optind != 1)
		usage();
	path = profpath(argv[optind]);
	if ((buf = aprefio_readfile(path, APREF_PROF_MAXSIZE, &len)) == NULL)
		err(1, "%s", path);
	memset(&ar, 0, sizeof(ar));
	ar.ar_prof = buf;
	ar.ar_len = len;
	ar.ar_budget = (uint64_t)budget;
	fd = devopen(O_RDONLY);
	if (ioctl(fd, APREFIOC_REPLAY, &ar) == -1)
		err(1, "replay %s", path);
	(void)close(fd);
	free(buf);
	return 0;
}

int
main(int argc, char **argv)
{
	size_t i;

	pagesize = sysconf(_SC_PAGESIZE);
	if (argc < 2)
		usage();
	for (i = 0; i < __arraycount(cmds); i++) {
		if (strcmp(argv[1], cmds[i].c_name) == 0)
			return (*cmds[i].c_func)(argc - 1, argv + 1);
	}
	usage();
}

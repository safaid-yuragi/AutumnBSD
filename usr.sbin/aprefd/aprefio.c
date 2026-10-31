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

#include <sys/cdefs.h>
__RCSID("$NetBSD$");

#include <sys/param.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "aprefio.h"

/* Read a whole regular file of at most max bytes. */
void *
aprefio_readfile(const char *path, size_t max, size_t *lenp)
{
	struct stat st;
	char *buf = NULL;
	size_t len, done;
	ssize_t n;
	int fd, serrno;

	if ((fd = open(path, O_RDONLY | O_NOFOLLOW)) == -1)
		return NULL;
	if (fstat(fd, &st) == -1)
		goto fail;
	if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
	    (uintmax_t)st.st_size > max) {
		errno = EFTYPE;
		goto fail;
	}
	len = (size_t)st.st_size;
	if ((buf = malloc(len > 0 ? len : 1)) == NULL)
		goto fail;
	for (done = 0; done < len; done += (size_t)n) {
		if ((n = read(fd, buf + done, len - done)) == -1)
			goto fail;
		if (n == 0) {
			errno = EIO;
			goto fail;
		}
	}
	(void)close(fd);
	*lenp = len;
	return buf;
fail:
	serrno = errno;
	free(buf);
	(void)close(fd);
	errno = serrno;
	return NULL;
}

static int
write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	ssize_t n;

	while (len > 0) {
		if ((n = write(fd, p, len)) == -1)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/*
 * Write a file through a temporary name and rename(2), so that a half
 * written file never takes its place.
 */
static int
write_file(const char *path, const void *a, size_t alen, const void *b,
    size_t blen)
{
	char tmp[PATH_MAX];
	int fd, serrno;

	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	(void)unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd == -1)
		return -1;
	if (write_all(fd, a, alen) == -1 || write_all(fd, b, blen) == -1 ||
	    fsync(fd) == -1)
		goto fail;
	if (close(fd) == -1) {
		fd = -1;
		goto fail;
	}
	fd = -1;
	if (rename(tmp, path) == -1)
		goto fail;
	return 0;
fail:
	serrno = errno;
	if (fd != -1)
		(void)close(fd);
	(void)unlink(tmp);
	errno = serrno;
	return -1;
}

static uint32_t
prof_crc(const struct aprefetch_prof_hdr *ph,
    const struct aprefetch_prof_ent *pe, uint32_t n)
{
	struct aprefetch_prof_hdr h = *ph;
	uLong crc;

	h.ph_crc = 0;
	crc = crc32(0L, (const Bytef *)&h, sizeof(h));
	if (n > 0)
		crc = crc32(crc, (const Bytef *)pe, (uInt)(n * sizeof(*pe)));
	return (uint32_t)crc;
}

int
aprefio_load_prof(const char *path, struct aprefio_prof *p)
{
	struct aprefetch_prof_hdr ph;
	struct aprefetch_prof_ent *pe = NULL;
	size_t len, elen;
	char *buf;

	if ((buf = aprefio_readfile(path, APREF_PROF_MAXSIZE, &len)) == NULL)
		return -1;
	if (len < sizeof(ph))
		goto bad;
	memcpy(&ph, buf, sizeof(ph));
	elen = len - sizeof(ph);
	if (ph.ph_magic != APREF_PROF_MAGIC ||
	    ph.ph_version != APREF_PROF_VERSION ||
	    ph.ph_nfs > APREF_PROF_MAXFS || elen % sizeof(*pe) != 0 ||
	    elen / sizeof(*pe) != ph.ph_nentries)
		goto bad;
	if ((pe = malloc(elen > 0 ? elen : 1)) == NULL) {
		free(buf);
		return -1;
	}
	memcpy(pe, buf + sizeof(ph), elen);
	if (prof_crc(&ph, pe, ph.ph_nentries) != ph.ph_crc)
		goto bad;
	free(buf);
	p->p_hdr = ph;
	p->p_ents = pe;
	return 0;
bad:
	free(pe);
	free(buf);
	errno = EFTYPE;
	return -1;
}

int
aprefio_save_prof(const char *path, const struct aprefetch_prof_ent *pe,
    uint32_t n, const uint64_t *fsid, uint32_t nfs)
{
	struct aprefetch_prof_hdr ph;
	uint64_t npages = 0;
	uint32_t i;

	if (nfs > APREF_PROF_MAXFS) {
		errno = EINVAL;
		return -1;
	}
	memset(&ph, 0, sizeof(ph));
	ph.ph_magic = APREF_PROF_MAGIC;
	ph.ph_version = APREF_PROF_VERSION;
	ph.ph_ctime = (uint64_t)time(NULL);
	ph.ph_nentries = n;
	for (i = 0; i < n; i++)
		npages += pe[i].pe_npages;
	ph.ph_npages = (uint32_t)MIN(npages, UINT32_MAX);
	ph.ph_nfs = nfs;
	if (nfs > 0)
		memcpy(ph.ph_fsid, fsid, nfs * sizeof(*fsid));
	ph.ph_crc = prof_crc(&ph, pe, n);
	return write_file(path, &ph, sizeof(ph), pe, n * sizeof(*pe));
}

void
aprefio_free_prof(struct aprefio_prof *p)
{

	free(p->p_ents);
	p->p_ents = NULL;
}

int
aprefio_load_trace(const char *path, struct aprefio_trace *t)
{
	struct aprefetch_trace_hdr th;
	struct aprefetch_rec *rec = NULL;
	size_t len, rlen;
	char *buf;

	buf = aprefio_readfile(path, sizeof(th) +
	    (size_t)APREF_TRACEMAX * sizeof(*rec), &len);
	if (buf == NULL)
		return -1;
	if (len < sizeof(th))
		goto bad;
	memcpy(&th, buf, sizeof(th));
	rlen = len - sizeof(th);
	if (th.th_magic != APREF_TRACE_MAGIC ||
	    th.th_version != APREF_TRACE_VERSION ||
	    th.th_recsize != sizeof(*rec) ||
	    rlen != (size_t)th.th_nrecs * sizeof(*rec))
		goto bad;
	if ((rec = malloc(rlen > 0 ? rlen : 1)) == NULL) {
		free(buf);
		return -1;
	}
	memcpy(rec, buf + sizeof(th), rlen);
	free(buf);
	t->t_hdr = th;
	t->t_recs = rec;
	return 0;
bad:
	free(buf);
	errno = EFTYPE;
	return -1;
}

int
aprefio_save_trace(const char *path, const struct aprefetch_rec *rec,
    uint32_t n, uint64_t dropped)
{
	struct aprefetch_trace_hdr th;

	memset(&th, 0, sizeof(th));
	th.th_magic = APREF_TRACE_MAGIC;
	th.th_version = APREF_TRACE_VERSION;
	th.th_ctime = (uint64_t)time(NULL);
	th.th_nrecs = n;
	th.th_recsize = sizeof(*rec);
	th.th_dropped = dropped;
	return write_file(path, &th, sizeof(th), rec, n * sizeof(*rec));
}

void
aprefio_free_trace(struct aprefio_trace *t)
{

	free(t->t_recs);
	t->t_recs = NULL;
}

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

/* Profile and trace files, shared by aprefd(8) and aprefctl(8). */

#ifndef _APREFIO_H_
#define _APREFIO_H_

#include <sys/aprefetch.h>

#define	APREF_TRACEFILE		APREFETCH_DBDIR "/boot.trace"
#define	APREF_TRACEMAX		(8 * 1024 * 1024)	/* entries */

struct aprefio_prof {
	struct aprefetch_prof_hdr	 p_hdr;
	struct aprefetch_prof_ent	*p_ents;
};

struct aprefio_trace {
	struct aprefetch_trace_hdr	 t_hdr;
	struct aprefetch_rec		*t_recs;
};

int	aprefio_load_prof(const char *, struct aprefio_prof *);
int	aprefio_save_prof(const char *, const struct aprefetch_prof_ent *,
	    uint32_t, const uint64_t *, uint32_t);
void	aprefio_free_prof(struct aprefio_prof *);
int	aprefio_load_trace(const char *, struct aprefio_trace *);
int	aprefio_save_trace(const char *, const struct aprefetch_rec *,
	    uint32_t, uint64_t);
void	aprefio_free_trace(struct aprefio_trace *);
void	*aprefio_readfile(const char *, size_t, size_t *);

#endif /* !_APREFIO_H_ */

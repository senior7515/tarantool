/*-
 * Copyright (c) 2013 Advanced Computing Technologies LLC
 * Written by: John H. Baldwin <jhb@FreeBSD.org>
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

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if defined(__NetBSD__)
typedef off_t funopen_off_t;
#else

typedef fpos_t funopen_off_t;
#ifndef __type_max

#define type_signed(t) (! ((t) 0 < (t) -1))
#define __type_max(t) \
  ((t) (! type_signed (t) ? (t) -1 \
        : (t) (~ (((unsigned long long) (~ (t) 0)) << (sizeof (t) * CHAR_BIT - 1)))))

#endif /* __type_max */

#endif

#define	FUNOPEN_OFF_MAX __type_max(funopen_off_t)

struct memstream {
	char **bufp;
	size_t *sizep;
	ssize_t len;
	off_t offset;
};

static int
memstream_grow(struct memstream *ms, funopen_off_t newoff)
{
	char *buf;
	funopen_off_t newsize;

	if (newoff < 0 || newoff >= SSIZE_MAX)
		newsize = SSIZE_MAX - 1;
	else
		newsize = newoff;
	if (newsize > ms->len) {
		buf = realloc(*ms->bufp, newsize + 1);
		if (buf != NULL) {
			memset(buf + ms->len + 1, 0, newsize - ms->len);
			*ms->bufp = buf;
			ms->len = newsize;
			return (1);
		}
		return (0);
	}
	return (1);
}

static void
memstream_update(struct memstream *ms)
{

	assert(ms->len >= 0 && ms->offset >= 0);
	*ms->sizep = ms->len < ms->offset ? ms->len : ms->offset;
}

static int
memstream_write(void *cookie, const char *buf, int len)
{
	struct memstream *ms;
	ssize_t tocopy;

	ms = cookie;
	if (!memstream_grow(ms, ms->offset + len))
		return (-1);
	tocopy = ms->len - ms->offset;
	if (len < tocopy)
		tocopy = len;
	memcpy(*ms->bufp + ms->offset, buf, tocopy);
	ms->offset += tocopy;
	memstream_update(ms);
	return (tocopy);
}

static funopen_off_t
memstream_seek(void *cookie, funopen_off_t pos, int whence)
{
	struct memstream *ms;
#ifdef DEBUG
	funopen_off_t old;
#endif

	ms = cookie;
#ifdef DEBUG
	old = ms->offset;
#endif
	switch (whence) {
	case SEEK_SET:
		/* _fseeko() checks for negative offsets. */
		assert(pos >= 0);
		ms->offset = pos;
		break;
	case SEEK_CUR:
		/* This is only called by _ftello(). */
		assert(pos == 0);
		break;
	case SEEK_END:
		if (pos < 0) {
			if (pos + ms->len < 0) {
				errno = EINVAL;
				return (-1);
			}
		} else {
			if (FUNOPEN_OFF_MAX - ms->len < pos) {
				errno = EOVERFLOW;
				return (-1);
			}
		}
		ms->offset = ms->len + pos;
		break;
	}
	memstream_update(ms);
	return (ms->offset);
}

static int
memstream_close(void *cookie)
{

	free(cookie);
	return (0);
}

FILE *
open_memstream(char **bufp, size_t *sizep)
{
	struct memstream *ms;
	int save_errno;
	FILE *fp;

	if (bufp == NULL || sizep == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	*bufp = calloc(1, 1);
	if (*bufp == NULL)
		return (NULL);
	ms = malloc(sizeof(*ms));
	if (ms == NULL) {
		save_errno = errno;
		free(*bufp);
		*bufp = NULL;
		errno = save_errno;
		return (NULL);
	}
	ms->bufp = bufp;
	ms->sizep = sizep;
	ms->len = 0;
	ms->offset = 0;
	memstream_update(ms);
	fp = funopen(ms, NULL, memstream_write, memstream_seek,
	    memstream_close);
	if (fp == NULL) {
		save_errno = errno;
		free(ms);
		free(*bufp);
		*bufp = NULL;
		errno = save_errno;
		return (NULL);
	}
	fwide(fp, -1);
	return (fp);
}

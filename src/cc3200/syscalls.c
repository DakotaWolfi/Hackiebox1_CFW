/*
 * The firmware has no heap: every allocation fails. The allocator
 * entries are defined here so that newlib's allocator (pulled in by
 * references from printf's string growth and Opus' unused create
 * functions) is not linked.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <stddef.h>
#include <reent.h>

void *_sbrk(ptrdiff_t incr)
{
    (void)incr;
    errno = ENOMEM;
    return (void *)-1;
}

void *_malloc_r(struct _reent *r, size_t n)
{
    (void)n;
    r->_errno = ENOMEM;
    return NULL;
}

void *_realloc_r(struct _reent *r, void *p, size_t n)
{
    (void)p;
    (void)n;
    r->_errno = ENOMEM;
    return NULL;
}

void _free_r(struct _reent *r, void *p)
{
    (void)r;
    (void)p;
}

void *malloc(size_t n)
{
    (void)n;
    errno = ENOMEM;
    return NULL;
}

void free(void *p)
{
    (void)p;
}

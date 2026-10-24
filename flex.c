/* flex.c
 *
 * Sliding heap allocator implementing the interface in flex.h.
 *
 * Blocks live contiguously between flex__.base and flex__.top. Memory is
 * obtained either from a dynamic area or by growing the Wimp slot. Each
 * block is preceded by a header recording its anchor and size. Freed blocks
 * have a NULL anchor until compaction slides later blocks down over them.
 *
 * vim: sw=4 ts=8 et
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"
#include "swis.h"

#include "opts.h"
#include "swiextra.h"

#include "flex.h"

/* ----------------------------------------------------------------------- */

/* Start of application space: where the Wimp slot begins. */
#define flex__APPSPACE ((char *) 0x8000)

typedef struct
{
    flex_ptr anchor; /* NULL if free */
    int      size;   /* requested size in bytes */
}
flex__hdr;

/* Sizes are rounded to a multiple of the header size (a power of two). */
#define flex__ROUND(n) \
    (((n) + (int) sizeof(flex__hdr) - 1) & ~((int) sizeof(flex__hdr) - 1))

#define flex__LEN(h) ((int) sizeof(flex__hdr) + flex__ROUND((h)->size))

static struct
{
    char *base;     /* first block */
    char *top;      /* end of last block */
    char *limit;    /* end of memory claimed */
    int   area;     /* dynamic area number, or 0 for Wimp slot */
    int   budging;  /* move up when the C library wants memory */
    int   deferred; /* compact lazily rather than on every free */
    int   freed;    /* bytes in free blocks awaiting compaction */
    char *name;     /* program name for error reports */
    int  *msgs;     /* MessageTrans file descriptor, or NULL */
}
flex__;

/* ----------------------------------------------------------------------- */

/* Report a fatal error, translated if a messages file was given, and exit. */
static void flex__fail(const char *token, const char *dflt)
{
    _kernel_oserror e;

    e.errnum = 0;
    if (flex__.msgs == NULL ||
        _swix(MessageTrans_Lookup, _INR(0,7), flex__.msgs, token,
              e.errmess, sizeof(e.errmess), 0, 0, 0, 0))
        strcpy(e.errmess, dflt);

    _swix(Wimp_ReportError, _INR(0,2), &e, 1, flex__.name);
    exit(EXIT_FAILURE);
}

static void flex__check(void)
{
    if (flex__.base == NULL)
        flex__fail("flex3", "Flex not initialised");
}

/* Return the header for anchor's block, failing if anchor isn't valid. */
static flex__hdr *flex__header(flex_ptr anchor)
{
    char      *p;
    flex__hdr *h;

    flex__check();

    p = *anchor;
    if (p < flex__.base + sizeof(flex__hdr) || p > flex__.top)
        flex__fail("flex1", "Flex memory error");

    h = (flex__hdr *) (p - sizeof(flex__hdr));
    if (h->anchor != anchor)
        flex__fail("flex1", "Flex memory error");

    return h;
}

/* Set the Wimp slot size (-1 reads it). Wimp_SlotSize also moves the memory
 * limit, so lift the limit to the top of application space for the call
 * and put it back afterwards. This keeps the C library ignorant of flex's
 * memory above its own limit. */
static _kernel_oserror *flex__slot(int size, int *got)
{
    int              memlim;
    int              appspace;
    _kernel_oserror *e;

    memlim = appspace = 0;
    _swix(OS_ChangeEnvironment, _INR(0,2)|_OUT(1), 0, 0, 0, &memlim);
    _swix(OS_ChangeEnvironment, _INR(0,2)|_OUT(1), 14, 0, 0, &appspace);

    if (appspace > memlim)
        _swix(OS_ChangeEnvironment, _INR(0,2), 0, appspace, 0);

    e = _swix(Wimp_SlotSize, _INR(0,1)|_OUT(0), size, -1, got);

    if (appspace > memlim)
        _swix(OS_ChangeEnvironment, _INR(0,2), 0, memlim, 0);

    return e;
}

/* Ensure there are at least n spare bytes above flex__.top. */
static int flex__more(int n)
{
    int need;
    int got;

    need = n - (int) (flex__.limit - flex__.top);
    if (need <= 0)
        return 1;

    if (flex__.area)
    {
        if (_swix(OS_ChangeDynamicArea, _INR(0,1)|_OUT(1),
                  flex__.area, need, &got))
            return 0;
        flex__.limit += got;
    }
    else
    {
        if (flex__slot((int) (flex__.limit + need - flex__APPSPACE), &got))
            return 0;
        flex__.limit = flex__APPSPACE + got;
    }

    return flex__.limit - flex__.top >= n;
}

/* Return spare memory above flex__.top to the OS. */
static void flex__give(void)
{
    int spare;
    int got;

    spare = (int) (flex__.limit - flex__.top);
    if (spare <= 0)
        return;

    if (flex__.area)
    {
        if (_swix(OS_ChangeDynamicArea, _INR(0,1)|_OUT(1),
                  flex__.area, -spare, &got) == NULL)
            flex__.limit -= got;
    }
    else
    {
        if (flex__slot((int) (flex__.top - flex__APPSPACE), &got) == NULL)
            flex__.limit = flex__APPSPACE + got;
    }
}

/* Move every block from 'from' to flex__.top by 'by' bytes, fixing anchors.
 * The caller must ensure the memory is available. */
static void flex__shift(char *from, int by)
{
    char      *p;
    flex__hdr *h;

    memmove(from + by, from, flex__.top - from);
    flex__.top += by;

    for (p = from + by; p < flex__.top; p += flex__LEN(h))
    {
        h = (flex__hdr *) p;
        if (h->anchor)
            *h->anchor = h + 1;
    }
}

/* Slide used blocks down over free ones. */
static void flex__squeeze(void)
{
    char      *src;
    char      *dst;
    flex__hdr *h;
    int        len;

    if (flex__.freed == 0)
        return;

    src = dst = flex__.base;
    while (src < flex__.top)
    {
        h   = (flex__hdr *) src;
        len = flex__LEN(h);
        if (h->anchor)
        {
            if (dst != src)
            {
                memmove(dst, src, len);
                h = (flex__hdr *) dst;
                *h->anchor = h + 1;
            }
            dst += len;
        }
        src += len;
    }
    flex__.top   = dst;
    flex__.freed = 0;
}

/* Called by the C library when it needs to extend its heap in the Wimp
 * slot. If budging, slide the whole flex heap up and hand over the gap. */
static int flex__budge(int n, void **p)
{
    int by;

    by = flex__ROUND(n);

    if (!flex__.budging)
        return 0;

    flex__squeeze();
    if (!flex__more(by))
        return 0;

    flex__shift(flex__.base, by);
    *p = flex__.base;
    flex__.base += by;

    return by;
}

static void flex__die(void)
{
    _swix(OS_DynamicArea, _INR(0,1), OS_DynamicArea_Remove, flex__.area);
}

/* ----------------------------------------------------------------------- */

int flex_alloc(flex_ptr anchor, int n)
{
    int        len;
    flex__hdr *h;

    flex__check();

    /* Clear the anchor on failure, as Acorn's flex did. */
    if (n < 0)
    {
        *anchor = NULL;
        return 0;
    }

    len = (int) sizeof(flex__hdr) + flex__ROUND(n);

    flex__squeeze();
    if (!flex__more(len))
    {
        *anchor = NULL;
        return 0;
    }

    h = (flex__hdr *) flex__.top;
    h->anchor = anchor;
    h->size   = n;
    flex__.top += len;
    *anchor = h + 1;

    return 1;
}

void flex_free(flex_ptr anchor)
{
    flex__hdr *h;

    h = flex__header(anchor);
    h->anchor = NULL;
    *anchor = NULL;
    flex__.freed += flex__LEN(h);

    if (!flex__.deferred)
        flex_compact();
}

int flex_size(flex_ptr anchor)
{
    return flex__header(anchor)->size;
}

int flex_extend(flex_ptr anchor, int newsize)
{
    int size;

    size = flex_size(anchor);

    return flex_midextend(anchor, size, newsize - size);
}

int flex_midextend(flex_ptr anchor, int at, int by)
{
    flex__hdr *h;
    int        size;
    int        delta;
    char      *p;

    h    = flex__header(anchor);
    size = h->size;

    if (at < 0 || at > size || at + by < 0)
        flex__fail("flex1", "Flex memory error");
    if (by == 0)
        return 1;

    delta = flex__ROUND(size + by) - flex__ROUND(size);

    if (delta > 0)
    {
        if (!flex__more(delta))
        {
            flex__squeeze(); /* may move this block */
            if (!flex__more(delta))
                return 0;
            h = flex__header(anchor);
        }
        flex__shift((char *) h + flex__LEN(h), delta);
    }

    p = *anchor;
    memmove(p + at + by, p + at, size - at);

    if (delta < 0)
        flex__shift((char *) h + flex__LEN(h), delta);

    h->size = size + by;

    if (delta < 0 && !flex__.deferred)
        flex__give();

    return 1;
}

int flex_reanchor(flex_ptr to, flex_ptr from)
{
    flex__header(from)->anchor = to;
    *to   = *from;
    *from = NULL;

    return 1;
}

int flex_set_budge(int newstate)
{
    int old;

    /* Budging only applies to the Wimp slot. */
    if (flex__.area)
        return -1;

    old = flex__.budging;
    if (newstate != -1)
        flex__.budging = newstate;

    return old;
}

int flex_init(char *program_name, int *error_fd, int dynamic_size)
{
    int   area;
    char *base;
    int   slot;

    flex__.name = program_name;
    flex__.msgs = error_fd;

    if (dynamic_size &&
        _swix(OS_DynamicArea, _INR(0,8)|_OUT(1)|_OUT(3),
              OS_DynamicArea_Create, -1, DefaultSize, -1,
              OS_DynamicArea_Flags_NotDraggable, dynamic_size, 0, 0,
              program_name, &area, &base) == NULL)
    {
        flex__.area = area;
        flex__.base = flex__.top = flex__.limit = base;
        atexit(flex__die);
    }
    else
    {
        /* No dynamic area: grow the Wimp slot above the C library's heap. */
        if (flex__slot(-1, &slot))
            slot = 0;
        flex__.area = 0;
        flex__.base = flex__.top = flex__.limit = flex__APPSPACE + slot;
        _kernel_register_slotextend(flex__budge);
    }

    /* Check that memory can be claimed at all. */
    if (!flex__more(1))
        flex__fail("flex2", "Not enough memory, or not within *desktop world");
    flex__give();

    return flex__.area;
}

void flex_save_heap_info(char *filename)
{
    FILE      *f;
    char      *p;
    flex__hdr *h;

    f = fopen(filename, "a");
    if (f == NULL)
        return;

    fprintf(f, "flex: area %d base %p top %p limit %p\n",
            flex__.area, (void *) flex__.base, (void *) flex__.top,
            (void *) flex__.limit);

    for (p = flex__.base; p < flex__.top; p += flex__LEN(h))
    {
        h = (flex__hdr *) p;
        fprintf(f, "  %p size %d anchor %p%s\n",
                (void *) (h + 1), h->size, (void *) h->anchor,
                h->anchor ? "" : " (free)");
    }

    fclose(f);
}

int flex_compact(void)
{
    flex__squeeze();
    flex__give();

    return 0;
}

int flex_set_deferred_compaction(int newstate)
{
    int old;

    old = flex__.deferred;
    flex__.deferred = newstate;
    if (!flex__.deferred)
        flex_compact();

    return old;
}

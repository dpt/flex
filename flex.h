/* flex.h
 *
 * Sliding heap allocator for RISC OS.
 *
 * Blocks are reached through an anchor: a pointer variable owned by the
 * caller whose address is passed to every call. flex may move blocks, and
 * updates their anchors when it does, so always access a block through its
 * anchor and never keep a raw pointer into it across a flex call. When
 * budging is on, a block may also move when malloc or stack extension needs
 * more memory.
 *
 * Fatal errors (a bad anchor, bad arguments, use before flex_init) are
 * reported with Wimp_ReportError and the program exits. Messages are looked
 * up with these tokens if a messages file was given to flex_init:
 *
 *   flex1  Flex memory error
 *   flex2  Not enough memory, or not within *desktop world
 *   flex3  Flex not initialised
 *
 * vim: sw=4 ts=8 et
 */

#ifndef FLEX_H
#define FLEX_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void **flex_ptr;

/* Initialise flex. Call this before any other function.
 *
 * program_name names the dynamic area and titles error reports. It must
 * last as long as the program, for example a string literal.
 *
 * error_fd is a MessageTrans file descriptor used to translate error
 * messages, or NULL for the English defaults.
 *
 * dynamic_size is the maximum size of a dynamic area to create, or 0 to
 * use the Wimp slot. If the dynamic area can't be created then the Wimp
 * slot is used instead.
 *
 * Returns the dynamic area number, or 0 if the Wimp slot is in use. Fatal
 * (flex2) if no memory can be claimed. */
int flex_init(char *program_name, int *error_fd, int dynamic_size);

/* Allocate n bytes and point *anchor at them.
 *
 * Returns 1 on success. Returns 0 if there isn't enough memory or n is
 * negative, and sets *anchor to NULL. */
int flex_alloc(flex_ptr anchor, int n);

/* Free the block owned by anchor and set *anchor to NULL. */
void flex_free(flex_ptr anchor);

/* Return the size in bytes of the block owned by anchor. */
int flex_size(flex_ptr anchor);

/* Grow or shrink the block owned by anchor to newsize bytes, keeping its
 * contents up to the smaller of the old and new sizes.
 *
 * Returns 1 on success, or 0 if there isn't enough memory, in which case
 * the block is unchanged. A negative newsize is fatal (flex1). */
int flex_extend(flex_ptr anchor, int newsize);

/* Grow or shrink the block owned by anchor at offset 'at'.
 *
 * If by is positive, by bytes are inserted at 'at': the bytes from 'at'
 * onwards move up by by. The inserted bytes are undefined.
 *
 * If by is negative, -by bytes are removed below 'at': the bytes from 'at'
 * onwards move down to at + by.
 *
 * Returns 1 on success, or 0 if there isn't enough memory, in which case
 * the block is unchanged. Fatal (flex1) if at is outside the block or
 * at + by is negative. */
int flex_midextend(flex_ptr anchor, int at, int by);

/* Move ownership of the block owned by 'from' to 'to'. Sets *to to the
 * block and *from to NULL. Always returns 1. */
int flex_reanchor(flex_ptr to, flex_ptr from);

/* Set whether flex moves its blocks up to give the C library memory when
 * malloc or stack extension needs it. Off by default.
 *
 * Pass 1 to allow moves or 0 to refuse them. With budging off, pointers
 * into blocks stay valid across calls that don't call flex. Pass -1 to
 * leave the state unchanged.
 *
 * Returns the previous state, or -1 if a dynamic area is in use, where
 * budging is never needed. */
int flex_set_budge(int newstate);

/* Set whether to compact on every flex_free (0, the default) or to defer
 * compaction (1). Deferred compaction happens in flex_alloc, in
 * flex_compact, when budging and when flex_midextend runs out of memory.
 * Turning deferral off compacts at once.
 *
 * Returns the previous state. */
int flex_set_deferred_compaction(int newstate);

/* Compact the heap and give spare memory back to the OS. Always returns 0,
 * meaning compaction is complete. */
int flex_compact(void);

/* Append a description of the heap and its blocks to the named file. */
void flex_save_heap_info(char *filename);

#ifdef __cplusplus
}
#endif

#endif /* FLEX_H */

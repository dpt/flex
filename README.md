# flex

A rewrite of Acorn's flex sliding heap allocator for RISC OS.

flex keeps Acorn's interface. Existing code should only need relinking
against the new library. The implementation is new: around 400 lines of C90
in place of the original's 1,000-odd, with the `TRACE` and `LIB_DEBUGGING`
scaffolding gone.

## What stays the same

- The anchor model. Every block is owned by a `flex_ptr` anchor that flex
  updates whenever it moves the block. Access blocks through their anchors
  and never hold a raw pointer into a block across a flex call.
- Every public function, with the same signature and return conventions:
  `flex_init`, `flex_alloc`, `flex_free`, `flex_size`, `flex_extend`,
  `flex_midextend`, `flex_reanchor`, `flex_set_budge`,
  `flex_set_deferred_compaction`, `flex_compact` and `flex_save_heap_info`.
- `flex_init` tries a dynamic area of up to `dynamic_size` bytes, falls back
  to the Wimp slot if it can't create one, and returns the area number or 0.
  The dynamic area is removed at exit.
- Budging is off by default and only applies to the Wimp slot.
- Compaction happens on every `flex_free` unless deferred.
- Fatal errors are reported with `Wimp_ReportError` and the program exits.
  The `flex1`, `flex2` and `flex3` message tokens are unchanged, so existing
  Messages files still work.

## What's different

- **Include guard.** The header's include guard is `FLEX_H` rather than
  `__flex_h`.
- **`flex_set_budge`.** Acorn's version swapped the C library's slot extend
  handler on each call, and would install any other value you passed as a
  handler of its own. flex registers its handler once in `flex_init` and
  `flex_set_budge` just sets a flag. Pass 1 or 0 to switch budging on or off.
  Passing -1 now returns the current state, as Acorn's documentation
  promised (Acorn's code returned -1). It still returns -1 when a dynamic
  area is in use.
- **Wimp slot handling.** When growing or shrinking the Wimp slot, flex
  temporarily lifts the memory limit so that `Wimp_SlotSize` doesn't move the
  C library's idea of where its memory ends.
- **Failure leaves blocks alone.** `flex_extend` and `flex_midextend` return
  0 with the block unchanged when memory runs out. `flex_alloc` sets the
  anchor to NULL on failure or if `n` is negative, as before.
- **Argument checking.** `flex_midextend` treats an `at` outside the block,
  or an `at + by` below zero, as a fatal `flex1` error. A negative size passed
  to `flex_extend` is fatal for the same reason.
- **Deferred compaction.** With deferral on, compaction happens in
  `flex_alloc`, `flex_compact`, when budging and when `flex_midextend` would
  otherwise run out of memory. Shrinking a block with `flex_midextend` only
  gives memory back to the OS straight away when deferral is off.
- **`flex_compact`** always completes and always returns 0.
- **`flex_save_heap_info`** works (Acorn's was marked untested). It
  appends a line for the heap followed by a line per block, marking free
  blocks.
- **Block rounding.** Block sizes are rounded up to a multiple of the
  eight-byte block header rather than to four bytes. `flex_size` still
  returns the size you asked for.

## Building

flex builds for RISC OS only, with CMake 3.18 or later and a RISC OS
toolchain file that defines `TARGET_RISCOS` and `riscos_set_flags()`:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/riscos-toolchain.cmake
cmake --build build
```

This produces a static `flex` library. It needs `kernel.h` and `swis.h` from
the RISC OS C library.

## Usage

```c
#include "flex.h"

static char *buf;

int main(void)
{
    flex_init("MyApp", NULL, 16 * 1024 * 1024);

    if (!flex_alloc((flex_ptr) &buf, 1024))
        return 1;

    buf[0] = 'x';

    if (!flex_extend((flex_ptr) &buf, 4096))
        return 1; /* buf still holds the original 1024 bytes */

    flex_free((flex_ptr) &buf);

    return 0;
}
```

See `flex.h` for the full description of each function.

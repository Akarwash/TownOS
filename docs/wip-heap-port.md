# WIP: Port the p5 explicit-free-list allocator into the kernel as a heap

> Working document. Temporary. Delete this file once the kernel heap is landed
> and the ADR (`docs/decisions/0010-kernel-heap-ported-from-p5.md`) and
> `docs/reference/heap.md` are in place. It exists only to keep the source and
> the port plan in front of us while we implement.

Repo: MiniOS, x86-64 kernel. The frame allocator (`alloc_frame` in
`kernel/memory.c`) now returns real, mapped RAM, but only whole 4096-byte
frames. This task adds a kernel heap (`kmalloc`/`kfree`) that hands out
arbitrary sizes, ported from a complete working allocator whose full source is
embedded below.

The source is included in full. Do not invent the algorithm. Port this exact
code, making only the changes described in the "port" section. Preserve the
original author's comments where they still apply.

## Design of the source allocator

Explicit free list with boundary tags and coalescing:

- Every block has a header (`el_blockhead_t`: size, state, next/prev free-list
  pointers) placed immediately before the user data, and a footer
  (`el_blockfoot_t`: size) immediately after.
- The footer is a boundary tag. It lets `el_block_below` find the previous
  block in memory by stepping back from a header to the previous block's
  footer, reading its size, and jumping back. Without it, coalescing downward
  would scan the whole heap.
- Two doubly-linked lists (available, used), each with dummy begin/end nodes so
  edge cases vanish.
- `el_malloc`: first-fit search (`el_find_first_avail`), split if oversized
  (`el_split_block`), mark used, return pointer just past the header.
- `el_free`: recover header from pointer, move used to available, coalesce with
  the block above (`el_merge_block_with_above`) and the block below (found via
  `el_block_below`, then merged into).
- Split and coalesce are inverses: split adds a header/footer pair, coalesce
  removes one.
- The slab comes from `mmap`; `el_append_pages_to_heap` grows it with more
  `mmap` at a requested contiguous address.

## The port: what changes

Almost nothing. The pointer arithmetic and list logic are OS-independent and
come over verbatim. Only three things change.

### Change 1: the slab source, `mmap` becomes the frame allocator

`el_init` and `el_append_pages_to_heap` call `mmap`. There is no `mmap` in the
kernel, this heap is the layer that would implement it. Replace with
`alloc_frame`.

Contiguity is the one real design decision. The source assumes a contiguous
slab (the boundary-tag walk and `el_append_pages_to_heap` both rely on
`heap_end` being the literal next byte). `alloc_frame` returns one 4096-byte
frame at a time and frames are not guaranteed adjacent.

The frame allocator uses a linear bitmap, so a run of consecutive clear bits is
contiguous physical memory. Add a `alloc_frames_contiguous(n)` helper to
`kernel/memory.c` that finds and reserves `n` consecutive free frames and
returns the base, or 0 if no such run exists. Use it for both the initial slab
and growth. This keeps the source's contiguous-heap assumption valid. If you
cannot make this work, stop and report rather than falling back to a
per-frame-arena design silently.

Also drop the fixed target addresses. The source pins the heap at
`EL_HEAP_START_ADDRESS` (0x0000612000000000) and asserts `mmap` returned exactly
that. In the kernel the heap lives at whatever physical frames the allocator
returns, so `heap_start` = the base returned by `alloc_frames_contiguous`, and
the `heap == EL_HEAP_START_ADDRESS` assertions are removed. Same for the
`el_ctl` control block: the source `mmap`s a page for it at a fixed address;
instead make `el_ctl` a plain static struct in `.bss` (it is a fixed-size
control block, no need to allocate it).

### Change 2: no libc

The source includes `<stdio.h>`, `<stdlib.h>`, `<assert.h>`, `<stdint.h>`,
`<sys/mman.h>`. None exist in the kernel.

- `printf`/`fprintf` become the kernel `print_string` (check `libc/` and
  `drivers/` for the exact name and for an integer-to-string/hex helper; if none
  prints numbers, add a minimal one). Keep the print/stats helpers, they are
  useful for the self-test, just make them kernel-native.
- `assert` becomes a kernel panic helper (print a message + halt) or is dropped
  where it guarded an `mmap` return that no longer exists.
- `uint64_t`/`size_t` come from the kernel's `include/types.h`.
- Remove the `<sys/mman.h>` dependency entirely (change 1 removes its uses).

### Change 3: interrupt safety (the one genuinely new thing)

In the source, one caller touches the allocator at a time. In the kernel, the
timer fires 100x/sec and could land while `kmalloc` is mid-relink of the free
list, and the handler may itself allocate. That corrupts the list.

Wrap the critical sections of `kmalloc` and `kfree` in a save-and-restore
interrupt guard: read the current interrupt flag (from RFLAGS via `pushf`),
`cli`, do the work, then restore the saved flag (do NOT unconditionally `sti`,
you may have been called from inside an interrupt handler where interrupts must
stay off). Add a small helper pair if one does not exist (`irq_save()` returning
the old state, `irq_restore(state)`). Comment loudly why: the free list is
shared mutable state, an interrupt mid-relink corrupts it.

## Naming and files

- Public interface: `void *kmalloc(size_t size)`, `void kfree(void *ptr)`. You
  may keep the internal `el_*` names (they are descriptive and match the
  comments) with `kmalloc`/`kfree` as thin wrappers over `el_malloc`/`el_free`,
  or rename throughout, your call. State which.
- New files: `kernel/heap.c`, `kernel/heap.h`. Do NOT merge into `memory.c`,
  keep frame allocator (pages) and heap (arbitrary sizes on pages) as distinct
  layers. The `alloc_frames_contiguous` helper is the exception, it belongs in
  `memory.c` next to the frame allocator.
- `heap_init()` called from `kernel_main` after `memory_init()`. Initial slab of
  16 pages (64KB) is fine, grow on demand.

## THE SOURCE TO PORT

### el_malloc.h

```c
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <sys/mman.h>
#include <assert.h>

// macro to add a byte offset to a pointer, arguments are a pointer
// and a # of bytes (usually size_t)
#define PTR_PLUS_BYTES(ptr,off) ((void *) (((size_t) (ptr)) + ((size_t) (off))))

// macro to add a byte offset to a pointer, arguments are a pointer
// and a # of bytes (usually size_t)
#define PTR_MINUS_BYTES(ptr,off) ((void *) (((size_t) (ptr)) - ((size_t) (off))))

// macro to add a byte offset to a pointer, arguments are a pointer
// and a # of bytes (usually size_t)
#define PTR_MINUS_PTR(ptr,ptq) ((long) (((size_t) (ptr)) - ((size_t) (ptq))))

// Basic defines for the default size/starting place for the heap
#define EL_PAGE_BYTES (4096)
#define EL_CTL_START_ADDRESS  ((void *) 0x0000610000000000)
#define EL_HEAP_START_ADDRESS ((void *) 0x0000612000000000)
#define EL_HEAP_DEFAULT_SIZE  ((size_t) EL_PAGE_BYTES)

// defines to indicate if a block is available or used
#define EL_AVAILABLE     'a'    // block state indicating available
#define EL_USED          'u'    // block state indicating in use
#define EL_BEGIN_BLOCK   'B'    // block state indicating dummy beginning node in a list
#define EL_END_BLOCK     'E'    // block state indicating dummy ending node in a list
#define EL_UNINITIALIZED  0     // indication of uninitialized data

typedef struct block {
  size_t size;                  // number of bytes of memory in this block
  char state;                   // either EL_AVAILABLE or EL_USED
  struct block *next;           // pointer to next block in same list
  struct block *prev;           // pointer to previous block in same list
} el_blockhead_t;

typedef struct {
  size_t size;
} el_blockfoot_t;

#define EL_BLOCK_OVERHEAD (sizeof(el_blockhead_t) + sizeof(el_blockfoot_t))

typedef struct {
  el_blockhead_t beg_actual;    // fixed node at beginning of list; state is EL_BEGIN_BLOCK
  el_blockhead_t end_actual;    // fixed node at end of list; state is EL_END_BLOCK
  el_blockhead_t *beg;          // pointer to beg_actual
  el_blockhead_t *end;          // pointer to end_actual
  size_t length;                // length of the used block list (not counting beg/end)
  size_t bytes;                 // total bytes in list used including overhead;
} el_blocklist_t;

typedef struct {
  void *heap_start;             // pointer to where the heap starts
  void *heap_end;               // pointer to where the heap ends; this memory address is out of bounds
  size_t heap_bytes;            // number of bytes currently in the heap
  el_blocklist_t avail_actual;  // space for the available list data
  el_blocklist_t used_actual;   // space for the used list data
  el_blocklist_t *avail;        // pointer to avail_actual
  el_blocklist_t *used;         // pointer to used_actual
} el_ctl_t;

extern el_ctl_t *el_ctl;

int  el_init(uint64_t initial_heap_size);
void el_print_heap_blocks();
void el_print_stats();
void el_cleanup();

el_blockfoot_t *el_get_footer(el_blockhead_t *block);
el_blockhead_t *el_get_header(el_blockfoot_t *foot);
el_blockhead_t *el_block_above(el_blockhead_t *block);
el_blockhead_t *el_block_below(el_blockhead_t *block);

void el_init_blocklist(el_blocklist_t *list);
void el_print_blocklist(el_blocklist_t *list);
void el_add_block_front(el_blocklist_t *list, el_blockhead_t *block);
void el_remove_block(el_blocklist_t *list, el_blockhead_t *block);

el_blockhead_t *el_find_first_avail(size_t size);
el_blockhead_t *el_split_block(el_blockhead_t *block, size_t new_size);
el_blockhead_t *el_allocate_block(size_t size);
void *el_malloc(size_t nbytes);

void el_merge_block_with_above(el_blockhead_t *lower);
void el_free(void *ptr);

int el_append_pages_to_heap(int npages);
```

### el_malloc.c

```c
// el_malloc.c: implementation of explicit list malloc functions.

#include "el_malloc.h"

el_ctl_t *el_ctl = NULL;

int el_init(uint64_t initial_heap_size){
  el_ctl =
    mmap(EL_CTL_START_ADDRESS,
         EL_PAGE_BYTES,
         PROT_READ | PROT_WRITE,
         MAP_PRIVATE | MAP_ANONYMOUS,
         -1, 0);
  assert(el_ctl == EL_CTL_START_ADDRESS);

  void *heap =
    mmap(EL_HEAP_START_ADDRESS,
         initial_heap_size,
         PROT_READ | PROT_WRITE,
         MAP_PRIVATE | MAP_ANONYMOUS,
         -1, 0);
  assert(heap == EL_HEAP_START_ADDRESS);

  el_ctl->heap_bytes = initial_heap_size;    // make the heap as big as possible to begin with
  el_ctl->heap_start = heap;                 // set addresses of start and end of heap
  el_ctl->heap_end   = PTR_PLUS_BYTES(heap,el_ctl->heap_bytes);

  if(el_ctl->heap_bytes < EL_BLOCK_OVERHEAD){
    fprintf(stderr,"el_init: heap size %ld to small for a block overhead %ld\n",
            el_ctl->heap_bytes,EL_BLOCK_OVERHEAD);
    return 1;
  }

  el_init_blocklist(&el_ctl->avail_actual);
  el_init_blocklist(&el_ctl->used_actual);
  el_ctl->avail = &el_ctl->avail_actual;
  el_ctl->used  = &el_ctl->used_actual;

  size_t size = el_ctl->heap_bytes - EL_BLOCK_OVERHEAD;
  el_blockhead_t *ablock = el_ctl->heap_start;
  ablock->size = size;
  ablock->state = EL_AVAILABLE;
  el_blockfoot_t *afoot = el_get_footer(ablock);
  afoot->size = size;

  ablock->prev = el_ctl->avail->beg;
  ablock->next = el_ctl->avail->beg->next;
  ablock->prev->next = ablock;
  ablock->next->prev = ablock;
  el_ctl->avail->length++;
  el_ctl->avail->bytes += (ablock->size + EL_BLOCK_OVERHEAD);

  return 0;
}

void el_cleanup(){
  munmap(el_ctl->heap_start, el_ctl->heap_bytes);
  munmap(el_ctl, EL_PAGE_BYTES);
}

el_blockfoot_t *el_get_footer(el_blockhead_t *head){
  size_t size = head->size;
  el_blockfoot_t *foot = PTR_PLUS_BYTES(head, sizeof(el_blockhead_t) + size);
  return foot;
}

el_blockhead_t *el_get_header(el_blockfoot_t *foot){ // move backward in memory from the foot to reach the head.
  el_blockhead_t *head = (el_blockhead_t *) PTR_MINUS_BYTES(foot, sizeof(el_blockhead_t) + (foot->size));
  return head;
}

el_blockhead_t *el_block_above(el_blockhead_t *block){
  el_blockhead_t *higher = PTR_PLUS_BYTES(block, block->size + EL_BLOCK_OVERHEAD);
  if((void *) higher >= (void*) el_ctl->heap_end){
    return NULL;
  }
  else{
    return higher;
  }
}

el_blockhead_t *el_block_below(el_blockhead_t *block){
  if((void *)block <= el_ctl->heap_start){ //check if block at start
    return NULL;
  }
  el_blockfoot_t *footer = (el_blockfoot_t *) PTR_MINUS_BYTES(block, sizeof(el_blockfoot_t)); // gets footer of previous
  el_blockhead_t *lower = el_get_header(footer); //get header from footer
  return lower;
}

void el_print_blocklist(el_blocklist_t *list){
  printf("{length: %3lu  bytes: %5lu}\n", list->length,list->bytes);
  el_blockhead_t *block = list->beg;
  for(int i=0; i<list->length; i++){
    printf("  ");
    block = block->next;
    printf("[%3d] head @ %p ", i, block);
    printf("{state: %c  size: %5lu}\n", block->state,block->size);
  }
}

void el_print_block(el_blockhead_t *block){
  el_blockfoot_t *foot = el_get_footer(block);
  printf("%p\n", block);
  printf("  state:      %c\n", block->state);
  printf("  size:       %lu (total: 0x%lx)\n", block->size, block->size+EL_BLOCK_OVERHEAD);
  printf("  prev:       %p\n", block->prev);
  printf("  next:       %p\n", block->next);
  printf("  user:       %p\n", PTR_PLUS_BYTES(block,sizeof(el_blockhead_t)));
  printf("  foot:       %p\n", foot);
  printf("  foot->size: %lu\n", foot->size);
}

void el_print_heap_blocks(){
  int i = 0;
  el_blockhead_t *cur = el_ctl->heap_start;
  while(cur != NULL){
    printf("[%3d] @ ",i);
    el_print_block(cur);
    cur = el_block_above(cur);
    i++;
  }
}

void el_print_stats(){
  printf("HEAP STATS (overhead per node: %lu)\n",EL_BLOCK_OVERHEAD);
  printf("heap_start:  %p\n",el_ctl->heap_start);
  printf("heap_end:    %p\n",el_ctl->heap_end);
  printf("total_bytes: %lu\n",el_ctl->heap_bytes);
  printf("AVAILABLE LIST: ");
  el_print_blocklist(el_ctl->avail);
  printf("USED LIST: ");
  el_print_blocklist(el_ctl->used);
  printf("HEAP BLOCKS:\n");
  el_print_heap_blocks();
}

void el_init_blocklist(el_blocklist_t *list){
  list->beg        = &(list->beg_actual);
  list->beg->state = EL_BEGIN_BLOCK;
  list->beg->size  = EL_UNINITIALIZED;
  list->end        = &(list->end_actual);
  list->end->state = EL_END_BLOCK;
  list->end->size  = EL_UNINITIALIZED;
  list->beg->next  = list->end;
  list->beg->prev  = NULL;
  list->end->next  = NULL;
  list->end->prev  = list->beg;
  list->length     = 0;
  list->bytes      = 0;
}

void el_add_block_front(el_blocklist_t *list, el_blockhead_t *block){
  el_blockhead_t *start = list->beg;
  el_blockhead_t *first = start->next;
  block->next = first; // First, attach the new block to the existing first block
  first->prev = block;
  block->prev = start;   // Then connect new block back to start
  start->next = block;
  list->length += 1; // Update list metadata
  list->bytes  += block->size + EL_BLOCK_OVERHEAD;
}

void el_remove_block(el_blocklist_t *list, el_blockhead_t *block){
  el_blockhead_t *prev = block->prev; // get the blocks it is connected to
  el_blockhead_t *next = block->next;
  prev->next = next; // change the ones next to the block we try to remove
  next->prev = prev;
  list->length-=1;  // update metadata
  list->bytes -= block->size + EL_BLOCK_OVERHEAD;
  block->next = NULL; // get rid of the connections
  block->prev = NULL;
}

el_blockhead_t *el_find_first_avail(size_t size){
  el_blockhead_t *f = el_ctl->avail->beg->next;
  while (f->state != EL_END_BLOCK) { // look through the blocks to find the first open one block
    if (f->state == EL_AVAILABLE) { // if it is open, you check size
      if(f->size>=size){ // if size matches, you have found your guy
        return f;
      }
    }
    f = f->next;
  }
  return NULL;
}

el_blockhead_t *el_split_block(el_blockhead_t *block, size_t new_size){
  size_t olds = block->size;
  el_blockfoot_t *oldf = el_get_footer(block);
  if (olds < new_size + EL_BLOCK_OVERHEAD){ //size check to see if we have enough space
    return NULL;
  }
  size_t left = olds - new_size - EL_BLOCK_OVERHEAD;
  block->size = new_size;
  el_blockfoot_t *shrunkf = el_get_footer(block); //creates footer for shrunk block
  shrunkf->size = new_size;
  oldf->size = left; // just make the old footer the one for the new one by updating size for new
  el_blockhead_t *newb = el_get_header(oldf);
  newb->size = left;
  return newb;
}

void *el_malloc(size_t nbytes){
  el_blockhead_t *b = el_find_first_avail(nbytes); // find the first available block that is large enough.
  if (b==NULL){ // check if found
    return NULL;
  }
  el_remove_block(el_ctl->avail, b); // get rid of it from available
  el_blockhead_t *newb = el_split_block(b, nbytes);
  b->state = EL_USED; // change to in use since it will hold parameter
  el_add_block_front(el_ctl->used, b);
  if (newb != NULL) { // the bit of memory not used is created into new block and added to available
    newb->state = EL_AVAILABLE;
    el_add_block_front(el_ctl->avail, newb);
  }
  return PTR_PLUS_BYTES(b, sizeof(el_blockhead_t));
}

void el_merge_block_with_above(el_blockhead_t *lower){
  if(lower == NULL){ // all the lower value checks
    return;
  }
  if(lower->state != EL_AVAILABLE){
    return;
  }
  el_blockhead_t *up = el_block_above(lower); // gets the block above lower
  if(up == NULL){ // all the checks needed for that up one
    return;
  }
  if(up->state != EL_AVAILABLE){
    return;
  }
  el_remove_block(el_ctl->avail, lower); // getting on with the merging
  el_remove_block(el_ctl->avail, up);
  lower->size = lower->size + EL_BLOCK_OVERHEAD + up->size;
  el_blockfoot_t *newf = el_get_footer(lower);
  newf->size = lower->size;
  el_add_block_front(el_ctl->avail, lower);
}

void el_free(void *ptr){
  if (ptr == NULL){ //inavlid pointer check
    printf("ERROR: el_free() not called on an EL_USED block\n");
    return;
  }
  el_blockhead_t *block = (el_blockhead_t *) PTR_MINUS_BYTES(ptr, sizeof(el_blockhead_t)); //comvert to block pointer
  if (block->state != EL_USED||block->state=='\0') { //check if it is used
    printf("ERROR: el_free() not called on an EL_USED block\n");
    return;
  }
  el_remove_block(el_ctl->used, block);
  block->state = EL_AVAILABLE; // mark as available
  el_add_block_front(el_ctl->avail, block);
  el_merge_block_with_above(block); //try to merge whatever is near it that is available
  el_blockhead_t *lower = el_block_below(block);
  el_merge_block_with_above(lower);
}

int el_append_pages_to_heap(int npages) {
  size_t nbytes = (size_t) npages * EL_PAGE_BYTES;
  void *mapped = mmap(el_ctl->heap_end, nbytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); // add the page to the end of the heap
  if(mapped == MAP_FAILED){     // Fail if mmap fails or if it mapped somewhere else
    fprintf(stderr, "ERROR: Unable to mmap() additional %d pages\n", npages);
    return 1;
  }
  if(mapped!=el_ctl->heap_end){
    fprintf(stderr, "ERROR: Unable to mmap() additional %d pages\n", npages);
    return 1;
  }
  el_blockhead_t *b = (el_blockhead_t *) el_ctl->heap_end;
  size_t size = nbytes - EL_BLOCK_OVERHEAD;
  el_blockfoot_t *f = (el_blockfoot_t *) PTR_PLUS_BYTES(b, sizeof(el_blockhead_t) + size); // Compute footer address at the end of the block
  b->size  = size;
  f->size = size;
  b->state = EL_AVAILABLE;   // intialize all values
  b->prev  = NULL;
  b->next  = NULL;
  el_ctl->heap_bytes += nbytes;
  el_ctl->heap_end = PTR_PLUS_BYTES(el_ctl->heap_end, nbytes);
  el_add_block_front(el_ctl->avail, b);   // Add the new free block to the available list
  el_blockhead_t *below = el_block_below(b);
  el_merge_block_with_above(below);
  return 0;
}
```

## Per-function port checklist

- `el_get_footer`, `el_get_header`, `el_block_above`, `el_block_below`,
  `el_init_blocklist`, `el_add_block_front`, `el_remove_block`,
  `el_find_first_avail`, `el_split_block`, `el_malloc`,
  `el_merge_block_with_above`, `el_free`: verbatim, only swap `printf` in
  `el_free`'s error path for the kernel print. The `PTR_*` macros come over
  unchanged.
- `el_init`: replace both `mmap` calls. `el_ctl` becomes a static struct (no
  allocation). `heap_start` = `alloc_frames_contiguous(initial_pages)`. Drop the
  fixed addresses and asserts. `fprintf` becomes kernel print.
- `el_append_pages_to_heap`: replace `mmap` with `alloc_frames_contiguous(npages)`.
  Because frames may not sit exactly at `heap_end`, the contiguous-grow
  assumption can fail. If the new run is not adjacent to `heap_end`, treat the
  new run as a fresh standalone free block (do NOT merge with below), and
  comment that non-adjacent growth is expected and handled. Report how you
  handled it.
- `el_cleanup`: stub or remove, the kernel heap never tears down.
- `el_print_*`: keep, make kernel-native (needed for the self-test coalesce
  check).
- `el_malloc`/`el_free` public wrappers `kmalloc`/`kfree` add the interrupt
  guard.

## Prove it works (temporary self-test in kernel_main, removed after, tree clean)

1. `kmalloc` three different sizes, write a distinct sentinel into each, read
   back, confirm match. Proves allocation and real writable memory.
2. `kfree` the middle one, `kmalloc` the same size, confirm the freed block is
   reused (same pointer back). Proves free returns blocks to the list.
3. `kfree` all three (adjacent) and confirm they coalesce: use `el_print_stats`
   / the available-list length to show the free-block count dropped to one large
   block.
4. Allocate past the initial 16-page slab to force the growth path, confirm it
   works.

Print PASS/FAIL per check. Report the actual output.

## Verify

- `make` builds clean under `-Wall -Wextra`.
- Boot in QEMU, the `ABAB` scheduler still runs after the self-test.
- `-d int -no-reboot -no-shutdown`: no page faults (`0x0E`) or GP faults
  (`0x0D`) from the heap test, timer/syscall vectors still fire.
- A page fault during the test most likely means non-contiguous frames, report
  and resolve.
- Confirm self-test removed, tree clean.

## Docs

- New ADR `docs/decisions/0010-kernel-heap-ported-from-p5.md` (Nygard,
  Accepted): context (frame allocator only does whole pages, fixed 4-task array
  exists for lack of a heap), decision (port this explicit-free-list allocator,
  `mmap` to `alloc_frames_contiguous`, add interrupt guard), consequences
  (arbitrary-size kernel allocation unblocks dynamic tasks and per-process page
  tables; first-fit fragmentation accepted; interrupt guard serialises
  allocation; the contiguity approach and its limits).
- New `docs/reference/heap.md`: header/footer boundary tags, split/coalesce as
  inverses, why the footer enables backward walking, the frame-allocator seam,
  the interrupt-safety requirement.
- Update `docs/architecture.md`, `docs/project-status.md`, `docs/README.md`
  decisions index (add 0010), `CHANGELOG.md`.
- Do not rewrite `learnings/` chapters.

## Style

Match existing style. No em dashes, use commas or parentheses. Named constants
only. Comment why, not what. Preserve the original author's comments where they
apply.

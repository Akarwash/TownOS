// heap.c: kernel heap, ported from the CMSC216 p5 el_malloc (an explicit
// free-list allocator with boundary tags and coalescing).
//
// The algorithm is the original author's, unchanged. Only three things differ
// from the p5 source, all forced by running in a kernel rather than a process:
//   1. The slab comes from the frame allocator (alloc_frames_contiguous), not
//      mmap, and lives at whatever physical frames it returns, not a fixed
//      virtual address. el_ctl is a plain .bss struct, not an mmap'd page.
//   2. There is no libc: printf/fprintf become the VGA print_string, assert and
//      the mmap-return checks are gone.
//   3. kmalloc/kfree add an interrupt-disable guard: the free list is shared
//      mutable state and the timer IRQ (100 Hz) could land mid-relink.
//
// See docs/reference/heap.md and docs/decisions/0010-kernel-heap-ported-from-p5.md.

#include "heap.h"
#include "memory.h"
#include "../drivers/screen.h"

// ============================================================================
// Pointer arithmetic macros (verbatim from the p5 source).
// ============================================================================
// macro to add a byte offset to a pointer, arguments are a pointer
// and a # of bytes (usually size_t)
#define PTR_PLUS_BYTES(ptr,off) ((void *) (((size_t) (ptr)) + ((size_t) (off))))

// macro to subtract a byte offset from a pointer, arguments are a pointer
// and a # of bytes (usually size_t)
#define PTR_MINUS_BYTES(ptr,off) ((void *) (((size_t) (ptr)) - ((size_t) (off))))

// The heap page size equals the frame allocator's frame size: the slab is built
// out of whole frames.
#define EL_PAGE_BYTES FRAME_SIZE

// Initial slab (16 pages = 64KB) and the default growth chunk. The heap grows
// on demand when a request does not fit; growth is at least this many pages.
#define HEAP_INITIAL_PAGES 16
#define HEAP_GROW_PAGES    16

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

// The control block is a fixed-size struct: no reason to allocate it. In the p5
// source it was mmap'd at a fixed address; here it lives in .bss (zero-init) and
// el_ctl points at it, so all the el_ctl-> code below comes over untouched.
static el_ctl_t el_ctl_actual;
static el_ctl_t *el_ctl = &el_ctl_actual;

// ============================================================================
// Interrupt-safety guard.
// ============================================================================
// The available/used lists are shared mutable state. The timer fires 100x/sec,
// and its handler (or the scheduler it drives) could call kmalloc while another
// kmalloc is halfway through relinking the list, which corrupts it. kmalloc and
// kfree therefore disable interrupts across their critical section.
//
// This is SAVE-and-RESTORE, not an unconditional sti: kmalloc/kfree may be
// called from inside an interrupt handler where interrupts are already off, and
// forcing them back on there would re-enter the very code we are protecting.
static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ __volatile__("pushfq\n\t"
                         "pop %0\n\t"
                         "cli"
                         : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ __volatile__("push %0\n\t"
                         "popfq"
                         : : "r"(flags) : "memory", "cc");
}

// ============================================================================
// Kernel-native print helpers (replace the p5 printf/fprintf).
// ============================================================================
static void print_dec(uint64_t n) {
    char buf[21];               // 2^64 is 20 digits
    int i = 0;
    if (n == 0) {
        print_char('0');
        return;
    }
    while (n > 0) {
        buf[i++] = '0' + (char)(n % 10);
        n /= 10;
    }
    while (i > 0) {
        print_char(buf[--i]);
    }
}

// print_hex used to be a second static copy right here. It now lives in the
// screen driver (drivers/screen.c) because the ELF loader needed the same thing,
// and two identical hex printers in one kernel is one too many.

// ============================================================================
// Boundary-tag address arithmetic (verbatim from the p5 source).
// ============================================================================

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

// ============================================================================
// Doubly-linked list surgery (verbatim from the p5 source).
// ============================================================================

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

// ============================================================================
// Allocation: first-fit search, split, mark used (verbatim core from p5).
// ============================================================================

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

// ============================================================================
// Free and coalesce (verbatim core from p5; only the error printf is native).
// ============================================================================

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
    print_string("ERROR: el_free() not called on an EL_USED block\n");
    return;
  }
  el_blockhead_t *block = (el_blockhead_t *) PTR_MINUS_BYTES(ptr, sizeof(el_blockhead_t)); //comvert to block pointer
  if (block->state != EL_USED||block->state=='\0') { //check if it is used
    print_string("ERROR: el_free() not called on an EL_USED block\n");
    return;
  }
  el_remove_block(el_ctl->used, block);
  block->state = EL_AVAILABLE; // mark as available
  el_add_block_front(el_ctl->avail, block);
  el_merge_block_with_above(block); //try to merge whatever is near it that is available
  el_blockhead_t *lower = el_block_below(block);
  el_merge_block_with_above(lower);
}

// ============================================================================
// Slab construction and growth: mmap becomes alloc_frames_contiguous.
// ============================================================================

// Build the initial slab and its single free block. The p5 source mmap'd a page
// for el_ctl and a slab at a fixed address; here el_ctl is static and the slab
// is whatever contiguous run of frames the allocator returns.
static int el_init(uint64_t initial_heap_size){
  uint32_t npages = (uint32_t)(initial_heap_size / EL_PAGE_BYTES);
  void *heap = (void *) alloc_frames_contiguous(npages);
  if(heap == NULL){
    print_string("el_init: could not get initial slab from frame allocator\n");
    return 1;
  }

  el_ctl->heap_bytes = initial_heap_size;    // make the heap as big as possible to begin with
  el_ctl->heap_start = heap;                 // set addresses of start and end of heap
  el_ctl->heap_end   = PTR_PLUS_BYTES(heap,el_ctl->heap_bytes);

  if(el_ctl->heap_bytes < EL_BLOCK_OVERHEAD){
    print_string("el_init: heap size too small for a block overhead\n");
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

int el_append_pages_to_heap(int npages) {
  size_t nbytes = (size_t) npages * EL_PAGE_BYTES;
  void *mapped = (void *) alloc_frames_contiguous((uint32_t) npages); // grow the slab
  if(mapped == NULL){     // Fail if no contiguous run of that length is free
    print_string("heap grow: no contiguous run of frames available\n");
    return 1;
  }
  // The p5 heap is ONE contiguous slab: heap_end must be the literal next byte,
  // because el_block_above/el_block_below walk memory linearly and are bounded by
  // a single heap_end/heap_start pair. The frame allocator scans its bitmap
  // bottom-up and this heap is its only consumer, so a growth run comes back
  // immediately above heap_end (adjacent) by construction. If it ever did not,
  // splicing a disjoint region into the single-heap walk would let those walks
  // step into the gap between regions and read garbage, so we refuse and reclaim
  // the frames rather than corrupt the heap. In this kernel this branch does not
  // trigger; it exists so a future non-adjacent case fails loudly, not silently.
  if(mapped != el_ctl->heap_end){
    print_string("heap grow: non-adjacent run, refusing to fragment heap\n");
    for(uint32_t k = 0; k < (uint32_t) npages; k++){
      free_frame((uint64_t) mapped + (uint64_t) k * EL_PAGE_BYTES);
    }
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

// ============================================================================
// Debug/stats helpers (kept from p5, made kernel-native).
// ============================================================================

static void el_print_blocklist(el_blocklist_t *list){
  print_string("{length: ");
  print_dec(list->length);
  print_string("  bytes: ");
  print_dec(list->bytes);
  print_string("}\n");
  el_blockhead_t *block = list->beg;
  for(size_t i = 0; i < list->length; i++){
    block = block->next;
    print_string("  [");
    print_dec(i);
    print_string("] head @ ");
    print_hex((uint64_t) block);
    print_string(" {state: ");
    print_char(block->state);
    print_string("  size: ");
    print_dec(block->size);
    print_string("}\n");
  }
}

static void el_print_block(el_blockhead_t *block){
  el_blockfoot_t *foot = el_get_footer(block);
  print_hex((uint64_t) block);
  print_string("\n  state: ");
  print_char(block->state);
  print_string("  size: ");
  print_dec(block->size);
  print_string("  foot->size: ");
  print_dec(foot->size);
  print_string("\n");
}

static void el_print_heap_blocks(){
  int i = 0;
  el_blockhead_t *cur = el_ctl->heap_start;
  while(cur != NULL){
    print_string("[");
    print_dec((uint64_t) i);
    print_string("] @ ");
    el_print_block(cur);
    cur = el_block_above(cur);
    i++;
  }
}

void heap_print_stats(){
  print_string("HEAP STATS (overhead per node: ");
  print_dec(EL_BLOCK_OVERHEAD);
  print_string(")\nheap_start:  ");
  print_hex((uint64_t) el_ctl->heap_start);
  print_string("\nheap_end:    ");
  print_hex((uint64_t) el_ctl->heap_end);
  print_string("\ntotal_bytes: ");
  print_dec(el_ctl->heap_bytes);
  print_string("\nAVAILABLE LIST: ");
  el_print_blocklist(el_ctl->avail);
  print_string("USED LIST: ");
  el_print_blocklist(el_ctl->used);
  print_string("HEAP BLOCKS:\n");
  el_print_heap_blocks();
}

// ============================================================================
// Public interface: kmalloc/kfree/heap_init and the two accessors.
// ============================================================================

void heap_init(void){
  if(el_init((uint64_t) HEAP_INITIAL_PAGES * EL_PAGE_BYTES) != 0){
    print_string("heap_init: FAILED to build initial slab\n");
  }
}

// kmalloc: allocate `size` bytes. If no block fits, grow the slab and retry.
// The el_malloc core is unchanged; the grow-and-retry policy and the interrupt
// guard live here in the wrapper so el_malloc stays the pure p5 algorithm.
void *kmalloc(size_t size){
  uint64_t flags = irq_save();   // free list is shared state, keep the timer out
  void *p = el_malloc(size);
  if(p == NULL){
    // No block large enough. Grow by at least HEAP_GROW_PAGES, or more if the
    // request itself needs more than that, then try once more.
    size_t need = size + EL_BLOCK_OVERHEAD;
    int npages = (int)((need + EL_PAGE_BYTES - 1) / EL_PAGE_BYTES);
    if(npages < HEAP_GROW_PAGES){
      npages = HEAP_GROW_PAGES;
    }
    if(el_append_pages_to_heap(npages) == 0){
      p = el_malloc(size);
    }
  }
  irq_restore(flags);
  return p;
}

void kfree(void *ptr){
  uint64_t flags = irq_save();
  el_free(ptr);
  irq_restore(flags);
}

size_t heap_avail_count(void){
  return el_ctl->avail->length;
}

size_t heap_total_bytes(void){
  return el_ctl->heap_bytes;
}

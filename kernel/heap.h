#ifndef HEAP_H
#define HEAP_H

#include "../include/types.h"

// ============================================================================
// A kernel heap: arbitrary-size allocation on top of the frame allocator.
// ============================================================================
// Ported from the CMSC216 p5 el_malloc, an explicit-free-list allocator with
// boundary tags and coalescing. The algorithm is unchanged; only the slab
// source (mmap becomes alloc_frames_contiguous), the libc dependencies, and
// interrupt safety differ. See docs/reference/heap.md and
// docs/decisions/0010-kernel-heap-ported-from-p5.md.
//
// Layering: the frame allocator (kernel/memory.c) hands out whole 4KB pages;
// this heap sits on top and hands out arbitrary byte sizes carved from those
// pages. Keep the two layers distinct.

// Build the initial slab and free list. Call once from kernel_main, AFTER
// memory_init (the heap draws its pages from the frame allocator).
void heap_init(void);

// The public allocation interface. kmalloc/kfree are thin wrappers over the
// ported el_malloc/el_free that add an interrupt-disable guard around the
// critical section (the free list is shared mutable state; a timer interrupt
// landing mid-relink would corrupt it). See heap.c.
void *kmalloc(size_t size);
void  kfree(void *ptr);

// Debug/stats helpers, kept from the p5 source and made kernel-native. Useful
// for inspecting the heap (the self-test uses the two accessors below).
void   heap_print_stats(void);
size_t heap_avail_count(void);   // number of blocks on the available list
size_t heap_total_bytes(void);   // current heap size in bytes (grows on demand)

// Bytes currently allocated: the sum of the payload sizes of every in-use block,
// found by walking the heap block by block rather than trusting a cached counter.
// This is the leak metric that can see SMALL objects. heap_total_bytes only reports
// the slab size, so it catches a leaked pipe_t (~4KB, twenty of which outgrow the
// 64KB slab) but is blind to a leaked file_t (~24 bytes); this sums the used blocks
// directly, so a handful of small leaks show up. Slow and off every allocation path,
// exactly like frame_free_count and fat32_free_count. Returns (uint64_t)-1 if the
// heap walk fails to terminate (a corrupt heap), rather than looping forever.
uint64_t heap_used_bytes(void);

#endif

/*
 * vm/frame.c
 *
 * Physical Frame Management System
 *
 * This file acts as the physical memory manager for the virtual memory system.
 * It maintains a global "Frame Table" to track all physical pages currently 
 * allocated to user processes and implements the page replacement policy 
 * (eviction) when the system runs out of memory.
 *
 * Key Responsibilities and Functionalities:
 *
 * 1. Frame Table Management:
 * - Maintains a global linked list (`frame_table`) of `struct frame` objects.
 * - Each entry represents a physical page frame and links it back to the 
 * virtual page (`struct page`) that currently occupies it.
 *
 * 2. Frame Allocation (`vm_get_frame`):
 * - Serves as a wrapper around the low-level `palloc_get_page`.
 * - If physical memory is available, it allocates a frame, registers it in 
 * the table, and returns it.
 * - If physical memory is exhausted (FULL), it triggers the eviction process 
 * (`vm_evict_frame`) to swap out an existing page and make room for the new one.
 *
 * 3. Page Replacement Algorithm (Clock Algorithm):
 * - Implements the "Clock Algorithm" (Second Chance Algorithm) in `vm_evict_frame`.
 * - Iterates through the frame table circularly.
 * - Uses the hardware "Accessed Bit" (`pagedir_is_accessed`) to approximate LRU.
 * - If a page has been accessed recently, it gets a "second chance" (bit cleared).
 * - If a page has not been accessed, it is selected as the "victim" for eviction.
 *
 * 4. Concurrency Control & Frame Theft Protection:
 * - Uses `frame_lock` to synchronize access to the global frame table.
 * - Implements a critical check during eviction: `if (victim->page == NULL)`. 
 * This condition identifies frames that are currently being allocated and 
 * filled with data (e.g., during disk I/O) but are not yet fully mapped. 
 * These frames are skipped to prevent "Frame Theft," ensuring that a thread 
 * loading data doesn't have its frame stolen by another thread before completion.
 */

#include <string.h>
#include "vm/frame.h"
#include "vm/page.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"


static struct list frame_table;
struct lock frame_lock;

void
vm_frame_init (void) {
    list_init (&frame_table);
    lock_init (&frame_lock);
}

static struct frame *vm_evict_frame (void);

struct frame *
vm_get_frame (void) {
    struct frame *frame = malloc (sizeof (struct frame));
    if (frame == NULL) return NULL;

    frame->kva = palloc_get_page (PAL_USER | PAL_ZERO);
    frame->page = NULL;
    
    if (frame->kva == NULL) {
        free(frame); 
        frame = vm_evict_frame(); // 쫓아내고 빈 프레임 받아오기
        frame->page = NULL; 
        return frame;
    }

    lock_acquire (&frame_lock);
    list_push_back (&frame_table, &frame->elem);
    lock_release (&frame_lock);

    return frame;
}

static struct frame *
vm_evict_frame (void) {
    struct frame *victim = NULL;
    struct list_elem *e;
    
    lock_acquire (&frame_lock);

    if (list_empty(&frame_table)) {
        PANIC("Frame table is empty, but memory is full!");
    }
    
    e = list_begin (&frame_table);

    int loop_cnt = 0;
    
    while (true) {
        if (e == list_end (&frame_table)) {
            e = list_begin (&frame_table);
        }

        if (loop_cnt++ > 10000) {
            PANIC("Infinite loop in vm_evict_frame!");
        }

        victim = list_entry (e, struct frame, elem);
        
        if (victim->page == NULL) {
            e = list_next (e);
            continue; 
        }

        struct thread *t = victim->page->thread; 
        
        if (pagedir_is_accessed (t->pagedir, victim->page->va)) {
            pagedir_set_accessed (t->pagedir, victim->page->va, false);
        } else {
            break; 
        }
        
        e = list_next (e);
    }
    
    
    if (!victim->page->operations->swap_out (victim->page)) {
        // 스왑 실패 시 패닉
        PANIC("Swap out failed!"); 
    }

    victim->page->frame = NULL; 
    victim->page = NULL;       
    lock_release (&frame_lock);
    
    memset(victim->kva, 0, PGSIZE);
    return victim;
}
/*
 * vm/page.c
 *
 * Virtual Memory Page Management System
 *
 * This file implements the core abstraction of a "Virtual Page" in the Pintos 
 * virtual memory system. It is responsible for managing the Supplemental Page 
 * Table (SPT) for each process and defining the behavior of different page 
 * types (Anonymous and File-backed).
 *
 * Key Responsibilities and Functionalities:
 *
 * 1. Supplemental Page Table (SPT) Management:
 * - Implements a hash-table-based container (`struct hash spt`) to track 
 * all virtual memory pages belonging to a thread.
 * - Provides functions to initialize (`spt_init`), search (`spt_find_page`), 
 * insert (`spt_insert_page`), and destroy (`spt_kill`) the SPT.
 * - This table is crucial for the Page Fault Handler to determine the state 
 * and location of a page (e.g., on disk, in swap, or uninitialized) 
 * when a fault occurs.
 *
 * 2. Polymorphic Page Operations:
 * - Defines a unified interface (`struct page_operations`) for handling 
 * different types of pages:
 * a. Anonymous Pages (VM_ANON):
 * - Used for stacks, heaps, and executable data segments (after modification).
 * - Supports swapping to/from the swap disk via `vm/swap.c`.
 * - `anon_swap_in`: Restores data from the swap disk or initializes zero pages.
 * - `anon_swap_out`: Evicts data to the swap disk and saves the slot index.
 * b. File-Backed Pages (VM_FILE):
 * - Used for memory-mapped files (mmap) and lazy-loading executables.
 * - Supports reading from and writing back to the file system.
 * - `file_backed_swap_in`: Reads data from the file at a specific offset.
 * - `file_backed_swap_out`: Writes dirty pages back to the file system 
 * to ensure data persistence before eviction.
 *
 * 3. Page Lifecycle and Allocation:
 * - `vm_alloc_page_with_initializer`: Handles the "Lazy Loading" mechanism. 
 * It creates a page in the `VM_UNINIT` state, registering an initializer 
 * function. The actual memory allocation and data loading happen only 
 * when the page is accessed (Page Fault).
 * - `vm_deinit_page` & `destroy`: Handles the cleanup of resources, including 
 * freeing physical frames, releasing swap slots, and flushing file changes 
 * when a process exits or unmaps memory.
 *
 * 4. Synchronization and Hardware Interaction:
 * - Interacts with the hardware page table (pagedir) to set/clear dirty bits 
 * and presence bits.
 * - Uses `frame_lock` and `filesys_lock` to ensure thread safety during 
 * frame eviction and file I/O operations.
 */

#include "vm/page.h"
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "vm/swap.h"
#include "vm/frame.h"
#include "vm/vm.h"
#include "threads/synch.h"

extern struct lock frame_lock;
extern struct lock filesys_lock;

/* Forward Declarations */
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* Page Operations Tables */
static const struct page_operations anon_ops = {
    .swap_in = anon_swap_in,
    .swap_out = anon_swap_out,
    .destroy = anon_destroy,
    .type = VM_ANON,
};

static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};

static unsigned page_hash (const struct hash_elem *p_, void *aux UNUSED) {
    const struct page *p = hash_entry (p_, struct page, hash_elem);
    return hash_bytes (&p->va, sizeof p->va);
}

static bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
    const struct page *a = hash_entry (a_, struct page, hash_elem);
    const struct page *b = hash_entry (b_, struct page, hash_elem);
    return a->va < b->va;
}

void spt_init (struct hash *spt) {
    hash_init (spt, page_hash, page_less, NULL);
}

struct page *spt_find_page (struct hash *spt, void *va) {
    struct page page;
    page.va = pg_round_down (va);
    struct hash_elem *e = hash_find (spt, &page.hash_elem);
    return e ? hash_entry (e, struct page, hash_elem) : NULL;
}

bool spt_insert_page (struct hash *spt, struct page *page) {
    return hash_insert (spt, &page->hash_elem) == NULL;
}

void vm_deinit_page (struct page *page) {
    if (page->operations && page->operations->destroy) {
        page->operations->destroy (page);
    }
}

void spt_remove_page (struct hash *spt, struct page *page) {
    hash_delete (spt, &page->hash_elem);
    vm_deinit_page (page);
    free (page);
}

static void spt_destroy_func (struct hash_elem *e, void *aux UNUSED) {
    struct page *page = hash_entry (e, struct page, hash_elem);
    vm_deinit_page (page); 
    free (page);
}

void spt_kill (struct hash *spt) {
    hash_destroy (spt, spt_destroy_func);
}

/* --- Anonymous Page Functions --- */

bool anon_initializer (struct page *page, enum vm_type type UNUSED, void *kva UNUSED) {
    page->operations = &anon_ops;
    page->anon.swap_slot_idx = SIZE_MAX;
    return true;
}

static bool anon_swap_in (struct page *page, void *kva) {
    size_t index = page->anon.swap_slot_idx;
    /* 새 페이지거나 이미 free된 경우 0으로 초기화 */
    if (index == SIZE_MAX) {
        memset(kva, 0, PGSIZE);
        return true; 
    }
    return swap_in (index, kva);
}

static bool anon_swap_out (struct page *page) {
    if (page == NULL || page->frame == NULL) return false;
    size_t index = swap_out (page->frame->kva);
    if (index == SIZE_MAX) return false; 

    page->anon.swap_slot_idx = index;
    pagedir_clear_page (page->thread->pagedir, page->va);
    page->frame = NULL;
    return true;
}

static void anon_destroy (struct page *page) {
    if (page->frame != NULL) {
        struct frame *f = page->frame;
        struct thread *t = page->thread;
        if (t->pagedir != NULL) {
            pagedir_clear_page(t->pagedir, page->va);
        }
        lock_acquire(&frame_lock);
        list_remove(&f->elem);
        lock_release(&frame_lock);
        palloc_free_page(f->kva);
        free(f);
        page->frame = NULL;
    }
    if (page->anon.swap_slot_idx != SIZE_MAX) {
        swap_free (page->anon.swap_slot_idx);
    }
}


bool file_backed_initializer (struct page *page, enum vm_type type UNUSED, void *aux) {
    struct mmap_info_aux *info = (struct mmap_info_aux *)aux;
    page->operations = &file_ops;
    page->file.file = info->file;
    page->file.ofs = info->ofs;
    page->file.read_bytes = info->read_bytes;
    page->file.zero_bytes = info->zero_bytes;
    free(info);
    return file_backed_swap_in(page, page->frame->kva);
} 

static bool file_backed_swap_in (struct page *page, void *kva) {
    struct file_page *file_page = &page->file;
    if (file_page->file == NULL) return false;

    lock_acquire(&filesys_lock);
    off_t read_bytes = file_read_at(file_page->file, kva, 
                                    file_page->read_bytes, file_page->ofs);
    lock_release(&filesys_lock);

    if (read_bytes != (int)file_page->read_bytes) return false;
    memset(kva + file_page->read_bytes, 0, file_page->zero_bytes);
    return true;
}

static bool file_backed_swap_out (struct page *page) {
    struct file_page *file_page = &page->file;
    struct thread *t = page->thread;

    if (pagedir_is_dirty(t->pagedir, page->va)) {
        lock_acquire(&filesys_lock);
        file_write_at(file_page->file, page->frame->kva, 
                      file_page->read_bytes, file_page->ofs);
        lock_release(&filesys_lock);
        pagedir_set_dirty(t->pagedir, page->va, false);
    }
    page->frame = NULL;
    pagedir_clear_page(t->pagedir, page->va);
    return true;
}

static void file_backed_destroy (struct page *page) {
    struct file_page *file_page = &page->file;
    if (page->frame != NULL) {
        struct frame *f = page->frame;
        struct thread *t = page->thread;

        if (pagedir_is_dirty(t->pagedir, page->va)) { 
            lock_acquire(&filesys_lock);
            file_write_at(file_page->file, f->kva, file_page->read_bytes, file_page->ofs);
            lock_release(&filesys_lock);
            pagedir_set_dirty(t->pagedir, page->va, false);
        }
        pagedir_clear_page(t->pagedir, page->va);
        
        lock_acquire(&frame_lock);
        list_remove(&f->elem);
        lock_release(&frame_lock);
        palloc_free_page(f->kva);
        free(f);
        page->frame = NULL;
    }
}

/* Allocation Wrapper */
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
        bool (*init)(struct page *, enum vm_type, void *), void *aux) {
    ASSERT (VM_TYPE(type) != VM_UNINIT)
    struct hash *spt = &thread_current ()->spt;
    struct page *p = (struct page *)malloc (sizeof (struct page));
    if (p == NULL) return false;
    
    p->va = pg_round_down (upage);
    p->writable = writable;
    p->frame = NULL; 
    p->uninit.type = type;
    p->operations = NULL;
    p->uninit.init = init;
    p->uninit.aux = aux;
    p->thread = thread_current();

    if (!spt_insert_page (spt, p)) {
        free(p);
        return false;
    }
    return true;
}

bool vm_alloc_page (enum vm_type type, void *upage, bool writable) {
    return vm_alloc_page_with_initializer (type, upage, writable, anon_initializer, NULL);
}
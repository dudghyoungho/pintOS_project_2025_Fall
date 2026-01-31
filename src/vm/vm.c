/*
 * vm/vm.c
 *
 * Virtual Memory Subsystem Core & Interface
 *
 * This file serves as the central entry point and control logic for the Pintos 
 * Virtual Memory system. It bridges the gap between hardware exceptions (page faults), 
 * user system calls (mmap), and the underlying memory management structures 
 * (SPT, Frame Table, Swap).
 *
 * Key Responsibilities and Functionalities:
 *
 * 1. Page Fault Handling (`vm_try_handle_fault`):
 * - Acts as the primary dispatcher when a Page Fault occurs.
 * - Validates the faulting address (checks for NULL, kernel address, etc.).
 * - Detects valid Stack Growth scenarios using heuristics (e.g., assessing 
 * proximity to the stack pointer `esp` within the 32-byte threshold).
 * - Triggers Demand Paging by locating the faulting page in the Supplemental 
 * Page Table (SPT) and initiating the claim process.
 *
 * 2. Transactional Page Claiming (`vm_do_claim_page`):
 * - Implements a robust, atomic-like mechanism to materialize a virtual page 
 * into a physical frame.
 * - Race Condition Prevention: Implements "Delayed Linking" where the 
 * `frame->page` pointer is set ONLY after the data is successfully loaded 
 * and mapped. This prevents the eviction policy (Clock Algorithm) from 
 * stealing a frame that is currently being initialized by another thread.
 * - Data Integrity: Ensures swap slots are freed only after successful 
 * mapping, preventing data loss if a memory allocation fails mid-process.
 * - Connects the virtual page to the physical frame via the hardware 
 * page table (`pagedir_set_page`).
 *
 * 3. Stack Growth (`vm_stack_growth`):
 * - Automatically expands the user stack up to `STACK_MAX` (1MB).
 * - Allocates new Anonymous Pages (`VM_ANON`) marked with `VM_MARKER_0` 
 * when the stack pointer drops into unmapped valid stack space.
 *
 * 4. Memory Mapped Files (`do_mmap`, `do_munmap`):
 * - `do_mmap`: Handles the mapping of files into virtual memory using 
 * Lazy Loading. It reserves virtual pages (`VM_FILE`) without allocating 
 * physical memory immediately. It manages independent file handles via 
 * `file_reopen` to ensure safety even if the original file descriptor is closed.
 * - `do_munmap`: Handles the unmapping of files. It triggers the write-back 
 * mechanism (via `pagedir_is_dirty` checked in page destruction) to save 
 * modified data back to the disk and cleans up the SPT entries.
 *
 * 5. Initialization (`vm_init`):
 * - Bootstraps the VM subsystem by initializing the Frame Table.
 */


#include "vm/vm.h"
#include "threads/interrupt.h"
#include "vm/page.h"
#include "vm/frame.h" 
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "vm/swap.h" /* [필수] swap_free 사용 */

#define STACK_MAX (1 << 20)

/* VM 시스템 초기화 */
void
vm_init (void) {
    vm_frame_init ();
}

/* 스택 확장 */
static void
vm_stack_growth (void *addr) {
    void *stack_bottom = pg_round_down(addr);
    if (vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true)) {
        if (!vm_claim_page(stack_bottom)) {
        }
    }
}

bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr,
        bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
    
    struct thread *curr = thread_current ();
    struct hash *spt = &curr->spt;

    if (is_kernel_vaddr (addr) || addr == NULL) return false;

    struct page *page = spt_find_page (spt, addr);
    
    // 페이지가 없으면 스택 확장 시도 
    if (page == NULL) {
        void *esp = user ? f->esp : curr->stack_pointer;
        if (addr >= (void *)((uint8_t *)PHYS_BASE - STACK_MAX) &&
            addr >= (void *)((uint8_t *)esp - 32) &&
            addr <= (void *)PHYS_BASE) 
        {
            vm_stack_growth (addr);
            return true;
        }
        return false; 
    }

    if (write && !page->writable) return false;

    return vm_do_claim_page (page);
}

bool
vm_claim_page (void *va) {
    struct thread *curr = thread_current ();
    struct page *page = spt_find_page (&curr->spt, va);
    if (page == NULL) return false;
    return vm_do_claim_page (page);
}

bool
vm_do_claim_page (struct page *page) {
    struct frame *frame = vm_get_frame ();
    if (frame == NULL) return false;

    page->frame = frame;

    struct thread *t = thread_current ();
    bool success = false;

    if (page->operations != NULL && page->operations->swap_in) {
        success = page->operations->swap_in (page, frame->kva);
    } 
    else if (page->operations == NULL) { 
        if (page->uninit.init != NULL) {
            success = page->uninit.init (page, page->uninit.type, page->uninit.aux);
        } else {
            success = true;
        }
    }

    if (!success) {
        lock_acquire(&frame_lock);
        list_remove(&frame->elem);
        lock_release(&frame_lock);
        palloc_free_page(frame->kva);
        free(frame);
        page->frame = NULL;
        return false;
    }

    if (!pagedir_set_page (t->pagedir, page->va, frame->kva, page->writable)) {
        lock_acquire(&frame_lock);
        list_remove(&frame->elem);
        lock_release(&frame_lock);
        palloc_free_page(frame->kva);
        free(frame);
        page->frame = NULL;
        return false;
    }

    if (page->operations != NULL && page->operations->type == VM_ANON) {
        if (page->anon.swap_slot_idx != SIZE_MAX) {
            swap_free(page->anon.swap_slot_idx);
            page->anon.swap_slot_idx = SIZE_MAX;
        }
    }

    frame->page = page; 

    return true;
}

// mmap 
void *
do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {
    if (addr == NULL || addr == 0 || pg_ofs(addr) != 0) return NULL;
    if ((long)length <= 0) return NULL;
    if (offset % PGSIZE != 0) return NULL;
    if (is_kernel_vaddr(addr) || is_kernel_vaddr((uint8_t *)addr + length)) return NULL;

    struct thread *curr = thread_current();
    for (size_t i = 0; i < length; i += PGSIZE) {
        if (spt_find_page(&curr->spt, (uint8_t *)addr + i) != NULL) return NULL;
    }

    struct file *mfile = file_reopen(file);
    if (mfile == NULL) return NULL;

    struct mmap_file *mmap_info = malloc(sizeof(struct mmap_file));
    if (mmap_info == NULL) {
        file_close(mfile);
        return NULL;
    }

    mmap_info->mapid = curr->next_mapid++;
    mmap_info->file = mfile;
    mmap_info->vaddr = addr;
    mmap_info->size = length;
    list_push_back(&curr->mmap_list, &mmap_info->elem);

    size_t read_bytes = (size_t)file_length(mfile) < length ? (size_t)file_length(mfile) : length;
    
    off_t ofs = offset;
    void *upage = addr;
    size_t loop_len = length;

    while (loop_len > 0) {
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        struct mmap_info_aux *aux = malloc(sizeof(struct mmap_info_aux));
        if (aux == NULL) return NULL; 
        
        aux->file = mfile;
        aux->ofs = ofs;
        aux->read_bytes = page_read_bytes;
        aux->zero_bytes = page_zero_bytes;

        if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable, file_backed_initializer, aux)) {
            free(aux);
            return NULL;
        }

        if (read_bytes > 0) read_bytes -= page_read_bytes;
        loop_len -= (loop_len < PGSIZE ? loop_len : PGSIZE);
        upage += PGSIZE;
        ofs += page_read_bytes;
    }

    return addr;
}

// munmap 
void
do_munmap (struct mmap_file *mmap_info) {
    struct thread *curr = thread_current();
    void *upage = mmap_info->vaddr;
    size_t length = mmap_info->size;

    while (length > 0) {
        struct page *p = spt_find_page(&curr->spt, upage);
        if (p != NULL) {
            spt_remove_page(&curr->spt, p); 
        } 
        size_t page_size = (length < PGSIZE ? length : PGSIZE);
        upage += PGSIZE;
        length -= page_size;
    }

    file_close(mmap_info->file);
    list_remove(&mmap_info->elem);
    free(mmap_info);
}
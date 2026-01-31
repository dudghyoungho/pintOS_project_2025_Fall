/* vm/vm.h */
#ifndef VM_VM_H
#define VM_VM_H

#include <stdbool.h>
#include "threads/palloc.h"
#include "filesys/off_t.h"
#include <list.h>
#include "vm/page.h"

struct mmap_file;
struct thread;
struct intr_frame;


struct mmap_info_aux {
    struct file *file;
    off_t ofs;
    size_t read_bytes;
    size_t zero_bytes;
};

struct mmap_file {
    int mapid;
    struct file *file;
    void *vaddr;
    size_t size;
    struct list_elem elem;
};

void vm_init (void);
bool vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr, bool user, bool write, bool not_present);

bool vm_alloc_page (enum vm_type type, void *upage, bool writable);
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
        bool writable, bool (*init)(struct page *, enum vm_type, void *), void *aux);
bool vm_claim_page (void *va);
bool vm_do_claim_page (struct page *page);

/* mmap 관련 함수 */
void *do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset);
void do_munmap (struct mmap_file *mmap_info);
#endif
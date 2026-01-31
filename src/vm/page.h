#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "filesys/off_t.h"
#include "threads/thread.h"


enum vm_type {
    VM_UNINIT = 0,
    VM_ANON = 1,
    VM_FILE = 2,
    VM_PAGE_CACHE = 3,

    VM_MARKER_0 = (1 << 3),
    VM_MARKER_1 = (1 << 4),
};

#define VM_TYPE(type) ((type) & 7)

struct page;
struct frame;



struct file_page {
    struct file *file;
    off_t ofs;
    size_t read_bytes;
    size_t zero_bytes;
};

struct page_operations {
    bool (*swap_in) (struct page *, void *);
    bool (*swap_out) (struct page *);
    void (*destroy) (struct page *);
    enum vm_type type;
};

struct page {
    const struct page_operations *operations;
    void *va;
    struct frame *frame;
    struct thread *thread;
    
    union {
        struct {
            void *aux;
            bool (*init)(struct page *, enum vm_type, void *aux);
            enum vm_type type;
		} uninit; //VM_UNINIT

		struct {
			size_t swap_slot_idx; // Swap Slot Index 
		} anon; // VM_ANON
		
		struct file_page file;
	};

	bool writable; // 쓰기 가능 여부 
	struct hash_elem hash_elem;
};

/* 함수 프로토타입 */
void spt_init (struct hash *spt);
struct page *spt_find_page (struct hash *spt, void *va);
bool spt_insert_page (struct hash *spt, struct page *page);
void spt_remove_page (struct hash *spt, struct page *page);
void spt_kill (struct hash *spt);

void vm_deinit_page (struct page *page);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);
bool file_backed_initializer (struct page *page, enum vm_type type, void *aux);

#endif

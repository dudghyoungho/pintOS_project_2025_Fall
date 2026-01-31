/* vm/frame.h */
#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "vm/vm.h"
#include <list.h>
#include "threads/palloc.h"
#include "threads/synch.h"

struct page;

struct frame {
    void *kva;             /* 커널 가상 주소 (물리 메모리와 1:1 매핑) */
    struct page *page;     /* 역방향 매핑 (이 프레임을 누가 쓰는지) */
    struct list_elem elem; /* 프레임 테이블 리스트용 */
};

struct frame *vm_get_frame (void);
void vm_frame_init(void);
void vm_claim_page_wrapper (void *kpage);   
void vm_free_frame (struct frame *frame);

extern struct lock frame_lock;

#endif
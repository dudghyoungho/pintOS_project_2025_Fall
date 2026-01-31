/* vm/swap.h */

#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdbool.h>
#include <stddef.h>

void swap_init(void);
bool swap_in(size_t used_index, void *kpage);
size_t swap_out(void *kpage);
void swap_free (size_t used_index);

#endif
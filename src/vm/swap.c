/*
 * vm/swap.c
 *
 * Swap Partition Management System
 *
 * This file handles the movement of pages between physical memory (RAM) and 
 * the swap disk (a specific block device). It provides an abstraction layer 
 * that treats the swap disk as a collection of available "slots," where each 
 * slot corresponds to a single memory page (4KB).
 *
 * This module is primarily used by Anonymous Pages (stack, heap) which do not 
 * have a backing file in the file system. When the system runs out of physical 
 * frames, these pages are "swapped out" to this disk area.
 *
 * Key Responsibilities and Functionalities:
 *
 * 1. Swap Space Management (Bitmap):
 * - Manages the allocation status of the swap disk using a bitmap (`swap_map`).
 * - The swap disk is divided into page-sized slots. Since a page is 4KB 
 * and a disk sector is 512 bytes, one swap slot consists of 8 contiguous 
 * sectors (`SECTORS_PER_PAGE`).
 * - `swap_init`: Initializes the block device and the bitmap.
 *
 * 2. Data Transfer (I/O Operations):
 * - `swap_out`: Scans the bitmap for a free slot, writes the content of a 
 * physical frame (`kpage`) to that slot on the disk, flips the bit to 
 * "used", and returns the slot index.
 * - `swap_in`: Reads data from a specific swap slot index back into a 
 * physical frame (`kpage`).
 *
 * 3. Resource Cleanup:
 * - `swap_free`: Clears the bit in the bitmap corresponding to a specific 
 * slot index. This is used when a page is destroyed or successfully 
 * loaded back into memory, making the disk space available for reuse.
 *
 * 4. Synchronization:
 * - Uses a global lock (`swap_lock`) to ensure thread safety. Since multiple 
 * processes may try to swap pages in or out simultaneously, access to 
 * the bitmap and the block device must be serialized to prevent race conditions.
 */

#include "vm/swap.h"
#include "threads/vaddr.h"
#include "devices/block.h"
#include "lib/kernel/bitmap.h"
#include "threads/synch.h"
#include <stdio.h>

static struct block *swap_block;
static struct bitmap *swap_map;
static struct lock swap_lock;

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

void
swap_init (void) {
    lock_init(&swap_lock);
    swap_block = block_get_role (BLOCK_SWAP);
    if (swap_block == NULL) {
        return;
    }

    swap_map = bitmap_create (block_size (swap_block) / SECTORS_PER_PAGE);
    bitmap_set_all (swap_map, false); // 모두 비어있음(0)으로 초기화
    
}

bool
swap_in (size_t used_index, void *kpage) {
    if (swap_block == NULL || swap_map == NULL) return false;

    lock_acquire(&swap_lock);

    if (bitmap_test (swap_map, used_index) == false) {
        lock_release(&swap_lock);
        return false; 
    }

    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        block_read (swap_block, used_index * SECTORS_PER_PAGE + i,
                    (uint8_t *)kpage + i * BLOCK_SECTOR_SIZE);
    }
    
    lock_release(&swap_lock);
    
    return true;
}

size_t
swap_out (void *kpage) {
    if (swap_block == NULL || swap_map == NULL) return SIZE_MAX;

    lock_acquire(&swap_lock);

    size_t free_index = bitmap_scan_and_flip (swap_map, 0, 1, false);
    
    if (free_index == BITMAP_ERROR) {
        lock_release(&swap_lock);
        return SIZE_MAX; 
    }

    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        block_write (swap_block, free_index * SECTORS_PER_PAGE + i,
                     (uint8_t *)kpage + i * BLOCK_SECTOR_SIZE);
    }

    lock_release(&swap_lock);

    return free_index;
}

void
swap_free (size_t used_index) {
    if (swap_block == NULL || swap_map == NULL) return;
    
    lock_acquire(&swap_lock);
    
    if (bitmap_test (swap_map, used_index)) {
        bitmap_set (swap_map, used_index, false);
    }
    
    lock_release(&swap_lock);
}
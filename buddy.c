#include "buddy.h"
#include <stddef.h>
#include <string.h>

#define NULL ((void *)0)
#define PAGE_SIZE 4096
#define MAX_RANK 16
#define MAX_PAGES (128 * 1024 / 4)  // 128MB worth of 4K pages

// Block header structure for free blocks
typedef struct free_block {
    struct free_block *next;
} free_block_t;

// Global variables
static void *mem_base = NULL;
static int total_pages = 0;
static free_block_t *free_lists[MAX_RANK + 1];  // 1-indexed
static int block_rank[MAX_PAGES * 2];  // Track rank of each block

// Helper functions
static int get_block_index(void *ptr) {
    if (ptr < mem_base || ptr >= mem_base + total_pages * PAGE_SIZE) {
        return -1;
    }
    return ((char *)ptr - (char *)mem_base) / PAGE_SIZE;
}

static void *get_block_ptr(int index) {
    if (index < 0 || index >= total_pages) {
        return NULL;
    }
    return (char *)mem_base + index * PAGE_SIZE;
}

static int get_buddy_index(int index, int rank) {
    int block_size = 1 << (rank - 1);
    return index ^ block_size;  // XOR to find buddy
}

static void add_to_free_list(void *ptr, int rank) {
    free_block_t *block = (free_block_t *)ptr;
    // Insert at head for O(1) operation
    block->next = free_lists[rank];
    free_lists[rank] = block;
}

static void remove_from_free_list(void *ptr, int rank) {
    free_block_t **current = &free_lists[rank];
    while (*current) {
        if ((void *)*current == ptr) {
            *current = (*current)->next;
            return;
        }
        current = &(*current)->next;
    }
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0 || pgcount > MAX_PAGES) {
        return -EINVAL;
    }

    mem_base = p;
    total_pages = pgcount;

    // Initialize free lists
    for (int i = 1; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }

    // Initialize block ranks
    memset(block_rank, 0, sizeof(block_rank));

    // Add all memory as individual pages initially - in reverse order
    // so that when we allocate, we get the lowest address first
    for (int i = total_pages - 1; i >= 0; i--) {
        void *page_ptr = get_block_ptr(i);
        add_to_free_list(page_ptr, 1);
        block_rank[i] = 1;
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }

    // Find the smallest available block
    int alloc_rank = rank;
    while (alloc_rank <= MAX_RANK && !free_lists[alloc_rank]) {
        alloc_rank++;
    }

    if (alloc_rank > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }

    // Split larger blocks if necessary
    while (alloc_rank > rank) {
        // Get first block from the larger rank (lowest address)
        free_block_t *block = free_lists[alloc_rank];
        if (!block) {
            return ERR_PTR(-ENOSPC);
        }

        remove_from_free_list(block, alloc_rank);

        // Split it into two buddies
        int block_index = get_block_index(block);
        int buddy_index = get_buddy_index(block_index, alloc_rank - 1);
        void *buddy_ptr = get_block_ptr(buddy_index);

        // Add both halves to the next lower rank - add higher address first
        // so that lower address is allocated first
        if (block_index < buddy_index) {
            add_to_free_list(buddy_ptr, alloc_rank - 1);
            add_to_free_list(block, alloc_rank - 1);
        } else {
            add_to_free_list(block, alloc_rank - 1);
            add_to_free_list(buddy_ptr, alloc_rank - 1);
        }

        // Update block ranks
        int half_size = 1 << (alloc_rank - 2);
        for (int i = 0; i < half_size; i++) {
            block_rank[block_index + i] = alloc_rank - 1;
            block_rank[buddy_index + i] = alloc_rank - 1;
        }

        alloc_rank--;
    }

    // Now we should have a block of the exact size
    // Get first block (lowest address)
    free_block_t *block = free_lists[rank];
    if (!block) {
        return ERR_PTR(-ENOSPC);
    }

    remove_from_free_list(block, rank);

    // Mark as allocated
    int block_index = get_block_index(block);
    int block_size = 1 << (rank - 1);
    for (int i = 0; i < block_size; i++) {
        block_rank[block_index + i] = -rank;  // Negative means allocated
    }

    return block;
}

int return_pages(void *p) {
    if (!p) {
        return -EINVAL;
    }

    int block_index = get_block_index(p);
    if (block_index < 0 || block_index >= total_pages) {
        return -EINVAL;
    }

    // Find the rank of this block
    int rank = block_rank[block_index];
    if (rank >= 0) {
        return -EINVAL;  // Already free
    }
    rank = -rank;

    // Mark as freed
    int block_size = 1 << (rank - 1);
    for (int i = 0; i < block_size && block_index + i < total_pages; i++) {
        block_rank[block_index + i] = rank;
    }

    // Try to merge with buddy
    int current_rank = rank;
    int current_index = block_index;

    while (current_rank < MAX_RANK) {
        int buddy_index = get_buddy_index(current_index, current_rank);
        if (buddy_index < 0 || buddy_index >= total_pages) {
            break;
        }

        // Check if buddy is free and has the same rank
        if (block_rank[buddy_index] != current_rank) {
            break;
        }

        // Remove buddy from free list
        void *buddy_ptr = get_block_ptr(buddy_index);
        remove_from_free_list(buddy_ptr, current_rank);

        // Merge blocks - keep the lower address
        if (current_index > buddy_index) {
            current_index = buddy_index;
        }
        current_rank++;
    }

    // Update block rank for merged block
    int merged_size = 1 << (current_rank - 1);
    for (int i = 0; i < merged_size && current_index + i < total_pages; i++) {
        block_rank[current_index + i] = current_rank;
    }

    // Add merged block to free list - add to head
    void *merged_ptr = get_block_ptr(current_index);
    add_to_free_list(merged_ptr, current_rank);

    return OK;
}

int query_ranks(void *p) {
    if (!p) {
        return -EINVAL;
    }

    int block_index = get_block_index(p);
    if (block_index < 0 || block_index >= total_pages) {
        return -EINVAL;
    }

    int rank = block_rank[block_index];
    if (rank < 0) {
        // Allocated block
        return -rank;
    } else {
        // Free block - return the maximum rank
        return rank;
    }
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }

    int count = 0;
    free_block_t *current = free_lists[rank];
    while (current) {
        count++;
        current = current->next;
    }

    return count;
}
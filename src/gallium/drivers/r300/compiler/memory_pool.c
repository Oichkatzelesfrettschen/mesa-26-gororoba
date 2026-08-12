/*
 * Copyright 2009 Nicolai Hähnle <nhaehnle@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#include "memory_pool.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL_LARGE_ALLOC 4096
#define POOL_ALIGN       8

struct memory_block {
   struct memory_block *next;
};

void
memory_pool_init(struct memory_pool *pool)
{
   memset(pool, 0, sizeof(struct memory_pool));
}

void
memory_pool_destroy(struct memory_pool *pool)
{
   while (pool->blocks) {
      struct memory_block *block = pool->blocks;
      pool->blocks = block->next;
      free(block);
   }
}

static void
refill_pool(struct memory_pool *pool)
{
   unsigned int blocksize = pool->total_allocated;
   struct memory_block *newblock;

   if (!blocksize)
      blocksize = 2 * POOL_LARGE_ALLOC;

   newblock = malloc(blocksize);
   if (!newblock) {
      fprintf(stderr, "r300/compiler: out of memory allocating %u pool bytes\n", blocksize);
      abort();
   }

   newblock->next = pool->blocks;
   pool->blocks = newblock;

   pool->head = (unsigned char *)(newblock + 1);
   pool->end = ((unsigned char *)newblock) + blocksize;
   pool->total_allocated += blocksize;
}

void *
memory_pool_malloc(struct memory_pool *pool, unsigned int bytes)
{
   if (bytes < POOL_LARGE_ALLOC) {
      void *ptr;

      /* C17 6.5.6p9 requires pointer subtraction operands to refer to the
       * same array object.  Test the remaining span after a pool block exists
       * so the subtraction names that array object. */
      if (!pool->head || pool->end - pool->head < (ptrdiff_t)bytes)
         refill_pool(pool);

      assert(pool->head + bytes <= pool->end);

      ptr = pool->head;

      pool->head += bytes;
      pool->head =
         (unsigned char *)(((unsigned long)pool->head + POOL_ALIGN - 1) & ~(POOL_ALIGN - 1));

      return ptr;
   } else {
      struct memory_block *block = malloc(bytes + sizeof(struct memory_block));
      if (!block) {
         fprintf(stderr, "r300/compiler: out of memory allocating %u pool bytes\n", bytes);
         abort();
      }

      block->next = pool->blocks;
      pool->blocks = block;

      return (block + 1);
   }
}

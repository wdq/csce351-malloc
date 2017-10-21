/* 
 * mm.c -  Slightly less simple allocator based on explicit free lists, 
 *                  first fit placement, and boundary tag coalescing. 
 *
 * Each block has header and footer of the form:
 * 
 *      31                     3  2  1  0 
 *      -----------------------------------
 *     | s  s  s  s  ... s  s  s  0  0  a/f
 *      ----------------------------------- 
 * 
 * where s are the meaningful size bits and a/f is set 
 * iff the block is allocated. The list has the following form:
 *
 * begin                                                          end
 * heap                                                           heap  
 *  -----------------------------------------------------------------   
 * |  pad   | hdr(8:a) | ftr(8:a) | zero or more usr blks | hdr(8:a) |
 *  -----------------------------------------------------------------
 *          |       prologue      |                       | epilogue |
 *          |         block       |                       | block    |
 *
 * The allocated prologue and epilogue blocks are overhead that
 * eliminate edge conditions during coalescing.
 *
 * Each free block has a pointer to the previous and next free blocks. 
 * The free_listp pointer points to the first free block in the explicit free list.
 * New free blocks are placed at the start of this list.
 * Free blocks are found by looping through the free list and using the first block that fits.
 * 
 *      31                     3  2  1  0 
 *      -----------------------------------
 *     |    previous free block pointer
 *      ----------------------------------- 
 *      -----------------------------------
 *     |      next free block pointer
 *      ----------------------------------- 
 *      ...................................
 *
 *
 * Currently seems to score a 45 + 18 = 63 on csce.
 * On my desktop computer that this was developed it seems to score a 45 + 40 = 85.
 * It's interesting that it has a much higher throughput on my desktop.
 * Probably a combination of DDR4 vs DDR3, along with having a newer processor.
 *
 * // TODO: With the -g option it shows correct: 11, but the correctness points are 33. Does that my solution is only correct 1/3 of the time?
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "mm.h"
#include "memlib.h"


// Team structure 
team_t team = {
    "wquade-not-a-team-team", 
    "William Quade", "liam@quade.co",
    "", ""
}; 

// $begin mallocmacros
// Basic constants and macros
#define WSIZE       4       // word size (bytes)
#define DSIZE       8       // doubleword size (bytes)
#define CHUNKSIZE  (1<<12)  // initial heap size (bytes)
#define OVERHEAD    8       // overhead of header and footer (bytes)

// Return the maximum of two numbers
#define MAX(x, y) ((x) > (y)? (x) : (y))  

// Pack a size and allocated bit into a word
#define PACK(size, alloc)  ((size) | (alloc))

// Read and write a word at address p 
#define GET(p)       (*(size_t *)(p))
#define PUT(p, val)  (*(size_t *)(p) = (val))  

// Read the size and allocated fields from address p 
#define GET_SIZE(p)  (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

// Given block ptr bp, compute address of its header and footer
#define HDRP(bp)       ((char *)(bp) - WSIZE)  
#define FTRP(bp)       ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

// Given block ptr bp, compute address of next and previous blocks
#define NEXT_BLKP(bp)  ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))
#define PREV_BLKP(bp)  ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))

// Given block ptr bp, compute the address of the next and previous free blocks
#define NEXT_FREE_BLKP(bp)  (*(char **)((bp) + WSIZE))
#define PREV_FREE_BLKP(bp)  (*(char **)(bp))
// $end mallocmacros

// Global variables
// Must be only scalars (like ints, and pointers), no data structures (like structs and arrays).
static char *heap_listp;    // pointer to first block
static char *free_listp;    // pointer to the first free block

// function prototypes for internal helper routines
static void *extend_heap(size_t words);
static void place(void *bp, size_t asize);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void addblock(void *bp);
static void removeblock(void *bp);
static void printblock(void *bp); 
static void checkblock(void *bp);


/* 
 * mm_init - Initialize the memory manager 
 */
// $begin mminit
int mm_init(void) 
{
    // create the initial empty heap
    if ((heap_listp = mem_sbrk(6*WSIZE)) == NULL) {
       return -1;
    }
    PUT(heap_listp, 0);                         // alignment padding
    PUT(heap_listp+WSIZE, PACK(OVERHEAD, 1));   // prologue header
    PUT(heap_listp+DSIZE, PACK(OVERHEAD, 1));   // prologue footer
    PUT(heap_listp+WSIZE+DSIZE, PACK(0, 1));    // epilogue header

    free_listp = heap_listp + DSIZE;            // Setup the explicit free list

    // Extend the empty heap with a free block of WSIZE bytes (less initial utilization)
    if (extend_heap(WSIZE) == NULL) {
       return -1;
    }

    return 0;
}
// $end mminit

/* 
 * mm_malloc - Allocate a block with at least size bytes of payload 
 */
// $begin mmmalloc
void *mm_malloc(size_t size) 
{
    size_t asize;      // adjusted block size
    size_t extendsize; // amount to extend heap if no fit
    char *bp;      

    // Ignore spurious requests
    if (size <= 0) {
       return NULL;
    }

    // Adjust block size to include overhead and alignment reqs.
    if (size <= DSIZE) {
       asize = DSIZE + OVERHEAD;
    } else {
       asize = DSIZE * ((size + (OVERHEAD) + (DSIZE-1)) / DSIZE);
    }    

    // Search the free list for a fit, place into memory if possible.
    if ((bp = find_fit(asize)) != NULL) {
       place(bp, asize);
       return bp;
    }

    // No fit found. Extend the heap and place the block.
    extendsize = MAX(asize,CHUNKSIZE);
    if ((bp = extend_heap(extendsize/WSIZE)) == NULL) {
       return NULL;
    }
    place(bp, asize);

    return bp;
} 
// $end mmmalloc

/* 
 * mm_free - Free a block 
 */
// $begin mmfree
void mm_free(void *bp)
{
    // Find the size of the block being freed.
    size_t size = GET_SIZE(HDRP(bp));

    // Clear the header and footer.
    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));

    // Coalesce so that the freed memory ends up in the freed list in as big of a chunk as possible.
    coalesce(bp);
}
// $end mmfree

/*
 * mm_realloc - naive implementation of mm_realloc
 */
// $begin mm_realloc
// TODO: Look into optimizing this. The provided implementation works, but there could possibly be room for improvement.
void *mm_realloc(void *ptr, size_t size)
{


    
    size_t currentSize = GET_SIZE(HDRP(ptr));
    size_t newSize =  (((size_t)(size) + (OVERHEAD-1)) & ~0x7) + OVERHEAD;
    if(newSize < 3 * OVERHEAD) {
      newSize = 3 * OVERHEAD;
    }
    void *newp;
    size_t copySize;
    
    // Shrink the existing block if possible. 
    if(newSize <= currentSize) {    
      // Don't do anything if there isn't enough space to split the block.
      if(currentSize - newSize <= 3 * OVERHEAD) {
        return ptr;
      }
      
      PUT(HDRP(ptr), PACK(newSize, 1));
      PUT(FTRP(ptr), PACK(newSize, 1));
      PUT(HDRP(NEXT_BLKP(ptr)), PACK(currentSize - newSize, 1));
      mm_free(NEXT_BLKP(ptr));
      return ptr;
    }

    // If the block is already the right size, then just use it.
    if(GET_SIZE(HDRP(ptr)) + OVERHEAD <= GET_SIZE(HDRP(ptr))) {
        return ptr;
    }

    // If the next block is available, and the combined size is big enough, combine the blocks and use it.
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(ptr)));
    size_t combined_size = GET_SIZE(HDRP(NEXT_BLKP(ptr))) + currentSize;
    if(!next_alloc && combined_size >= newSize) {
        removeblock(NEXT_BLKP(ptr));
        PUT(HDRP(ptr), PACK(combined_size, 1));
        PUT(FTRP(ptr), PACK(combined_size, 1));
        return ptr;
    }
    
        if ((newp = mm_malloc(size)) == NULL) {
        	printf("ERROR: mm_malloc failed in mm_realloc\n");
        	exit(1);
        }
        copySize = GET_SIZE(HDRP(ptr));
        if (size < copySize) {
          copySize = size;
        }
        memcpy(newp, ptr, copySize);
        mm_free(ptr);
        return newp;  

}
// $end mm_realloc

/* 
 * mm_checkheap - Check the heap for consistency. Hasn't been modified from what was provided. Might not work.
 */
// $begin mm_checkheap
// TODO: Should probably change this to do more than the provided function. It's worth 5 points.
void mm_checkheap(int verbose) 
{
    char *bp = heap_listp;

    if (verbose) {
       printf("Heap (%p):\n", heap_listp);
    }

    if ((GET_SIZE(HDRP(heap_listp)) != DSIZE) || !GET_ALLOC(HDRP(heap_listp))) {
       printf("Bad prologue header\n");
    }
    checkblock(heap_listp);

    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
       if (verbose) {
           printblock(bp);
        }
       checkblock(bp);
    }
     
    if (verbose) {
       printblock(bp);
    }
    if ((GET_SIZE(HDRP(bp)) != 0) || !(GET_ALLOC(HDRP(bp)))) {
       printf("Bad epilogue header\n");
    }
}
// $end mm_checkheap


/*********************************************************************************/
/*********************************************************************************/
//        The remaining routines are internal helper routines                     /
/*********************************************************************************/
/*********************************************************************************/


/* 
 * extend_heap - Extend heap with free block and return its block pointer
 */
// $begin mmextendheap
static void *extend_heap(size_t words) 
{
    char *bp;
    size_t size;
    
    // Allocate an even number of words to maintain alignment, a minimum of 16 bytes.
    if(((words % 2) ? (words+1) * WSIZE : words * WSIZE) > OVERHEAD + OVERHEAD) {
        size = ((words % 2) ? (words+1) * WSIZE : words * WSIZE);
    } else {
        size = OVERHEAD + OVERHEAD;
    }

    // Quit if we can't get enough memory.
    if ((bp = mem_sbrk(size)) == (void *)-1) { 
       return NULL;
    }

    // Initialize free block header/footer and the epilogue header
    PUT(HDRP(bp), PACK(size, 0));         // free block header
    PUT(FTRP(bp), PACK(size, 0));         // free block footer
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); // new epilogue header

    // Coalesce to combine with previous free block (if it exists), and add to explicit free list.
    return coalesce(bp);
}
// $end mmextendheap

/* 
 * place - Place block of asize bytes at start of free block bp 
 *         and split if remainder would be at least minimum block size
 */
// $begin place
static void place(void *bp, size_t asize)
{
    // Get the block size.
    size_t csize = GET_SIZE(HDRP(bp));   

    // Split the block if it's large enough to be split
    if ((csize - asize) >= (DSIZE + OVERHEAD)) { 
       PUT(HDRP(bp), PACK(asize, 1));
       PUT(FTRP(bp), PACK(asize, 1));
       // Remove the placed block from the free list.
       removeblock(bp);
       bp = NEXT_BLKP(bp);
       PUT(HDRP(bp), PACK(csize-asize, 0));
       PUT(FTRP(bp), PACK(csize-asize, 0));
       // Coalesce so it can merge with nearby free blocks, and also be added to the free list.
       coalesce(bp);
    } else { 
       // Just use the whole block if it isn't big enough to be split.
       PUT(HDRP(bp), PACK(csize, 1));
       PUT(FTRP(bp), PACK(csize, 1));
       // Remove the placed block from the free list
       removeblock(bp);
    }
}
// $end place

/* 
 * find_fit - Find a fit for a block with asize bytes 
 */
// $begin find_fit
static void *find_fit(size_t asize)
{
    void *bp;

    // Find the first fit by looping through the explicit free list.
    for (bp = free_listp; GET_ALLOC(HDRP(bp)) == 0; bp = NEXT_FREE_BLKP(bp)) {
       if (!GET_ALLOC(HDRP(bp)) && (asize <= GET_SIZE(HDRP(bp)))) {
           return bp;
       }
    }

    // If there isn't a fit, then extend the heap and return the extended block. That way there will always be a fit.
    bp = extend_heap(asize/WSIZE);

    return bp;

}
// $end find_fit

/*
 * coalesce - boundary tag coalescing. Return ptr to coalesced block
 */
// $begin coalesce
static void *coalesce(void *bp) 
{
    // Get the size of the current block, and check if the block before and after the current block are allocated.
    size_t prev_alloc = (GET_ALLOC(FTRP(PREV_BLKP(bp)))) || PREV_BLKP(bp) == bp;
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    // Case 1 (don't merge anything)
    if(prev_alloc && next_alloc) {
      addblock(bp);
      return bp;
    
    // Case 2 (merge next block)
    } else if (prev_alloc && !next_alloc) {
       size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
       removeblock(NEXT_BLKP(bp));
       PUT(HDRP(bp), PACK(size, 0));
       PUT(FTRP(bp), PACK(size, 0));

    // Case 3 (merge previous block)
    } else if (!prev_alloc && next_alloc) {
       size += GET_SIZE(HDRP(PREV_BLKP(bp)));
       removeblock(PREV_BLKP(bp));
       PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
       PUT(FTRP(PREV_BLKP(bp)), PACK(size, 0));
       bp = PREV_BLKP(bp);

    // Case 4 (merge previous and next blocks)
    } else if(!prev_alloc && !next_alloc) {
       size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(HDRP(NEXT_BLKP(bp)));
       removeblock(PREV_BLKP(bp));
       removeblock(NEXT_BLKP(bp));
       PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
       PUT(FTRP(PREV_BLKP(bp)), PACK(size, 0));
       bp = PREV_BLKP(bp);

    }

    // Add the merged block to the explicit free list.
    addblock(bp);

    return bp;
}
// $end coalesce

/*
 * addblock - Add a block to the start of the free_listp explicit free list.
 *            Adjusts the neighbor pointers so everything still is linked correctly.
 */
// $begin addblock
static void addblock(void *bp) {
    NEXT_FREE_BLKP(bp) = free_listp;    // Point the new block's next free block to the start of the list.
    PREV_FREE_BLKP(free_listp) = bp;    // Point the start of the list's previous free block to the new block.
    PREV_FREE_BLKP(bp) = NULL;          // Point the new block's previous free block to nothing.
    free_listp = bp;                    // Set the new block as the start of the list.
}
// $end addblock

/*
 * removeblock - Remove a block from the free_listp explicit free list.
 *               Moves around some neighbor prev/next pointers so everything is still linked correctly.
 */
// $begin removeblock
static void removeblock(void *bp) {
        if(PREV_FREE_BLKP(bp)) {
            // If the block being removed has a previous free block:
            // Then set the previous free block's next free block to the block being removed's next free block.
            // Ex: removeblock(B)
            // Before: [A] -> [B] -> [C]
            // After:  [A] -> [C]
            //         [B] -> [C]
            NEXT_FREE_BLKP(PREV_FREE_BLKP(bp)) = NEXT_FREE_BLKP(bp);
        } else {
            // If the block being removed doesn't have a previous free block, then it's the first block in free_listp.
            // Set the free_listp to point to the next block after the one being removed.
            // Ex: removeblock(A)
            // Before: free_listp -> [A] -> [B]
            // After:  free_listp -> [B]
            //                [A] -> [B]
            free_listp = NEXT_FREE_BLKP(bp);
        }
        // Then point the next free block's previous free block pointer to the previous free block of the one being removed.
        // Ex: removeblock(B)
        // Before: [A] <- [B] <- [C]
        // After:  [A] <- [C]
        //         [A] <- [B]
        PREV_FREE_BLKP(NEXT_FREE_BLKP(bp)) = PREV_FREE_BLKP(bp);
}
// $end removeblock

/*
 * printblock - Print the block's contents. This hasn't been modified from what was provided. Probably won't work.
 */
// $begin printblock
// TODO: Get this working with the new code.
static void printblock(void *bp) 
{
    size_t hsize, halloc, fsize, falloc;

    hsize = GET_SIZE(HDRP(bp));
    halloc = GET_ALLOC(HDRP(bp));  
    fsize = GET_SIZE(FTRP(bp));
    falloc = GET_ALLOC(FTRP(bp));  
    
    if (hsize == 0) {
       printf("%p: EOL\n", bp);
       return;
    }

    printf("%p: header: [%d:%c] footer: [%d:%c]\n", bp, hsize, (halloc ? 'a' : 'f'), fsize, (falloc ? 'a' : 'f')); 
}
// $end printblock

/*
 * checkblock - Check the block. This hasn't been modified from what was provided. Probably won't work.
 */
// $begin checkblock
// TODO: Get this working with the new code.
static void checkblock(void *bp) 
{
    if ((size_t)bp % 8) {
       printf("Error: %p is not doubleword aligned\n", bp);
    }
    if (GET(HDRP(bp)) != GET(FTRP(bp))) {
       printf("Error: header does not match footer\n");
    }
}
// $end checkblock

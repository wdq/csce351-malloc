/* 
 * mm-implicit.c -  Simple allocator based on implicit free lists, 
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
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include "mm.h"
#include "memlib.h"


/* Team structure */
team_t team = {
    "wquade-not-a-team-team", 
    "William Quade", "liam@quade.co",
    "", ""
}; 

/* $begin mallocmacros */
/* Basic constants and macros */
#define WSIZE       4       /* word size (bytes) */  
#define DSIZE       8       /* doubleword size (bytes) */
#define CHUNKSIZE  (1<<12)  /* initial heap size (bytes) */
#define OVERHEAD    8       /* overhead of header and footer (bytes) */
#define FREE_OVERHEAD   24 // 8 (size/a) + 8 (next) + 8 (prev)

// Example address: 0xf61ae840, 8 bytes size
#define ADDRESS_SIZE    8   // bytes

#define MAX(x, y) ((x) > (y)? (x) : (y))  

/* Pack a size and allocated bit into a word */
#define PACK(size, alloc)  ((size) | (alloc))

/* Read and write a word at address p */
#define GET(p)       (*(size_t *)(p))
#define PUT(p, val)  (*(size_t *)(p) = (val))  

/* Read the size and allocated fields from address p */
#define GET_SIZE(p)  (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

/* Given block ptr bp, compute address of its header and footer */
#define HDRP(bp)       ((void *)(bp) - WSIZE)  
#define FTRP(bp)       ((void *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/* Given block ptr bp, compute address of next and previous blocks */
#define NEXT_BLKP(bp)  ((void *)(bp) + GET_SIZE(((void *)(bp) - WSIZE)))
#define PREV_BLKP(bp)  ((void *)(bp) - GET_SIZE(((void *)(bp) - DSIZE)))

/* Given block ptr bp, compute the address of the next and previous free blocks */
#define NEXT_FREE_BLKP(bp)  (*(void **)((bp) + WSIZE))
#define PREV_FREE_BLKP(bp)  (*(void **)(bp))


/* $end mallocmacros */

/* Global variables */
static char *heap_listp = 0;  /* pointer to first block */  
static char *free_listp = 0; /* pointer to the first free block */

/* function prototypes for internal helper routines */
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
/* $begin mminit */
int mm_init(void) 
{
    //printf("\nmm_init() starting!\n");
    /* create the initial empty heap */
    if ((heap_listp = mem_sbrk(4*DSIZE)) == NULL) {
       return -1;
    }
    PUT(heap_listp, 0);                        /* alignment padding */
    PUT(heap_listp+WSIZE, PACK(DSIZE, 1));  /* prologue header */ 
    PUT(heap_listp+DSIZE, PACK(DSIZE, 1)); /* prologue footer */
    PUT(heap_listp+DSIZE+WSIZE, PACK(0, 1)); /* epilogue header */

    free_listp = heap_listp + DSIZE; // Setup the explicit free list

    /* Extend the empty heap with a free block of CHUNKSIZE bytes */
    if (extend_heap(WSIZE) == NULL) {
       return -1;
    }

    //printf("\nmm_init() done!\n");
    return 0;
}
/* $end mminit */

/* 
 * mm_malloc - Allocate a block with at least size bytes of payload 
 */
/* $begin mmmalloc */
void *mm_malloc(size_t size) 
{
    printf("\nmm_malloc start!\n");
    size_t asize;      /* adjusted block size */
    size_t extendsize; /* amount to extend heap if no fit */
    char *bp;      

    /* Ignore spurious requests */
    if (size <= 0) {
       return NULL;
    }

    /* Adjust block size to include overhead and alignment reqs. */
    //asize = MAX(((size + (DSIZE-1)) & ~0x7) + DSIZE, FREE_OVERHEAD);
    if (size <= DSIZE) {
       asize = DSIZE + DSIZE;
    } else {
       asize = DSIZE * ((size + (DSIZE) + (DSIZE-1)) / DSIZE);
    }    
    //printf("size=%i\n", size);
    //printf("asize=%i\n", asize);

    /* Search the free list for a fit */
    if ((bp = find_fit(asize)) != NULL) {
       place(bp, asize);
       printf("\nmm_malloc() stop B!\n");
       return bp;
    }

    /* No fit found. Get more memory and place the block */
    extendsize = MAX(asize,CHUNKSIZE);
    if ((bp = extend_heap(extendsize/WSIZE)) == NULL) {
        printf("\nmm_malloc() stop C!\n");
       return NULL;
    }
    place(bp, asize);
    printf("\nmm_malloc() stop D!\n");
    return bp;
} 
/* $end mmmalloc */

/* 
 * mm_free - Free a block 
 */
/* $begin mmfree */
void mm_free(void *bp)
{
    printf("\nmm_free start!\n");
    //printf("Address: %x\n", bp);
    size_t size = GET_SIZE(HDRP(bp));

    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    coalesce(bp);
}

/* $end mmfree */

/*
 * mm_realloc - naive implementation of mm_realloc
 */
void *mm_realloc(void *ptr, size_t size)
{
    void *newp;
    size_t copySize;

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

/* 
 * mm_checkheap - Check the heap for consistency 
 */
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

/* The remaining routines are internal helper routines */

/* 
 * extend_heap - Extend heap with free block and return its block pointer
 */
/* $begin mmextendheap */
static void *extend_heap(size_t words) 
{
    //printf("\nextend_heap() start!\n");

    char *bp;
    size_t size;
    
    /* Allocate an even number of words to maintain alignment */
    // Must be at least the size of the FREE_OVERHEAD block size.
    size = MAX(((words % 2) ? (words+1) * WSIZE : words * WSIZE), DSIZE * 2);

    if ((bp = mem_sbrk(size)) == (void *)-1) { 
       return NULL;
    }

    /* Initialize free block header/footer and the epilogue header */
    PUT(HDRP(bp), PACK(size, 0));         /* free block header */
    PUT(FTRP(bp), PACK(size, 0));         /* free block footer */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* new epilogue header */

    //printf("\nextend_heap() done!\n");

    /* Coalesce if the previous block was free */
    return coalesce(bp);
}
/* $end mmextendheap */

/* 
 * place - Place block of asize bytes at start of free block bp 
 *         and split if remainder would be at least minimum block size
 */
/* $begin mmplace */
/* $begin mmplace-proto */
static void place(void *bp, size_t asize)
/* $end mmplace-proto */
{
    printf("\nplace() start!\n");

    size_t csize = GET_SIZE(HDRP(bp));   

    if ((csize - asize) >= (DSIZE + DSIZE)) { 
       PUT(HDRP(bp), PACK(asize, 1));
       PUT(FTRP(bp), PACK(asize, 1));
       removeblock(bp);
       bp = NEXT_BLKP(bp);
       PUT(HDRP(bp), PACK(csize-asize, 0));
       PUT(FTRP(bp), PACK(csize-asize, 0));
       coalesce(bp);
    } else { 
       PUT(HDRP(bp), PACK(csize, 1));
       PUT(FTRP(bp), PACK(csize, 1));
       removeblock(bp);
    }
}
/* $end mmplace */

/* 
 * find_fit - Find a fit for a block with asize bytes 
 */
static void *find_fit(size_t asize)
{
    printf("\nfind_fit() start!\n");
    // first fit search 
    void *bp;

    for (bp = free_listp; GET_ALLOC(HDRP(bp)) == 0; bp = NEXT_FREE_BLKP(bp)) {
       if ((asize <= GET_SIZE(HDRP(bp)))) {
            printf("\nfind_fit() stop A!\n");
           return bp;
       }
    }

    int extendsize = MAX(asize, DSIZE + DSIZE);
    bp = extend_heap(extendsize/4);
    printf("\nfind_fit() stop B!\n");
    return bp;

    //return NULL; /* no fit */
}

/*
 * coalesce - boundary tag coalescing. Return ptr to coalesced block
 */
static void *coalesce(void *bp) 
{
    printf("\ncoalesce() start!\n");

    size_t prev_alloc = (GET_ALLOC(FTRP(PREV_BLKP(bp)))) || PREV_BLKP(bp) == bp;
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    if (prev_alloc && !next_alloc) {      // Case 1 (merge next block)
printf("A\n");
       size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
       removeblock(NEXT_BLKP(bp));
       PUT(HDRP(bp), PACK(size, 0));
       PUT(FTRP(bp), PACK(size, 0));

    } else if (!prev_alloc && next_alloc) {      // Case 2 (merge previous block)
printf("B\n");
       size += GET_SIZE(HDRP(PREV_BLKP(bp)));
       bp = PREV_BLKP(bp);
       removeblock(bp);
       PUT(HDRP(bp), PACK(size, 0));
       PUT(FTRP(bp), PACK(size, 0));

    } else if(!prev_alloc && !next_alloc) {      // Case 3 (merge previous and next blocks)
printf("C\n");
       size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(HDRP(NEXT_BLKP(bp)));
       removeblock(PREV_BLKP(bp));
       removeblock(NEXT_BLKP(bp));
       bp = PREV_BLKP(bp);
       PUT(HDRP(bp), PACK(size, 0));
       PUT(FTRP(bp), PACK(size, 0));

    }

    addblock(bp);

    printf("\ncoalesce() done!\n");
    return bp;
}

// Add a block to the start of the free_listp explicit free list.
static void addblock(void *bp) {
    printf("Adblock\n");
    NEXT_FREE_BLKP(bp) = free_listp; // Point the new block's next free block to the start of the list.
    PREV_FREE_BLKP(free_listp) = bp; // Point the start of the list's previous free block to the new block.
    PREV_FREE_BLKP(bp) = NULL; // Point the new block's previous free block to nothing.
    free_listp = bp; // Set the new block as the start of the list.
}

// Remove a block from the free_listp explicit free list.
// Moves around some neighbor prev/next pointers so everything is still linked correctly (I hope).
static void removeblock(void *bp) {
    printf("Removeblock\n");
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

static void checkblock(void *bp) 
{
    if ((size_t)bp % 8) {
       printf("Error: %p is not doubleword aligned\n", bp);
    }
    if (GET(HDRP(bp)) != GET(FTRP(bp))) {
       printf("Error: header does not match footer\n");
    }
}

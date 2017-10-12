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
    "not-a-team-team", 
    "William Quade", "liam@quade.co",
    "", ""
}; 

/* $begin mallocmacros */
/* Basic constants and macros */
#define WORD_SIZE       4       /* word size (bytes) */  
#define DOUBLE_SIZE       8       /* doubleword size (bytes) */
#define CHUNK_SIZE  (1<<12)  /* initial heap size (bytes) */
#define OVERHEAD    8       /* overhead of header and footer (bytes) */

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
#define HEADER_POINTER(bp)          ((char *)(bp) - WORD_SIZE)
#define NEXT_FREE_POINTER(bp)       ((char *)(bp)) 
#define PREVIOUS_FREE_POINTER(bp)   ((char *)(bp) + WORD_SIZE)
#define FOOTER_POINTER(bp)          ((char *)(bp) + GET_SIZE(HEADER_POINTER(bp)) - DOUBLE_SIZE)

/* Given block ptr bp, compute address of next and previous blocks */
#define NEXT_BLOCK_POINTER(bp)  ((char *)(bp) + GET_SIZE(((char *)(bp) - WORD_SIZE)))
#define PREVIOUS_BLOCK_POINTER(bp)  ((char *)(bp) - GET_SIZE(((char *)(bp) - DOUBLE_SIZE)))
/* $end mallocmacros */

/* Global variables */
static char *heap_listp;  /* pointer to first block */  

/* function prototypes for internal helper routines */
static void *extend_heap(size_t words);
static void place(void *bp, size_t asize);
static void *find_fit(size_t asize);
static void *coalesce(void *bp);
static void printblock(void *bp); 
static void checkblock(void *bp);

/* 
 * mm_init - Initialize the memory manager 
 */
/* $begin mminit */
int mm_init(void) 
{
    // Explicit free list starts out the same as implicit free list?
    // Since they start out as all free.

    /* create the initial empty heap */
    if ((heap_listp = mem_sbrk(4*WORD_SIZE)) == NULL)
	return -1;
    PUT(heap_listp, 0);                        /* alignment padding */
    PUT(heap_listp+WORD_SIZE, PACK(OVERHEAD, 1));  /* prologue header */ 
    PUT(heap_listp+DOUBLE_SIZE, PACK(OVERHEAD, 1));  /* prologue footer */ 
    PUT(heap_listp+WORD_SIZE+DOUBLE_SIZE, PACK(0, 1));   /* epilogue header */
    heap_listp += DOUBLE_SIZE;


    /* Extend the empty heap with a free block of CHUNK_SIZE bytes */
    if (extend_heap(CHUNK_SIZE/WORD_SIZE) == NULL)
	return -1;
    return 0;
}
/* $end mminit */

/* 
 * mm_malloc - Allocate a block with at least size bytes of payload 
 */
/* $begin mmmalloc */
void *mm_malloc(size_t size) 
{
    size_t asize;      /* adjusted block size */
    size_t extenDOUBLE_SIZE; /* amount to extend heap if no fit */
    char *bp;      

    /* Ignore spurious requests */
    if (size <= 0) {
        return NULL;
    }

    /* Adjust block size to include overhead and alignment reqs. */
    if (size <= DOUBLE_SIZE) {
	   asize = DOUBLE_SIZE + OVERHEAD;
    } else {
	   asize = DOUBLE_SIZE * ((size + (OVERHEAD) + (DOUBLE_SIZE-1)) / DOUBLE_SIZE);
    }

    /* Search the free list for a fit */
    if ((bp = find_fit(asize)) != NULL) {
	   place(bp, asize);
	   return bp;
    }

    /* No fit found. Get more memory and place the block */
    extenDOUBLE_SIZE = MAX(asize,CHUNK_SIZE);
    
    if ((bp = extend_heap(extenDOUBLE_SIZE/WORD_SIZE)) == NULL) {
	   return NULL;
    }

    place(bp, asize);
    return bp;
} 
/* $end mmmalloc */

/* 
 * mm_free - Free a block 
 */
/* $begin mmfree */
void mm_free(void *bp)
{
    size_t size = GET_SIZE(HEADER_POINTER(bp));

    PUT(HEADER_POINTER(bp), PACK(size, 0));
    PUT(FOOTER_POINTER(bp), PACK(size, 0));
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
    copySize = GET_SIZE(HEADER_POINTER(ptr));
    if (size < copySize)
      copySize = size;
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

    if (verbose)
	printf("Heap (%p):\n", heap_listp);

    if ((GET_SIZE(HEADER_POINTER(heap_listp)) != DOUBLE_SIZE) || !GET_ALLOC(HEADER_POINTER(heap_listp)))
	printf("Bad prologue header\n");
    checkblock(heap_listp);

    for (bp = heap_listp; GET_SIZE(HEADER_POINTER(bp)) > 0; bp = NEXT_BLOCK_POINTER(bp)) {
	if (verbose) 
	    printblock(bp);
	checkblock(bp);
    }
     
    if (verbose)
	printblock(bp);
    if ((GET_SIZE(HEADER_POINTER(bp)) != 0) || !(GET_ALLOC(HEADER_POINTER(bp))))
	printf("Bad epilogue header\n");
}

/* The remaining routines are internal helper routines */

/* 
 * extend_heap - Extend heap with free block and return its block pointer
 */
/* $begin mmextendheap */
static void *extend_heap(size_t words) 
{
    char *bp;
    size_t size;
	
    /* Allocate an even number of words to maintain alignment */
    size = (words % 2) ? (words+1) * WORD_SIZE : words * WORD_SIZE;
    if ((bp = mem_sbrk(size)) == (void *)-1) 
	return NULL;

    /* Initialize free block header/footer and the epilogue header */
    PUT(HEADER_POINTER(bp), PACK(size, 0));         /* free block header */
    PUT(FOOTER_POINTER(bp), PACK(size, 0));         /* free block footer */
    PUT(HEADER_POINTER(NEXT_BLOCK_POINTER(bp)), PACK(0, 1)); /* new epilogue header */

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
    size_t csize = GET_SIZE(HEADER_POINTER(bp));   

    if ((csize - asize) >= (DOUBLE_SIZE + OVERHEAD)) { 
	PUT(HEADER_POINTER(bp), PACK(asize, 1));
	PUT(FOOTER_POINTER(bp), PACK(asize, 1));
	bp = NEXT_BLOCK_POINTER(bp);
	PUT(HEADER_POINTER(bp), PACK(csize-asize, 0));
	PUT(FOOTER_POINTER(bp), PACK(csize-asize, 0));
    }
    else { 
	PUT(HEADER_POINTER(bp), PACK(csize, 1));
	PUT(FOOTER_POINTER(bp), PACK(csize, 1));
    }
}
/* $end mmplace */

/* 
 * find_fit - Find a fit for a block with asize bytes 
 */
static void *find_fit(size_t asize)
{
    /* first fit search */
    void *bp;

    for (bp = heap_listp; GET_SIZE(HEADER_POINTER(bp)) > 0; bp = NEXT_BLOCK_POINTER(bp)) {
    	if (!GET_ALLOC(HEADER_POINTER(bp)) && (asize <= GET_SIZE(HEADER_POINTER(bp)))) {
    	    return bp;
    	}
    }
    return NULL; /* no fit */
}

/*
 * coalesce - boundary tag coalescing. Return ptr to coalesced block
 */
static void *coalesce(void *bp) 
{
    size_t prev_alloc = GET_ALLOC(FOOTER_POINTER(PREVIOUS_BLOCK_POINTER(bp)));
    size_t next_alloc = GET_ALLOC(HEADER_POINTER(NEXT_BLOCK_POINTER(bp)));
    size_t size = GET_SIZE(HEADER_POINTER(bp));

    if (prev_alloc && next_alloc) {            /* Case 1 */
	return bp;
    }

    else if (prev_alloc && !next_alloc) {      /* Case 2 */
	size += GET_SIZE(HEADER_POINTER(NEXT_BLOCK_POINTER(bp)));
	PUT(HEADER_POINTER(bp), PACK(size, 0));
	PUT(FOOTER_POINTER(bp), PACK(size,0));
    }

    else if (!prev_alloc && next_alloc) {      /* Case 3 */
	size += GET_SIZE(HEADER_POINTER(PREVIOUS_BLOCK_POINTER(bp)));
	PUT(FOOTER_POINTER(bp), PACK(size, 0));
	PUT(HEADER_POINTER(PREVIOUS_BLOCK_POINTER(bp)), PACK(size, 0));
	bp = PREVIOUS_BLOCK_POINTER(bp);
    }

    else {                                     /* Case 4 */
	size += GET_SIZE(HEADER_POINTER(PREVIOUS_BLOCK_POINTER(bp))) + 
	    GET_SIZE(FOOTER_POINTER(NEXT_BLOCK_POINTER(bp)));
	PUT(HEADER_POINTER(PREVIOUS_BLOCK_POINTER(bp)), PACK(size, 0));
	PUT(FOOTER_POINTER(NEXT_BLOCK_POINTER(bp)), PACK(size, 0));
	bp = PREVIOUS_BLOCK_POINTER(bp);
    }

    return bp;
}


static void printblock(void *bp) 
{
    size_t hsize, halloc, fsize, falloc;

    hsize = GET_SIZE(HEADER_POINTER(bp));
    halloc = GET_ALLOC(HEADER_POINTER(bp));  
    fsize = GET_SIZE(FOOTER_POINTER(bp));
    falloc = GET_ALLOC(FOOTER_POINTER(bp));  
    
    if (hsize == 0) {
	printf("%p: EOL\n", bp);
	return;
    }

    printf("%p: header: [%d:%c] footer: [%d:%c]\n", bp, 
	   hsize, (halloc ? 'a' : 'f'), 
	   fsize, (falloc ? 'a' : 'f')); 
}

static void checkblock(void *bp) 
{
    if ((size_t)bp % 8)
	printf("Error: %p is not doubleword aligned\n", bp);
    if (GET(HEADER_POINTER(bp)) != GET(FOOTER_POINTER(bp)))
	printf("Error: header does not match footer\n");
}


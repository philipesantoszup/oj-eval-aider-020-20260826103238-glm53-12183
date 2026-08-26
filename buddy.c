#include "buddy.h"
#include <stdlib.h>

#define MAXRANK 16
#define PAGESZ 4096

/* Global state of the buddy system */
static int inited = 0;
static char *pool_base = NULL;
static int npages = 0;

/* blk_rank[i]: rank of the (free or allocated) block containing page i */
static unsigned char *blk_rank = NULL;
/* alloc_flag[i]: rank if page i is the start of an allocated block, else 0 */
static unsigned char *alloc_flag = NULL;

/* Per-rank free lists (doubly linked, sorted by page index) */
static int *fnext = NULL;
static int *fprev = NULL;
static int fhead[MAXRANK + 1];
static int ftail[MAXRANK + 1];
static int fcount[MAXRANK + 1];

static void set_rank_range(int s, int len, int r) {
    int i;
    for (i = s; i < s + len; ++i)
        blk_rank[i] = (unsigned char)r;
}

static void list_insert(int r, int s) {
    if (fhead[r] < 0) {
        fhead[r] = ftail[r] = s;
        fnext[s] = -1;
        fprev[s] = -1;
    } else if (s < fhead[r]) {
        fnext[s] = fhead[r];
        fprev[s] = -1;
        fprev[fhead[r]] = s;
        fhead[r] = s;
    } else if (s > ftail[r]) {
        fnext[ftail[r]] = s;
        fprev[s] = ftail[r];
        fnext[s] = -1;
        ftail[r] = s;
    } else {
        int cur = fhead[r];
        while (fnext[cur] >= 0 && fnext[cur] < s)
            cur = fnext[cur];
        fnext[s] = fnext[cur];
        fprev[s] = cur;
        if (fnext[cur] >= 0)
            fprev[fnext[cur]] = s;
        fnext[cur] = s;
    }
    fcount[r]++;
}

static void list_remove(int r, int s) {
    if (fprev[s] >= 0)
        fnext[fprev[s]] = fnext[s];
    else
        fhead[r] = fnext[s];
    if (fnext[s] >= 0)
        fprev[fnext[s]] = fprev[s];
    else
        ftail[r] = fprev[s];
    fcount[r]--;
}

int init_page(void *p, int pgcount) {
    int r, off, rem, len;

    if (p == NULL || pgcount <= 0)
        return -EINVAL;

    if (inited) {
        free(blk_rank);
        free(alloc_flag);
        free(fnext);
        free(fprev);
        blk_rank = NULL;
        alloc_flag = NULL;
        fnext = NULL;
        fprev = NULL;
        inited = 0;
    }

    pool_base = (char *)p;
    npages = pgcount;

    blk_rank = (unsigned char *)malloc((size_t)pgcount);
    alloc_flag = (unsigned char *)calloc((size_t)pgcount, 1);
    fnext = (int *)malloc(sizeof(int) * (size_t)pgcount);
    fprev = (int *)malloc(sizeof(int) * (size_t)pgcount);
    if (blk_rank == NULL || alloc_flag == NULL ||
        fnext == NULL || fprev == NULL)
        return -ENOSPC;

    for (r = 1; r <= MAXRANK; ++r) {
        fhead[r] = -1;
        ftail[r] = -1;
        fcount[r] = 0;
    }

    /* Build initial free blocks: greedily take the largest fitting block. */
    off = 0;
    rem = pgcount;
    while (rem > 0) {
        r = MAXRANK;
        while ((1 << (r - 1)) > rem)
            r--;
        len = 1 << (r - 1);
        set_rank_range(off, len, r);
        list_insert(r, off);
        off += len;
        rem -= len;
    }

    inited = 1;
    return OK;
}

void *alloc_pages(int rank) {
    int r, s, half;

    if (!inited || rank < 1 || rank > MAXRANK)
        return ERR_PTR(-EINVAL);

    /* Find the smallest rank >= rank that has a free block. */
    r = rank;
    while (r <= MAXRANK && fcount[r] == 0)
        r++;
    if (r > MAXRANK)
        return ERR_PTR(-ENOSPC);

    /* Take the lowest-address free block of that rank. */
    s = fhead[r];
    list_remove(r, s);

    /* Split down to the requested rank. */
    while (r > rank) {
        r--;
        half = 1 << (r - 1);
        list_insert(r, s + half);
        set_rank_range(s, half << 1, r);
    }

    alloc_flag[s] = (unsigned char)rank;
    return (void *)(pool_base + (long)s * PAGESZ);
}

int return_pages(void *p) {
    long off;
    int i, s, r, half, buddy;

    if (!inited || p == NULL)
        return -EINVAL;

    off = (char *)p - pool_base;
    if (off < 0 || off >= (long)npages * PAGESZ)
        return -EINVAL;
    if (off % PAGESZ != 0)
        return -EINVAL;

    i = (int)(off / PAGESZ);
    if (alloc_flag[i] == 0)
        return -EINVAL;

    r = alloc_flag[i];
    alloc_flag[i] = 0;
    s = i;

    /* Merge with free buddies as long as possible. */
    while (r < MAXRANK) {
        half = 1 << (r - 1);
        buddy = s ^ half;
        if (buddy + half > npages)
            break;
        if (blk_rank[buddy] == (unsigned char)r && alloc_flag[buddy] == 0) {
            list_remove(r, buddy);
            if (buddy < s)
                s = buddy;
            r++;
            set_rank_range(s, 1 << (r - 1), r);
        } else {
            break;
        }
    }

    list_insert(r, s);
    return OK;
}

int query_ranks(void *p) {
    long off;

    if (!inited || p == NULL)
        return -EINVAL;

    off = (char *)p - pool_base;
    if (off < 0 || off >= (long)npages * PAGESZ || off % PAGESZ != 0)
        return -EINVAL;

    return blk_rank[off / PAGESZ];
}

int query_page_counts(int rank) {
    if (!inited || rank < 1 || rank > MAXRANK)
        return -EINVAL;
    return fcount[rank];
}

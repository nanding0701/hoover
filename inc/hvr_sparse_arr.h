#ifndef _HVR_SPARSE_ARR_H
#define _HVR_SPARSE_ARR_H

#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "dlmalloc.h"
#ifdef __cplusplus
}
#endif
#include "hvr_avl_tree.h"

/*
 * A HOOVER sparse array allows the insertion, deletion, and check for tuple
 * values in a more memory efficient manner than storing them densely.
 *
 * At a high level, a HOOVER sparse array is a map data structure that maps from
 * integer keys to a list of integer values associated with them. As such, you
 * can insert a pairing (i, j), delete the same pairing (i, j), and check if
 * (i, j) is a present pairing in the data structure.
 *
 * A sparse array is constrained in the keys it can have, in that at
 * construction time it is passed in a key 'capacity'. Valid keys for that
 * sparse array must be between 0 and capacity-1.
 *
 * A sparse array chunks this contiguous key range into 'segments', and stores
 * each segment's data in a contiguous chunk of memory. The keys are not
 * explicitly stored, but simple encoded as a given segment and a given offset
 * in that segment. The values associated with each key are explicitly stored in
 * each segment. A segment is only instantiated in memory once a key belonging
 * to that segment has a value actually inserted.
 */

#define HVR_SPARSE_ARR_SEGMENT_SIZE 1024

typedef struct _hvr_sparse_arr_seg_t {
    struct hvr_avl_node *seg[HVR_SPARSE_ARR_SEGMENT_SIZE];
    unsigned seg_size[HVR_SPARSE_ARR_SEGMENT_SIZE];
    // Only used for maintaining segs_pool
    struct _hvr_sparse_arr_seg_t *next;
} hvr_sparse_arr_seg_t;

typedef struct _hvr_sparse_arr_t {
    hvr_sparse_arr_seg_t **segs;
    unsigned capacity;
    unsigned nsegs;

    hvr_sparse_arr_seg_t *segs_pool;
    hvr_sparse_arr_seg_t *preallocated;

    void *pool;
    mspace allocator;

    hvr_avl_node_allocator avl_allocator;
} hvr_sparse_arr_t;

extern void hvr_sparse_arr_init(hvr_sparse_arr_t *arr, unsigned capacity);

extern void hvr_sparse_arr_destroy(hvr_sparse_arr_t *arr);

extern void hvr_sparse_arr_insert(unsigned i, unsigned j,
        hvr_sparse_arr_t *arr);

extern int hvr_sparse_arr_contains(unsigned i, unsigned j,
        hvr_sparse_arr_t *arr);

extern void hvr_sparse_arr_remove(unsigned i, unsigned j,
        hvr_sparse_arr_t *arr);

extern void hvr_sparse_arr_remove_value(unsigned j, hvr_sparse_arr_t *arr);

extern void hvr_sparse_arr_remove_row(unsigned i,
        hvr_sparse_arr_t *arr);

extern unsigned hvr_sparse_arr_linearize_row(unsigned i, uint64_t **out_arr,
        hvr_sparse_arr_t *arr);

extern void hvr_sparse_arr_release_row(uint64_t *out_arr,
        hvr_sparse_arr_t *arr);

extern size_t hvr_sparse_arr_used_bytes(hvr_sparse_arr_t *arr);

#endif

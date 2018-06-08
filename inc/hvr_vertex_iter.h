#ifndef _HVR_VERTEX_ITER_H
#define _HVR_VERTEX_ITER_H

#include "hvr_sparse_vec_pool.h"

typedef struct _hvr_vertex_iter_t {
    hvr_sparse_vec_range_node_t *next;
    unsigned index_for_current_chunk;
    hvr_sparse_vec_t *pool;
} hvr_vertex_iter_t;

void hvr_vertex_iter_init(hvr_vertex_iter_t *iter, hvr_sparse_vec_t *pool);

hvr_sparse_vec_t *hvr_vertex_iter_next(hvr_vertex_iter_t *iter);

#endif // _HVR_VERTEX_ITER_H

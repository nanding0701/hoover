#include <shmem.h>
#include <shmemx.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

#include "hvr_common.h"
#include "hvr_dist_bitvec.h"

#define BITS_PER_ELE (sizeof(hvr_dist_bitvec_ele_t) * BITS_PER_BYTE)

void hvr_dist_bitvec_init(hvr_dist_bitvec_size_t dim0,
        hvr_dist_bitvec_size_t dim1, hvr_dist_bitvec_t *vec) {
    vec->dim0 = dim0;
    vec->dim1 = dim1;

    vec->dim1_length_in_words = (dim1 + BITS_PER_ELE - 1) / BITS_PER_ELE;

    vec->dim0_per_pe = (dim0 + shmem_n_pes() - 1) / shmem_n_pes();

    vec->symm_vec = (hvr_dist_bitvec_ele_t *)shmem_malloc_wrapper(
            vec->dim0_per_pe * vec->dim1_length_in_words *
            sizeof(hvr_dist_bitvec_ele_t));
    assert(vec->symm_vec);
    memset(vec->symm_vec, 0x00, vec->dim0_per_pe * vec->dim1_length_in_words *
            sizeof(hvr_dist_bitvec_ele_t));

    vec->seq_nos = (uint64_t *)shmem_malloc_wrapper(
            vec->dim0_per_pe * sizeof(vec->seq_nos[0]));
    assert(vec->seq_nos);
    memset(vec->seq_nos, 0x00, vec->dim0_per_pe * sizeof(vec->seq_nos[0]));

    int pool_size = 1024 * 1024;
    if (getenv("HVR_DIST_BITVEC_POOL_SIZE")) {
        pool_size = atoi(getenv("HVR_DIST_BITVEC_POOL_SIZE"));
    }
    vec->pool = malloc_helper(pool_size);
    assert(vec->pool || pool_size == 0);
    vec->pool_size = pool_size;

    // Force eager allocation?
    memset(vec->pool, 0xff, pool_size);

    vec->tracker = create_mspace_with_base(vec->pool,
            pool_size, 0);
    assert(vec->tracker);

    shmem_barrier_all();
}

void hvr_dist_bitvec_set(hvr_dist_bitvec_size_t coord0,
        hvr_dist_bitvec_size_t coord1, hvr_dist_bitvec_t *vec) {
    assert(coord0 < vec->dim0);
    assert(coord1 < vec->dim1);

    const unsigned coord0_pe = coord0 / vec->dim0_per_pe;
    const unsigned coord0_offset = coord0 % vec->dim0_per_pe;
    assert(coord0_offset < vec->dim0_per_pe);

    const unsigned coord1_word = coord1 / BITS_PER_ELE;
    const unsigned coord1_bit = coord1 % BITS_PER_WORD;
    hvr_dist_bitvec_ele_t coord1_mask = (((hvr_dist_bitvec_ele_t)1) <<
            ((hvr_dist_bitvec_ele_t)coord1_bit));

    shmem_uint64_atomic_or(
            vec->symm_vec + ((coord0_offset * vec->dim1_length_in_words) +
            coord1_word), coord1_mask, coord0_pe);

    shmem_fence();

    shmem_uint64_atomic_inc(vec->seq_nos + coord0_offset, coord0_pe);
}

void hvr_dist_bitvec_clear(hvr_dist_bitvec_size_t coord0,
        hvr_dist_bitvec_size_t coord1, hvr_dist_bitvec_t *vec) {
    assert(coord0 < vec->dim0);
    assert(coord1 < vec->dim1);

    const unsigned coord0_pe = coord0 / vec->dim0_per_pe;
    const unsigned coord0_offset = coord0 % vec->dim0_per_pe;

    const unsigned coord1_word = coord1 / BITS_PER_ELE;
    const unsigned coord1_bit = coord1 % BITS_PER_WORD;
    hvr_dist_bitvec_ele_t coord1_mask = (((hvr_dist_bitvec_ele_t)1) <<
            ((hvr_dist_bitvec_ele_t)coord1_bit));
    coord1_mask = ~coord1_mask;

    shmem_uint64_atomic_and(
            vec->symm_vec + (coord0_offset * vec->dim1_length_in_words) +
            coord1_word, coord1_mask, coord0_pe);

    shmem_fence();

    shmem_uint64_atomic_inc(vec->seq_nos + coord0_offset, coord0_pe);
}

uint64_t hvr_dist_bitvec_get_seq_no(hvr_dist_bitvec_size_t coord0,
        hvr_dist_bitvec_t *vec) {
    assert(coord0 < vec->dim0);
    const hvr_dist_bitvec_size_t coord0_pe = coord0 / vec->dim0_per_pe;
    const hvr_dist_bitvec_size_t coord0_offset = coord0 % vec->dim0_per_pe;
    assert(coord0_offset < vec->dim0_per_pe);

    uint64_t curr_seq_no = shmem_uint64_atomic_fetch(
            vec->seq_nos + coord0_offset,
            coord0_pe);

    return curr_seq_no;
}

size_t hvr_dist_bitvec_mem_used(hvr_dist_bitvec_t *vec) {
    return vec->dim0_per_pe * vec->dim1_length_in_words *
        sizeof(hvr_dist_bitvec_ele_t) + vec->dim0_per_pe *
        sizeof(vec->seq_nos[0]) + vec->pool_size;
}

void hvr_dist_bitvec_local_subcopy_init(hvr_dist_bitvec_t *vec,
        hvr_dist_bitvec_local_subcopy_t *out) {
    out->coord0 = UINT64_MAX;
    out->dim1 = vec->dim1;
    out->dim1_length_in_words = vec->dim1_length_in_words;
    out->subvec = (hvr_dist_bitvec_ele_t *)mspace_malloc(vec->tracker,
            vec->dim1_length_in_words * sizeof(out->subvec[0]));
    assert(out->subvec);
    memset(out->subvec, 0x00,
            vec->dim1_length_in_words * sizeof(out->subvec[0]));
    out->seq_no = 0;
}

void hvr_dist_bitvec_copy_locally(hvr_dist_bitvec_size_t coord0,
        hvr_dist_bitvec_t *vec, hvr_dist_bitvec_local_subcopy_t *out) {
    assert(out->subvec);
    assert(coord0 < vec->dim0);

    const hvr_dist_bitvec_size_t coord0_pe = coord0 / vec->dim0_per_pe;
    const hvr_dist_bitvec_size_t coord0_offset = coord0 % vec->dim0_per_pe;
    assert(coord0_offset < vec->dim0_per_pe);

    out->coord0 = coord0;

    out->seq_no = shmem_uint64_atomic_fetch(vec->seq_nos + coord0_offset,
            coord0_pe);

    shmem_getmem(out->subvec,
            vec->symm_vec + (coord0_offset * vec->dim1_length_in_words),
            vec->dim1_length_in_words * sizeof(out->subvec[0]), coord0_pe);
}

int hvr_dist_bitvec_local_subcopy_contains(hvr_dist_bitvec_size_t coord1,
        hvr_dist_bitvec_local_subcopy_t *vec) {
    assert(coord1 < vec->dim1);
    const hvr_dist_bitvec_size_t coord1_word = coord1 / BITS_PER_ELE;
    const hvr_dist_bitvec_size_t coord1_bit = coord1 % BITS_PER_WORD;
    hvr_dist_bitvec_size_t coord1_mask = ((uint64_t)1 << coord1_bit);
    if (vec->subvec[coord1_word] & coord1_mask) {
        return 1;
    } else {
        return 0;
    }
}

int hvr_dist_bitvec_owning_pe(hvr_dist_bitvec_size_t coord0,
        hvr_dist_bitvec_t *vec) {
    assert(coord0 < vec->dim0);
    return coord0 / vec->dim0_per_pe;
}

void hvr_dist_bitvec_my_chunk(hvr_dist_bitvec_size_t *lower,
        hvr_dist_bitvec_size_t *upper, hvr_dist_bitvec_t *vec) {
    *lower = shmem_my_pe() * vec->dim0_per_pe;
    *upper = (shmem_my_pe() + 1) * vec->dim0_per_pe;

    if (*lower > vec->dim0) *lower = vec->dim0;
    if (*upper > vec->dim0) *upper = vec->dim0;
}

void hvr_dist_bitvec_local_subcopy_copy(hvr_dist_bitvec_local_subcopy_t *dst,
        hvr_dist_bitvec_local_subcopy_t *src) {
    assert(dst->dim1 == src->dim1);
    assert(dst->dim1_length_in_words == src->dim1_length_in_words);

    dst->coord0 = src->coord0;
    memcpy(dst->subvec, src->subvec,
            src->dim1_length_in_words * sizeof(hvr_dist_bitvec_ele_t));
    dst->seq_no = src->seq_no;
}

void hvr_dist_bitvec_local_subcopy_destroy(hvr_dist_bitvec_t *vec,
        hvr_dist_bitvec_local_subcopy_t *c) {
    mspace_free(vec->tracker, c->subvec);
}

size_t hvr_dist_bitvec_local_subcopy_bytes(
        hvr_dist_bitvec_local_subcopy_t *vec) {
    return sizeof(*vec) + vec->dim1_length_in_words * sizeof(*(vec->subvec));
}

#include <shmem.h>
#include <string.h>
#include <limits.h>

#include "hvr_dist_bitvec.h"
#include "hvr_internal.h"

#define BITS_PER_ELE (sizeof(hvr_dist_bitvec_ele_t) * BITS_PER_BYTE)

void hvr_dist_bitvec_init(hvr_dist_bitvec_size_t dim0,
        hvr_dist_bitvec_size_t dim1, hvr_dist_bitvec *vec) {
    vec->dim0 = dim0;
    vec->dim1 = dim1;

    vec->dim1_length_in_words = (dim1 + BITS_PER_ELE - 1) / BITS_PER_ELE;

    vec->dim0_per_pe = (dim0 + shmem_n_pes() - 1) / shmem_n_pes();

    vec->symm_vec = (hvr_dist_bitvec_ele_t *)shmem_malloc(vec->dim0_per_pe *
            vec->dim1_length_in_words * sizeof(hvr_dist_bitvec_ele_t));
    assert(vec->symm_vec);
    memset(vec->symm_vec, 0x00, vec->dim0_per_pe * vec->dim1_length_in_words *
            sizeof(hvr_dist_bitvec_ele_t));
    shmem_barrier_all();
}

void hvr_dist_bitvec_set(hvr_dist_bitvec_size_t coord0,
        hvr_dist_bitvec_size_t coord1, hvr_dist_bitvec *vec) {
    const unsigned coord0_pe = coord0 / vec->dim0_per_pe;
    const unsigned coord0_offset = coord0 % vec->dim0_per_pe;

    const unsigned coord1_word = coord1 / BITS_PER_ELE;
    const unsigned coord1_bit = coord1 % BITS_PER_WORD;
    unsigned coord1_mask = (1U << coord1_bit);

    assert(sizeof(hvr_dist_bitvec_ele_t) == sizeof(unsigned int));
    shmem_uint_atomic_or(
            vec->symm_vec + (coord0_offset * vec->dim1_length_in_words) +
            coord1_word, coord1_mask, coord0_pe);
}

void hvr_dist_bitvec_clear(hvr_dist_bitvec_size_t coord0,
        hvr_dist_bitvec_size_t coord1, hvr_dist_bitvec *vec) {
    const unsigned coord0_pe = coord0 / vec->dim0_per_pe;
    const unsigned coord0_offset = coord0 % vec->dim0_per_pe;

    const unsigned coord1_word = coord1 / BITS_PER_ELE;
    const unsigned coord1_bit = coord1 % BITS_PER_WORD;
    unsigned coord1_mask = (1U << coord1_bit);
    coord1_mask = ~coord1_mask;

    assert(sizeof(hvr_dist_bitvec_ele_t) == sizeof(unsigned int));
    shmem_uint_atomic_and(
            vec->symm_vec + (coord0_offset * vec->dim1_length_in_words) +
            coord1_word, coord1_mask, coord0_pe);
}

void hvr_dist_bitvec_local_subcopy_init(hvr_dist_bitvec *vec,
        hvr_dist_bitvec_local_subcopy *out) {
    out->coord0 = UINT_MAX;
    out->dim1 = vec->dim1;
    out->subvec = (hvr_dist_bitvec_ele_t *)malloc(
            vec->dim1_length_in_words * sizeof(*(out->subvec)));
    assert(out->subvec);
}

void hvr_dist_bitvec_copy_locally(hvr_dist_bitvec_size_t coord0,
        hvr_dist_bitvec *vec, hvr_dist_bitvec_local_subcopy *out) {
    out->coord0 = coord0;
    const unsigned coord0_pe = coord0 / vec->dim0_per_pe;
    assert(coord0_pe == shmem_my_pe());

    const unsigned coord0_offset = coord0 % vec->dim0_per_pe;

    for (unsigned i = 0; i < vec->dim1_length_in_words; i++) {
        out->subvec[i] = shmem_uint_atomic_fetch(
                vec->symm_vec + (coord0_offset * vec->dim1_length_in_words + i),
                shmem_my_pe());
    }
}

int hvr_dist_bitvec_local_subcopy_contains(hvr_dist_bitvec_size_t coord1,
        hvr_dist_bitvec_local_subcopy *vec) {
    const unsigned coord1_word = coord1 / BITS_PER_ELE;
    const unsigned coord1_bit = coord1 % BITS_PER_WORD;
    unsigned coord1_mask = (1U << coord1_bit);
    if (vec->subvec[coord1_word] & coord1_mask) {
        return 1;
    } else {
        return 0;
    }
}

int hvr_dist_bitvec_owning_pe(hvr_dist_bitvec_size_t coord0,
        hvr_dist_bitvec_size_t coord1, hvr_dist_bitvec *vec) {
    return coord0 / vec->dim0_per_pe;
}

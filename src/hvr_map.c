#include "hvr_map.h"

#include <string.h>

#define HVR_MAP_BUCKET(my_key) ((my_key) % HVR_MAP_BUCKETS)
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static int comp(const void *_a, const void *_b) {
    return ((hvr_map_entry_t *)_a)->key - ((hvr_map_entry_t *)_b)->key;
}

// Add a new key with one initial value
static void hvr_map_seg_add(hvr_vertex_id_t key, hvr_map_val_t val,
        hvr_map_seg_t *s, unsigned init_val_capacity) {
    const unsigned insert_index = s->nkeys;
    assert(insert_index < HVR_MAP_SEG_SIZE);
    s->data[insert_index].key = key;
    s->keys[insert_index] = key;

    assert(HVR_MAP_N_INLINE_VALS > 0);

    s->data[insert_index].inline_vals[0] = val;
    s->data[insert_index].ext_vals = NULL;
    s->data[insert_index].ext_capacity = 0;
    s->data[insert_index].length = 1;

    s->nkeys++;

    if (s->nkeys == HVR_MAP_SEG_SIZE) {
        qsort(&(s->data[0]), HVR_MAP_SEG_SIZE, sizeof(s->data[0]), comp);
        for (unsigned i = 0; i < HVR_MAP_SEG_SIZE; i++) {
            s->keys[i] = s->data[i].key;
        }
    }
}

static inline int binarySearch(hvr_vertex_id_t *keys, int low, int high,
        hvr_vertex_id_t key) { 
    if (high < low) 
        return -1; 
    int mid = (low + high) / 2;  /*low + (high - low)/2;*/
    if (key == keys[mid]) 
        return mid; 
    if (key > keys[mid]) 
        return binarySearch(keys, (mid + 1), high, key); 
    return binarySearch(keys, low, (mid -1), key); 
}

static inline int hvr_map_find(hvr_vertex_id_t key, hvr_map_t *m,
        hvr_map_seg_t **out_seg, unsigned *out_index) {
    unsigned bucket = HVR_MAP_BUCKET(key);

    hvr_map_seg_t *seg = m->buckets[bucket];
    while (seg) {
        const unsigned nkeys = seg->nkeys;
        if (nkeys == HVR_MAP_SEG_SIZE) {
            int index = binarySearch(seg->keys, 0, HVR_MAP_SEG_SIZE, key);
            if (index >= 0) {
                *out_seg = seg;
                *out_index = index;
                return 1;
            }
        } else {
            for (unsigned i = 0; i < nkeys; i++) {
                if (seg->keys[i] == key) {
                    *out_seg = seg;
                    *out_index = i;
                    return 1;
                }
            }
        }
        seg = seg->next;
    }
    return 0;
}

void hvr_map_init(hvr_map_t *m, unsigned n_segs, unsigned init_val_capacity,
        hvr_map_type_t type) {
    memset(m, 0x00, sizeof(*m));
    m->init_val_capacity = init_val_capacity;
    m->type = type;

    hvr_map_seg_t *prealloc = (hvr_map_seg_t *)malloc(n_segs * sizeof(*prealloc));
    assert(prealloc);
    for (unsigned i = 0; i < n_segs - 1; i++) {
        prealloc[i].next = prealloc + (i + 1);
    }
    prealloc[n_segs - 1].next = NULL;
    m->pool = prealloc;
}

void hvr_map_add(hvr_vertex_id_t key, hvr_map_val_t to_insert,
        hvr_map_t *m) {
    hvr_map_seg_t *seg;
    unsigned seg_index;
    int success = hvr_map_find(key, m, &seg, &seg_index);

    if (success) {
        // Key already exists, add it to existing values
        unsigned i = 0;
        unsigned nvals = seg->data[seg_index].length;
        hvr_map_val_t *inline_vals = &(seg->data[seg_index].inline_vals[0]);
        hvr_map_val_t *ext_vals = seg->data[seg_index].ext_vals;

        /*
         * Check if this value is already present for the given key, and if so
         * abort early.
         */
        switch (m->type) {
            case (EDGE_INFO):
                while (i < nvals && i < HVR_MAP_N_INLINE_VALS) {
                    if (EDGE_INFO_VERTEX(inline_vals[i].edge_info) ==
                            EDGE_INFO_VERTEX(to_insert.edge_info)) {
                        assert(EDGE_INFO_EDGE(inline_vals[i].edge_info) ==
                                EDGE_INFO_EDGE(to_insert.edge_info));
                        return;
                    }
                    i++;
                }
                while (i < nvals) {
                    if (EDGE_INFO_VERTEX(ext_vals[i - HVR_MAP_N_INLINE_VALS].edge_info) ==
                            EDGE_INFO_VERTEX(to_insert.edge_info)) {
                        assert(EDGE_INFO_EDGE(ext_vals[i - HVR_MAP_N_INLINE_VALS].edge_info) ==
                                EDGE_INFO_EDGE(to_insert.edge_info));
                        return;
                    }
                    i++;
                }
                break;
            case (CACHED_VERT_INFO):
                while (i < nvals && i < HVR_MAP_N_INLINE_VALS) {
                    if (inline_vals[i].cached_vert == to_insert.cached_vert) {
                        return;
                    }
                    i++;
                }
                while (i < nvals) {
                    if (ext_vals[i - HVR_MAP_N_INLINE_VALS].cached_vert ==
                            to_insert.cached_vert) {
                        return;
                    }
                    i++;
                }
                break;
            case (INTERACT_INFO):
                while (i < nvals && i < HVR_MAP_N_INLINE_VALS) {
                    if (inline_vals[i].interact == to_insert.interact) {
                        return;
                    }
                    i++;
                }
                while (i < nvals) {
                    if (ext_vals[i - HVR_MAP_N_INLINE_VALS].interact ==
                            to_insert.interact) {
                        return;
                    }
                    i++;
                }
                break;
            default:
                abort();
        }

        if (nvals < HVR_MAP_N_INLINE_VALS) {
            // Can immediately insert into inline values.
            inline_vals[nvals] = to_insert;
        } else {
            // Must insert into extended values
            if (nvals - HVR_MAP_N_INLINE_VALS == seg->data[seg_index].ext_capacity) {
                // Need to resize
                unsigned curr_capacity = seg->data[seg_index].ext_capacity;
                unsigned new_capacity = (curr_capacity == 0 ?
                        m->init_val_capacity : 2 * curr_capacity);
                seg->data[seg_index].ext_vals = (hvr_map_val_t *)realloc(ext_vals,
                        new_capacity * sizeof(hvr_map_val_t));
                assert(seg->data[seg_index].ext_vals);
                seg->data[seg_index].ext_capacity = new_capacity;
            }
            seg->data[seg_index].ext_vals[nvals - HVR_MAP_N_INLINE_VALS] = to_insert;
        }

        seg->data[seg_index].length += 1;
    } else {
        const unsigned bucket = HVR_MAP_BUCKET(key);

        // Have to add as new key
        if (m->buckets[bucket] == NULL) {
            // First segment created
            hvr_map_seg_t *new_seg = m->pool;
            assert(new_seg);
            m->pool = new_seg->next;
            memset(new_seg, 0x00, sizeof(*new_seg));

            hvr_map_seg_add(key, to_insert, new_seg, m->init_val_capacity);
            assert(m->buckets[bucket] == NULL);
            m->buckets[bucket] = new_seg;
            m->bucket_tails[bucket] = new_seg;
        } else {
            hvr_map_seg_t *last_seg_in_bucket= m->bucket_tails[bucket];

            if (last_seg_in_bucket->nkeys == HVR_MAP_SEG_SIZE) {
                // Have to append new segment
                hvr_map_seg_t *new_seg = m->pool;
                assert(new_seg);
                m->pool = new_seg->next;
                memset(new_seg, 0x00, sizeof(*new_seg));

                hvr_map_seg_add(key, to_insert, new_seg, m->init_val_capacity);
                assert(last_seg_in_bucket->next == NULL);
                last_seg_in_bucket->next = new_seg;
                m->bucket_tails[bucket] = new_seg;
            } else {
                // Insert in existing segment
                hvr_map_seg_add(key, to_insert, last_seg_in_bucket,
                        m->init_val_capacity);
            }
        }
    }
}

// Remove function for edge info
void hvr_map_remove(hvr_vertex_id_t key, hvr_map_val_t val,
        hvr_map_t *m) {
    hvr_map_seg_t *seg;
    unsigned seg_index;

    const int success = hvr_map_find(key, m, &seg, &seg_index);

    if (success) {
        const unsigned nvals = seg->data[seg_index].length;
        hvr_map_val_t *inline_vals = &(seg->data[seg_index].inline_vals[0]);
        hvr_map_val_t *ext_vals = seg->data[seg_index].ext_vals;
        unsigned j = 0;
        int found = 0;

        // Clear out the value we are removing
        switch (m->type) {
            case (EDGE_INFO):
                while (!found && j < HVR_MAP_N_INLINE_VALS && j < nvals) {
                    if (EDGE_INFO_VERTEX(inline_vals[j].edge_info) ==
                            EDGE_INFO_VERTEX(val.edge_info)) {
                        found = 1;
                    } else {
                        j++;
                    }
                }
                while (!found && j < nvals) {
                    if (EDGE_INFO_VERTEX(ext_vals[j - HVR_MAP_N_INLINE_VALS].edge_info) ==
                            EDGE_INFO_VERTEX(val.edge_info)) {
                        found = 1;
                    } else {
                        j++;
                    }
                }
                break;
            case (CACHED_VERT_INFO):
                while (!found && j < HVR_MAP_N_INLINE_VALS && j < nvals) {
                    if (inline_vals[j].cached_vert == val.cached_vert) {
                        found = 1;
                    } else {
                        j++;
                    }
                }
                while (!found && j < nvals) {
                    if (ext_vals[j - HVR_MAP_N_INLINE_VALS].cached_vert ==
                            val.cached_vert) {
                        found = 1;
                    } else {
                        j++;
                    }
                }
                break;
            case (INTERACT_INFO):
                while (!found && j < HVR_MAP_N_INLINE_VALS && j < nvals) {
                    if (inline_vals[j].interact == val.interact) {
                        found = 1;
                    } else {
                        j++;
                    }
                }
                while (!found && j < nvals) {
                    if (ext_vals[j - HVR_MAP_N_INLINE_VALS].interact ==
                            val.interact) {
                        found = 1;
                    } else {
                        j++;
                    }
                }
                break;
            default:
                abort();
        }

        if (found) {
            // Found it
            assert(j < nvals);
            hvr_map_val_t *copy_to, *copy_from;

            if (j < HVR_MAP_N_INLINE_VALS) {
                copy_to = &inline_vals[j];
            } else {
                copy_to = &ext_vals[j - HVR_MAP_N_INLINE_VALS];
            }

            if (nvals - 1 < HVR_MAP_N_INLINE_VALS) {
                copy_from = &inline_vals[nvals - 1];
            } else {
                copy_from = &ext_vals[nvals - 1 - HVR_MAP_N_INLINE_VALS];
            }

            *copy_to = *copy_from;
            seg->data[seg_index].length = nvals - 1;
        }
    }
}

hvr_edge_type_t hvr_map_contains(hvr_vertex_id_t key,
        hvr_vertex_id_t val, hvr_map_t *m) {
    assert(m->type == EDGE_INFO);

    hvr_map_seg_t *seg;
    unsigned seg_index;

    const int success = hvr_map_find(key, m, &seg, &seg_index);

    if (success) {
        unsigned nvals = seg->data[seg_index].length;
        hvr_map_val_t *inline_vals = &(seg->data[seg_index].inline_vals[0]);
        hvr_map_val_t *ext_vals = seg->data[seg_index].ext_vals;
        unsigned j = 0;
        while (j < MIN(HVR_MAP_N_INLINE_VALS, nvals)) {
            if (EDGE_INFO_VERTEX(inline_vals[j].edge_info) == val) {
                return EDGE_INFO_EDGE(inline_vals[j].edge_info);
            }
            j++;
        }
        while (j < nvals) {
            if (EDGE_INFO_VERTEX(ext_vals[j - HVR_MAP_N_INLINE_VALS].edge_info) == val) {
                return EDGE_INFO_EDGE(ext_vals[j - HVR_MAP_N_INLINE_VALS].edge_info);
            }
            j++;
        }
    }
    return NO_EDGE;
}

int hvr_map_linearize(hvr_vertex_id_t key, hvr_map_t *m,
        hvr_map_val_list_t *out_vals) {
    hvr_map_seg_t *seg;
    unsigned seg_index;

    const int success = hvr_map_find(key, m, &seg, &seg_index);

    if (success) {
        out_vals->inline_vals = &(seg->data[seg_index].inline_vals[0]);
        out_vals->ext_vals = seg->data[seg_index].ext_vals;

        return seg->data[seg_index].length;
    } else {
        return -1;
    }
}

void hvr_map_clear(hvr_map_t *m) {
    for (unsigned i = 0; i < HVR_MAP_BUCKETS; i++) {
        hvr_map_seg_t *seg = m->buckets[i];
        while (seg) {
            hvr_map_seg_t *next = seg->next;
            free(seg);
            seg = next;
        }
        m->buckets[i] = NULL;
        m->bucket_tails[i] = NULL;
    }
}

size_t hvr_map_count_values(hvr_vertex_id_t key, hvr_map_t *m) {
    hvr_map_seg_t *seg;
    unsigned seg_index;

    const int success = hvr_map_find(key, m, &seg, &seg_index);

    if (success) {
        return seg->data[seg_index].length;
    } else {
        return 0;
    }
}

void hvr_map_size_in_bytes(hvr_map_t *m, size_t *out_capacity,
        size_t *out_used, double *avg_val_capacity, double *avg_val_length) {
    size_t capacity = sizeof(*m);
    size_t used = sizeof(*m);
    uint64_t sum_val_capacity = 0;
    uint64_t sum_val_length = 0;
    uint64_t count_vals = 0;

    for (unsigned b = 0; b < HVR_MAP_BUCKETS; b++) {
        hvr_map_seg_t *bucket = m->buckets[b];
        while (bucket) {
            capacity += sizeof(*bucket);
            used += sizeof(*bucket);

            for (unsigned v = 0; v < HVR_MAP_SEG_SIZE; v++) {
                capacity += (HVR_MAP_N_INLINE_VALS + bucket->data[v].ext_capacity) *
                    sizeof(hvr_map_val_t);
                used += bucket->data[v].length * sizeof(hvr_map_val_t);

                if (bucket->data[v].length > 0) {
                    sum_val_capacity += HVR_MAP_N_INLINE_VALS +
                        bucket->data[v].ext_capacity;
                    sum_val_length += bucket->data[v].length;
                    count_vals++;
                }
            }

            bucket = bucket->next;
        }
    }

    *out_capacity = capacity;
    *out_used = used;
    *avg_val_capacity = (double)sum_val_capacity / (double)count_vals;
    *avg_val_length = (double)sum_val_length / (double)count_vals;
}

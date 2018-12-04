#include <string.h>
#include <stdio.h>
#include <shmem.h>
#include <stdint.h>
#include <limits.h>

#include "hvr_vertex_cache.h"

void hvr_vertex_cache_init(hvr_vertex_cache_t *cache,
        hvr_partition_t npartitions) {
    memset(cache, 0x00, sizeof(*cache));

    unsigned prealloc_segs = 768;
    if (getenv("HVR_VERT_CACHE_SEGS")) {
        prealloc_segs = atoi(getenv("HVR_VERT_CACHE_SEGS"));
    }

    hvr_map_init(&cache->cache_map, prealloc_segs, 0, 0, 1, CACHED_VERT_INFO);

    cache->partitions = (hvr_vertex_cache_node_t **)malloc(
            npartitions * sizeof(hvr_vertex_cache_node_t *));
    assert(cache->partitions);
    memset(cache->partitions, 0x00,
            npartitions * sizeof(hvr_vertex_cache_node_t *));
    cache->npartitions = npartitions;

    unsigned n_preallocs = 1024;
    if (getenv("HVR_VEC_CACHE_PREALLOCS")) {
        n_preallocs = atoi(getenv("HVR_VEC_CACHE_PREALLOCS"));
    }

    hvr_vertex_cache_node_t *prealloc =
        (hvr_vertex_cache_node_t *)malloc(n_preallocs * sizeof(*prealloc));
    assert(prealloc);
    memset(prealloc, 0x00, n_preallocs * sizeof(*prealloc));

    prealloc[0].part_next = prealloc + 1;
    prealloc[0].part_prev = NULL;
    prealloc[n_preallocs - 1].part_next = NULL;
    prealloc[n_preallocs - 1].part_prev = prealloc + (n_preallocs - 2);
    for (unsigned i = 1; i < n_preallocs - 1; i++) {
        prealloc[i].part_next = prealloc + (i + 1);
        prealloc[i].part_prev = prealloc + (i - 1);
    }
    cache->pool_head = prealloc + 0;
    cache->pool_mem = prealloc;
    cache->pool_size = n_preallocs;

    cache->n_cached_vertices = 0;

    cache->dist_from_local_vert = (uint8_t *)malloc(
            n_preallocs * sizeof(cache->dist_from_local_vert[0]));
    assert(cache->dist_from_local_vert);
    memset(cache->dist_from_local_vert, 0xff,
            n_preallocs * sizeof(cache->dist_from_local_vert[0]));
}

/*
 * Given a vertex ID on a remote PE, look up that vertex in our local cache.
 * Only returns an entry if the newest timestep stored in that entry is new
 * enough given target_timestep. If no matching entry is found, returns NULL.
 *
 * May lead to evictions of very old cache entries that we now consider unusable
 * because of their age, as judged by CACHED_TIMESTEPS_TOLERANCE.
 */
hvr_vertex_cache_node_t *hvr_vertex_cache_lookup(hvr_vertex_id_t vert,
        hvr_vertex_cache_t *cache) {
    hvr_map_val_list_t vals_list;
    int n = hvr_map_linearize(vert, &cache->cache_map, &vals_list);
    assert(n == -1 || n == 1);

    if (n == -1) {
        cache->cache_perf_info.nmisses++;
        return NULL;
    } else {
        cache->cache_perf_info.nhits++;
        return hvr_map_val_list_get(0, &vals_list).cached_vert;
    }
}

static void linked_list_remove_helper(hvr_vertex_cache_node_t *to_remove,
        hvr_vertex_cache_node_t *prev, hvr_vertex_cache_node_t *next,
        hvr_vertex_cache_node_t **prev_next,
        hvr_vertex_cache_node_t **next_prev,
        hvr_vertex_cache_node_t **head) {
    if (prev == NULL && next == NULL) {
        // Only element in the list
        assert(*head == to_remove);
        *head = NULL;
    } else if (prev == NULL) {
        // Only next is non-null, first element in list
        assert(*head == to_remove);
        *next_prev = NULL;
        *head = next;
    } else if (next == NULL) {
        // Only prev is non-null, last element in list
        *prev_next = NULL;
    } else {
        assert(prev && next);
        assert(*prev_next == *next_prev && *prev_next == to_remove &&
            *next_prev == to_remove);
        *prev_next = next;
        *next_prev = prev;
    }
}

void hvr_vertex_cache_remove_from_local_neighbor_list(
        hvr_vertex_cache_node_t *node, hvr_vertex_cache_t *cache) {
    if (local_neighbor_list_contains(node, cache)) {
        linked_list_remove_helper(node, node->local_neighbors_prev,
                node->local_neighbors_next,
                node->local_neighbors_prev ?
                &(node->local_neighbors_prev->local_neighbors_next) : NULL,
                node->local_neighbors_next ?
                &(node->local_neighbors_next->local_neighbors_prev) : NULL,
                &(cache->local_neighbors_head));
        node->local_neighbors_prev = NULL;
        node->local_neighbors_next = NULL;
    }
}

void hvr_vertex_cache_add_to_local_neighbor_list(hvr_vertex_cache_node_t *node,
        hvr_vertex_cache_t *cache) {
    if (!local_neighbor_list_contains(node, cache)) {
        if (cache->local_neighbors_head) {
            cache->local_neighbors_head->local_neighbors_prev = node;
        }
        node->local_neighbors_next = cache->local_neighbors_head;
        cache->local_neighbors_head = node;
    }
}

void hvr_vertex_cache_delete(hvr_vertex_t *vert, hvr_vertex_cache_t *cache) {
    hvr_vertex_cache_node_t *node = hvr_vertex_cache_lookup(vert->id, cache);
    assert(node);

    hvr_map_val_t to_remove = {.cached_vert = node};
    hvr_map_remove(vert->id, to_remove, &cache->cache_map);

    // Remove from partitions list
    linked_list_remove_helper(node, node->part_prev, node->part_next,
            node->part_prev ? &(node->part_prev->part_next) : NULL,
            node->part_next ? &(node->part_next->part_prev) : NULL,
            &(cache->partitions[node->part]));

    // Remove from local neighbors list if it is present
    if (local_neighbor_list_contains(node, cache)) {
        linked_list_remove_helper(node, node->local_neighbors_prev,
                node->local_neighbors_next,
                node->local_neighbors_prev ?
                    &(node->local_neighbors_prev->local_neighbors_next) : NULL,
                node->local_neighbors_next ?
                    &(node->local_neighbors_next->local_neighbors_prev) : NULL,
                &(cache->local_neighbors_head));
    }

    // Insert into pool using bucket pointers
    if (cache->pool_head) {
        cache->pool_head->part_prev = node;
    }
    node->part_next = cache->pool_head;
    node->part_prev = NULL;
    cache->pool_head = node;

    cache->n_cached_vertices--;
}

hvr_vertex_cache_node_t *hvr_vertex_cache_add(hvr_vertex_t *vert,
        hvr_partition_t part, hvr_vertex_cache_t *cache) {
    // Assume that vec is not already in the cache, but don't enforce this
    hvr_vertex_cache_node_t *new_node = NULL;
    if (cache->pool_head) {
        // Look for an already free node
        new_node = cache->pool_head;
        cache->pool_head = new_node->part_next;
        if (cache->pool_head) {
            cache->pool_head->part_prev = NULL;
        }
    } else {
        // No valid node found, print an error
        fprintf(stderr, "ERROR: PE %d exhausted %u cache slots\n",
                shmem_my_pe(), cache->pool_size);
        abort();
    }

    const unsigned bucket = CACHE_BUCKET(vert->id);
    memcpy(&new_node->vert, vert, sizeof(*vert));
    new_node->part = part;

    // Insert into the appropriate partition list
    if (cache->partitions[part]) {
        cache->partitions[part]->part_prev = new_node;
    }
    new_node->part_next = cache->partitions[part];
    new_node->part_prev = NULL;
    cache->partitions[part] = new_node;

    hvr_map_val_t to_insert = {.cached_vert = new_node};
    hvr_map_add(vert->id, to_insert, &cache->cache_map);

    cache->n_cached_vertices++;

    return new_node;
}

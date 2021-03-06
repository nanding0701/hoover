/* For license: see LICENSE.txt file at top-level */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <shmem.h>
#include <math.h>

#include <hoover.h>

#define ACTOR_ID 0
#define POS 1

unsigned N = 50000;
int pe, npes;

hvr_partition_t actor_to_partition(const hvr_vertex_t *vertex, hvr_ctx_t ctx) {
    return (hvr_partition_t)hvr_vertex_get(POS, vertex, ctx);
}

hvr_edge_type_t should_have_edge(const hvr_vertex_t *a, const hvr_vertex_t *b,
        hvr_ctx_t ctx) {
    unsigned a_pos = (unsigned)hvr_vertex_get(POS, a, ctx);
    unsigned b_pos = (unsigned)hvr_vertex_get(POS, b, ctx);
    unsigned delta_pos = abs((int)a_pos - (int)b_pos);

    if (hvr_vertex_get_owning_pe(a) != hvr_vertex_get_owning_pe(b) &&
            delta_pos <= 2) {
        return BIDIRECTIONAL;
    } else {
        return NO_EDGE;
    }
}

void might_interact(const hvr_partition_t partition,
        hvr_partition_t *interacting_partitions,
        unsigned *n_interacting_partitions,
        unsigned interacting_partitions_capacity,
        hvr_ctx_t ctx) {
    *n_interacting_partitions = 0;
    if (partition > 0) {
        interacting_partitions[*n_interacting_partitions] = partition - 1;
        *n_interacting_partitions += 1;
    }
    if (partition > 1) {
        interacting_partitions[*n_interacting_partitions] = partition - 2;
        *n_interacting_partitions += 1;
    }
    if (partition < N - 1) {
        interacting_partitions[*n_interacting_partitions] = partition + 1;
        *n_interacting_partitions += 1;
    }
    if (partition < N - 2) {
        interacting_partitions[*n_interacting_partitions] = partition + 2;
        *n_interacting_partitions += 1;
    }
}

void update_metadata(hvr_vertex_t *vertex, hvr_set_t *couple_with,
        hvr_ctx_t ctx) {
    hvr_partition_t actor = (hvr_partition_t)hvr_vertex_get(ACTOR_ID, vertex,
            ctx);

    hvr_vertex_t *neighbor = NULL;
    hvr_edge_type_t dir;
    hvr_neighbors_t neighbors;
    hvr_get_neighbors(vertex, &neighbors, ctx);

    hvr_neighbors_next(&neighbors, &neighbor, &dir);

    if (neighbor) {
        unsigned curr_pos = (unsigned)hvr_vertex_get(POS, vertex, ctx);
        unsigned neighbor_pos = (unsigned)hvr_vertex_get(POS, neighbor, ctx);
        unsigned delta_pos = abs((int)curr_pos - (int)neighbor_pos);
        if (actor == 0) {
            // Chase actor 1. Shift +1 when it is +2 ahead of us.
            if (delta_pos > 1) {
                hvr_vertex_set(POS, curr_pos + 1, vertex, ctx);
            }
        } else if (actor == 1) {
            // Run from actor 0. Shift +1 when it is -1 behind us.
            if (delta_pos == 1 && curr_pos < N - 1) {
                hvr_vertex_set(POS, curr_pos + 1, vertex, ctx);
            }
        } else {
            abort();
        }
    }

    hvr_release_neighbors(&neighbors, ctx);
}

void update_coupled_val(hvr_vertex_iter_t *iter, hvr_ctx_t ctx,
        hvr_vertex_t *out_coupled_metric) {
    hvr_vertex_set(0, (double)0, out_coupled_metric, ctx);
}

int should_terminate(hvr_vertex_iter_t *iter, hvr_ctx_t ctx,
        hvr_vertex_t *local_coupled_metric,
        hvr_vertex_t *all_coupled_metrics,
        hvr_vertex_t *global_coupled_metric,
        hvr_set_t *coupled_pes,
        int n_coupled_pes, int *updates_on_this_iter,
        hvr_set_t *terminated_coupled_pes,
        uint64_t n_msgs_this_iter) {
    return 0;
}

int main(int argc, char **argv) {
    hvr_ctx_t hvr_ctx;

    int max_elapsed_seconds = 120;
    if (argc == 2) {
        max_elapsed_seconds = atoi(argv[1]);
    }

    shmem_init();
    hvr_ctx_create(&hvr_ctx);

    pe = shmem_my_pe();
    npes = shmem_n_pes();
    assert(npes == 2);

    hvr_vertex_t *vertices = hvr_vertex_create(hvr_ctx);
    hvr_vertex_set(ACTOR_ID, pe, vertices, hvr_ctx);
    hvr_vertex_set(POS, pe, vertices, hvr_ctx);

    // Statically divide 2D grid into PARTITION_DIM x PARTITION_DIM partitions
    hvr_init(N, // # partitions
            update_metadata,
            might_interact,
            update_coupled_val,
            actor_to_partition,
            NULL, // start_time_step
            should_have_edge,
            should_terminate,
            max_elapsed_seconds, // max_elapsed_seconds
            1, // max_graph_traverse_depth
            1,
            hvr_ctx);

    hvr_body(hvr_ctx);

    shmem_barrier_all();

    fprintf(stderr, "PE %d - (%u, %u) - iters=%d\n", pe,
            (unsigned)hvr_vertex_get(ACTOR_ID, vertices, hvr_ctx),
            (unsigned)hvr_vertex_get(POS, vertices, hvr_ctx),
            hvr_ctx->iter);
    assert((unsigned)hvr_vertex_get(ACTOR_ID, vertices, hvr_ctx) == pe);
    if (pe == 0) {
        if ((unsigned)hvr_vertex_get(POS, vertices, hvr_ctx) != N - 2) {
            fprintf(stderr, "ERROR expected POS (%u) == N - 2 (%u)\n",
                    (unsigned)hvr_vertex_get(POS, vertices, hvr_ctx), N - 2);
            abort();
        }
    } else {
        if ((unsigned)hvr_vertex_get(POS, vertices, hvr_ctx) != N - 1) {
            fprintf(stderr, "ERROR expected POS (%u) == N - 1 (%u)\n",
                    (unsigned)hvr_vertex_get(POS, vertices, hvr_ctx), N - 1);
            abort();
        }
    }
    printf("PE %d SUCCESS\n", pe);

    hvr_finalize(hvr_ctx);

    shmem_finalize();

    return 0;
}

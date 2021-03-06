/* For license: see LICENSE.txt file at top-level */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <shmem.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>

#ifdef CRAYPAT
#include <pat_api.h>
#endif

#include <set>
#include <vector>
#include <algorithm>

#include <hoover.h>
#include <shmem_rw_lock.h>

// #define VERBOSE

#define UNIFORM_DIST
// #define POINT_DIST
// #define GAUSSIAN_DIST

#define MAX(a, b) (((a) > (b)) ? a : b)
#define MIN(a, b) (((a) < (b)) ? a : b)

// Maximum # of vertices allowed in a subgraph
#define MAX_SUBGRAPH_VERTICES 4

// Number of patterns to share with other PEs
#define N_PATTERNS_SHARED 5

#define N_PATTERNS_TO_CONSIDER 6

/*
 * Maximum distance between two patterns for them to be considered anomalous
 * relative to each other. Must be >= 1.
 */
#define MAX_DISTANCE_FOR_ANOMALY 1

static inline int m_get(unsigned i, unsigned j, uint32_t matrix) {
    const unsigned bit = i * MAX_SUBGRAPH_VERTICES + j;
    const uint32_t mask = ((uint32_t)1 << bit);
    return (matrix & mask) > 0;
}

static inline uint32_t m_set(unsigned i, unsigned j, uint32_t matrix) {
    const unsigned bit = i * MAX_SUBGRAPH_VERTICES + j;
    const uint32_t mask = ((uint32_t)1 << bit);
    return (matrix | mask);
}

static inline uint32_t m_clear(unsigned i, unsigned j, uint32_t matrix) {
    const unsigned bit = i * MAX_SUBGRAPH_VERTICES + j;
    const uint32_t mask = ((uint32_t)1 << bit);
    const uint32_t inv_mask = ~mask;
    return (matrix & inv_mask);
}

typedef struct _adjacency_matrix_t {
    uint32_t matrix;
} adjacency_matrix_t;

typedef struct _subgraph_t {
    unsigned n_vertices;
    hvr_vertex_id_t vertices[MAX_SUBGRAPH_VERTICES];
    adjacency_matrix_t adjacency_matrix;
} subgraph_t;

typedef struct _pattern_count_t {
    adjacency_matrix_t matrix;
    unsigned count;
} pattern_count_t;

typedef struct _timestamped_pattern_count_t {
    hvr_time_t iter;
    unsigned n_patterns;
    pattern_count_t patterns[N_PATTERNS_SHARED];
} timestamped_pattern_count_t;

// Timing variables
static unsigned long long start_time = 0;
static unsigned long long time_limit_s = 0;
static long long elapsed_time = 0;
static long long max_elapsed, total_time;

// SHMEM variables
static int pe, npes;
long long p_wrk[SHMEM_REDUCE_MIN_WRKDATA_SIZE];
long p_sync[SHMEM_REDUCE_SYNC_SIZE];

// Application variables
static double distance_threshold = 1.0;
static unsigned domain_dim[3] = {0, 0, 0}; // Size of the global domain
static unsigned pe_chunks_by_dim[3] = {0, 0, 0}; // Number of chunks per dim
static unsigned pe_chunk_size[3] = {0, 0, 0}; // Length of each chunk in each dim
static unsigned partitions_by_dim[3] = {0, 0, 0}; // # of partitions in each dim
static unsigned partitions_size[3] = {0, 0, 0}; // Size of partition in each dim

#define PE_COORD_0(my_pe) ((my_pe) / (pe_chunks_by_dim[1] * \
            pe_chunks_by_dim[2]))
#define PE_COORD_1(my_pe) (((my_pe) / pe_chunks_by_dim[2]) % \
        pe_chunks_by_dim[1])
#define PE_COORD_2(my_pe) ((my_pe) % pe_chunks_by_dim[2])
#define TOTAL_PARTITIONS (partitions_by_dim[0] * partitions_by_dim[1] * \
        partitions_by_dim[2])

static unsigned min_point[3] = {0, 0, 0};
static unsigned max_point[3] = {0, 0, 0};

static unsigned min_n_vertices_to_add = 100;
static unsigned max_n_vertices_to_add = 500;

static timestamped_pattern_count_t *best_patterns = NULL;
static timestamped_pattern_count_t *best_patterns_buffer = NULL;
static long *best_patterns_lock = NULL;
static timestamped_pattern_count_t *neighbor_patterns_buffer = NULL;
static pattern_count_t *sorted_best_patterns = NULL;
static unsigned n_sorted_best_patterns = 0;

#define MAX_LOCAL_PATTERNS 1000
static pattern_count_t known_local_patterns[MAX_LOCAL_PATTERNS];
static unsigned n_known_local_patterns = 0;

#ifdef MULTITHREADED
static unsigned nthreads = 0;
#endif

static unsigned n_local_vertices = 0;

static FILE *pe_anomalies_fp = NULL;

static unsigned *pattern_counts = NULL;
static unsigned possible_permutations = 0;
static pattern_count_t my_best_patterns[N_PATTERNS_SHARED];

#ifdef GAUSSIAN_DIST
// Taken from https://phoxis.org/2013/05/04/generating-random-numbers-from-normal-distribution-in-c/
static double randn(double mu, double sigma) {
    double U1, U2, W, mult;
    static double X1, X2;
    static int call = 0;

    if (call == 1)
    {
        call = !call;
        return (mu + sigma * (double) X2);
    }

    do
    {
        U1 = -1 + ((double) rand () / RAND_MAX) * 2;
        U2 = -1 + ((double) rand () / RAND_MAX) * 2;
        W = pow (U1, 2) + pow (U2, 2);
    }
    while (W >= 1 || W == 0);

    mult = sqrt ((-2 * log (W)) / W);
    X1 = U1 * mult;
    X2 = U2 * mult;

    call = !call;

    return (mu + sigma * (double) X1);
}
#endif // GAUSSIAN_DIST

static unsigned int g_seed;

// Used to seed the generator.
inline void fast_srand(int seed) {
    g_seed = seed;
}

// Compute a pseudorandom integer.
// Output value in range [0, 32767]
inline int fast_rand(void) {
    g_seed = (214013*g_seed+2531011);
    return (g_seed>>16)&0x7FFF;
}

static void rand_point(unsigned *f0, unsigned *f1, unsigned *f2) {
#ifdef GAUSSIAN_DIST
    *f0 = (int)randn((double)(min_point[0] + max_point[0]) / 2.0,
            (max_point[0] - min_point[0]) / 3.0);
    *f1 = (unsigned)randn((double)(min_point[1] + max_point[1]) / 2.0,
            (max_point[1] - min_point[1]) / 3.0);
    *f2 = (unsigned)randn((double)(min_point[2] + max_point[2]) / 2.0,
            (max_point[2] - min_point[2]) / 3.0);
#elif defined(UNIFORM_DIST)
    *f0 = min_point[0] + (fast_rand() % (max_point[0] - min_point[0]));
    *f1 = min_point[1] + (fast_rand() % (max_point[1] - min_point[1]));
    *f2 = min_point[2] + (fast_rand() % (max_point[2] - min_point[2]));
#elif defined(POINT_DIST)
    *f0 = min_point[0] + (max_point[0] - min_point[0]) / 2;
    *f1 = min_point[1] + (max_point[1] - min_point[1]) / 2;
    *f2 = min_point[2] + (max_point[2] - min_point[2]) / 2;
#else
#error No distributed specified
#endif
}

static unsigned pattern_distance(adjacency_matrix_t *a, adjacency_matrix_t *b) {
    const uint32_t delta = a->matrix ^ b->matrix;
    const unsigned count_differences = __builtin_popcount(delta);
    return count_differences;
}

static inline unsigned patterns_identical(adjacency_matrix_t *a,
        adjacency_matrix_t *b) {
    return a->matrix == b->matrix;
}

#if 0
static void adjacency_matrix_to_string(adjacency_matrix_t *a, char *buf,
        size_t buf_size) {
    unsigned buf_index = 0;

    int nwritten = snprintf(buf + buf_index, buf_size - buf_index,
            "[# vertices = %d]\n", a->n_vertices);
    assert(nwritten > 0 && (unsigned)nwritten < buf_size - buf_index);
    buf_index += nwritten;

    for (int i = 0; i < MAX_SUBGRAPH_VERTICES; i++) {
        int nwritten = snprintf(buf + buf_index, buf_size - buf_index,
                "[");
        assert(nwritten > 0 && (unsigned)nwritten < buf_size - buf_index);
        buf_index += nwritten;

        for (int j = 0; j < MAX_SUBGRAPH_VERTICES; j++) {
            int nwritten = snprintf(buf + buf_index, buf_size - buf_index,
                    " %d", m_get(i, j, a->matrix));
            assert(nwritten > 0 && (unsigned)nwritten < buf_size - buf_index);
            buf_index += nwritten;
        }

        nwritten = snprintf(buf + buf_index, buf_size - buf_index, "]\n");
        assert(nwritten > 0 && (unsigned)nwritten < buf_size - buf_index);
        buf_index += nwritten;
    }
}
#endif

static unsigned adjacency_matrix_n_edges(adjacency_matrix_t *a) {
    unsigned count_edges = 0;
    for (unsigned i = 0; i < MAX_SUBGRAPH_VERTICES; i++) {
        for (unsigned j = 0; j <= i; j++) {
            if (m_get(i, j, a->matrix)) {
                count_edges++;
            }
        }
    }
    return count_edges;
}

static int index_in_subgraph(hvr_vertex_id_t vertex, subgraph_t *graph) {
    for (unsigned i = 0; i < graph->n_vertices; i++) {
        if (graph->vertices[i] == vertex) return i;
    }
    return -1;
}

static int already_in_subgraph(hvr_vertex_id_t vertex, subgraph_t *graph) {
    return index_in_subgraph(vertex, graph) >= 0;
}

static int subgraph_has_edge(hvr_vertex_id_t a, hvr_vertex_id_t b,
        subgraph_t *graph) {
    int a_index = index_in_subgraph(a, graph);
    int b_index = index_in_subgraph(b, graph);
    assert(a_index >= 0 && b_index >= 0);

    return m_get(a_index, b_index, graph->adjacency_matrix.matrix) ||
        m_get(b_index, a_index, graph->adjacency_matrix.matrix);
}

static void subgraph_add_edge(hvr_vertex_id_t a, hvr_vertex_id_t b,
        subgraph_t *graph) {
    int a_index = index_in_subgraph(a, graph);
    int b_index = index_in_subgraph(b, graph);
    assert(a_index >= 0 && b_index >= 0);

    // Bi directional for now
    graph->adjacency_matrix.matrix = m_set(a_index, b_index,
            graph->adjacency_matrix.matrix);
    graph->adjacency_matrix.matrix = m_set(b_index, a_index,
            graph->adjacency_matrix.matrix);
}

static inline void subgraph_add_vertex(hvr_vertex_id_t id, subgraph_t *graph) {
    graph->vertices[graph->n_vertices] = id;
    graph->n_vertices += 1;
}

static inline void subgraph_remove_vertex(hvr_vertex_id_t id,
        subgraph_t *graph) {
    int index = index_in_subgraph(id, graph);
    assert(index >= 0);

    graph->vertices[index] = graph->vertices[graph->n_vertices - 1];
    graph->n_vertices -= 1;
}

static void subgraph_remove_edge(hvr_vertex_id_t a, hvr_vertex_id_t b,
        subgraph_t *graph) {
    int a_index = index_in_subgraph(a, graph);
    int b_index = index_in_subgraph(b, graph);
    assert(a_index >= 0 && b_index >= 0);

    // Bi directional for now
    graph->adjacency_matrix.matrix = m_clear(a_index, b_index,
            graph->adjacency_matrix.matrix);
    graph->adjacency_matrix.matrix = m_clear(b_index, a_index,
            graph->adjacency_matrix.matrix);
}

static int find_matching_pattern(pattern_count_t *find, pattern_count_t *l,
        int n) {
    for (int i = 0; i < n; i++) {
        if (patterns_identical(&(find->matrix), &(l[i].matrix))) {
            return i;
        }
    }
    return -1;
}

static inline unsigned explore_subgraphs(hvr_vertex_t *last_added,
        subgraph_t *curr_state, hvr_ctx_t ctx
#ifdef VERBOSE
        , unsigned long long *accum_tracking_time
#endif
        ) {
    unsigned count_explores = 1;

    /*
     * Track the existence of this state so we can find the most common
     * states after this traversal.
     */
#ifdef VERBOSE
    const unsigned long long start_tracking_time = hvr_current_time_us();
#endif

    pattern_counts[curr_state->adjacency_matrix.matrix] += 1;

#ifdef VERBOSE
    *accum_tracking_time += (hvr_current_time_us() - start_tracking_time);
#endif

    if (curr_state->n_vertices < MAX_SUBGRAPH_VERTICES) {
        /*
         * Find the neighbors (i.e. halo regions) of the current vertex in the
         * subgraph. Check if adding any of them results in a change in graph
         * structure (addition of a vertex and/or edge). If it does, make that
         * change and explore further.
         */
        hvr_neighbors_t neighbors;
        hvr_get_neighbors(last_added, &neighbors, ctx);

        hvr_vertex_t *neighbor;
        hvr_edge_type_t neighbor_dir;

        while (hvr_neighbors_next(&neighbors, &neighbor, &neighbor_dir)) {
            if (already_in_subgraph(neighbor->id, curr_state)) {
                /*
                 * 'neighbor' is already in the subgraph (as is
                 * 'existing_vertex'). If the edge
                 * 'neighbor'<->'existing_vertex' is already in the subgraph, do
                 * nothing. Otherwise, add it and iterate.
                 */
                if (!subgraph_has_edge(neighbor->id, last_added->id,
                            curr_state)) {
                    subgraph_add_edge(neighbor->id, last_added->id, curr_state);

                    count_explores += explore_subgraphs(neighbor, curr_state, ctx
#ifdef VERBOSE
                            , accum_tracking_time
#endif
                            );

                    subgraph_remove_edge(neighbor->id, last_added->id,
                            curr_state);
                }
            } else {
                // Add the new vertex and an edge to it.
                subgraph_add_vertex(neighbor->id, curr_state);
                subgraph_add_edge(neighbor->id, last_added->id, curr_state);

                count_explores += explore_subgraphs(neighbor, curr_state, ctx
#ifdef VERBOSE
                        , accum_tracking_time
#endif
                        );
                subgraph_remove_edge(neighbor->id, last_added->id, curr_state);
                subgraph_remove_vertex(neighbor->id, curr_state);
            }
        }
        hvr_release_neighbors(&neighbors, ctx);
    }
    return count_explores;
}

static unsigned score_pattern(pattern_count_t *pattern) {
    return pattern->count * adjacency_matrix_n_edges(&pattern->matrix);
}

hvr_partition_t actor_to_partition(const hvr_vertex_t *actor, hvr_ctx_t ctx) {
    unsigned feat1_partition = (unsigned)hvr_vertex_get(0, actor, ctx) /
        partitions_size[0];
    unsigned feat2_partition = (unsigned)hvr_vertex_get(1, actor, ctx) /
        partitions_size[1];
    unsigned feat3_partition = (unsigned)hvr_vertex_get(2, actor, ctx) /
        partitions_size[2];
    return feat1_partition * partitions_by_dim[1] * partitions_by_dim[2] +
        feat2_partition * partitions_by_dim[2] + feat3_partition;
}

hvr_edge_type_t should_have_edge(const hvr_vertex_t *a, const hvr_vertex_t *b,
        hvr_ctx_t ctx) {
    const double delta0 = hvr_vertex_get(0, b, ctx) -
        hvr_vertex_get(0, a, ctx);
    const double delta1 = hvr_vertex_get(1, b, ctx) -
        hvr_vertex_get(1, a, ctx);
    const double delta2 = hvr_vertex_get(2, b, ctx) -
        hvr_vertex_get(2, a, ctx);
    if (delta0 * delta0 + delta1 * delta1 + delta2 * delta2 <=
            distance_threshold * distance_threshold) {
        return BIDIRECTIONAL;
    } else {
        return NO_EDGE;
    }
}

// Assumes we already hold the write lock on best_patterns_lock for the local PE
static void update_patterns_from(timestamped_pattern_count_t *tmp_buffer,
        int target_pe) {
    hvr_rwlock_rlock(best_patterns_lock, target_pe);
    shmem_getmem(neighbor_patterns_buffer, best_patterns,
            npes * sizeof(timestamped_pattern_count_t), target_pe);
    hvr_rwlock_runlock(best_patterns_lock, target_pe);

    /*
     * Look to see if any of the patterns stored in our left neighbor are
     * more recent than ours
     */
    for (int i = 0; i < npes; i++) {
        if (neighbor_patterns_buffer[i].iter >
                tmp_buffer[i].iter) {
            memcpy(&tmp_buffer[i], &neighbor_patterns_buffer[i],
                    sizeof(timestamped_pattern_count_t));
        }
    }
}

static void copy_to_local_patterns() {
    n_known_local_patterns = 0;

    for (uint32_t pattern = 0; pattern < possible_permutations; pattern++) {
        if (pattern_counts[pattern]) {
            pattern_count_t p;
            p.count = pattern_counts[pattern];
            p.matrix.matrix = pattern;

            memcpy(&known_local_patterns[n_known_local_patterns++], &p,
                    sizeof(p));
        }
    }
}

static void sort_patterns_by_score(pattern_count_t *patterns,
        unsigned n_patterns) {
    for (unsigned i = 0; i < n_patterns; i++) {
        unsigned best_score = score_pattern(patterns + i);
        unsigned best_score_index = i;
        for (unsigned j = i + 1; j < n_patterns; j++) {
            unsigned this_score = score_pattern(patterns + j);
            if (best_score < this_score) {
                best_score = this_score;
                best_score_index = j;
            }
        }

        pattern_count_t tmp;
        memcpy(&tmp, patterns + i, sizeof(tmp));
        memcpy(patterns + i, patterns + best_score_index,
                sizeof(tmp));
        memcpy(patterns + best_score_index, &tmp, sizeof(tmp));
    }
}

void start_time_step(hvr_vertex_iter_t *iter, hvr_set_t *couple_with,
        hvr_ctx_t ctx) {
#ifdef VERBOSE
    const unsigned long long start_callback = hvr_current_time_us();
#endif

    /*
     * On each time step, calculate a random number of vertices to insert. Then,
     * use hvr_sparse_vec_create_n to insert them with 3 randomly generated
     * features which are designed to be most likely to just interact with
     * vertices on this node (but possibly with vertices on other nodes).
     */
    const unsigned n_vertices_to_add = 300;
    // const unsigned n_vertices_to_add = min_n_vertices_to_add +
    //     (fast_rand() % (max_n_vertices_to_add - min_n_vertices_to_add));

    n_local_vertices += n_vertices_to_add;

    for (unsigned i = 0; i < n_vertices_to_add; i++) {
        /*
         * For each PE, have each feature have a gaussian distribution with a
         * mean of 'pe + 1' and a standard deviation of 0.5.
         */
        unsigned feat1, feat2, feat3;
        rand_point(&feat1, &feat2, &feat3);

        feat1 = MAX(feat1, 0);
        feat2 = MAX(feat2, 0);
        feat3 = MAX(feat3, 0);

        feat1 = MIN(feat1, domain_dim[0] - 1);
        feat2 = MIN(feat2, domain_dim[1] - 1);
        feat3 = MIN(feat3, domain_dim[2] - 1);

        hvr_vertex_t *new_vert = hvr_vertex_create(ctx);
        hvr_vertex_set(0, feat1, new_vert, ctx);
        hvr_vertex_set(1, feat2, new_vert, ctx);
        hvr_vertex_set(2, feat3, new_vert, ctx);
    }

    /*
     * Calculate the K most common subgraphs in our current partition of the
     * graph, which may imply fetching vertices from remote nodes.
     *
     * The best candidate subgraph is the one that minimizes the sum of the
     * descriptive length of the subgraph and the overall graph when compressed
     * by the subgraph.
     */
    const unsigned long long start_search = hvr_current_time_us();

    memset(pattern_counts, 0x00,
            possible_permutations * sizeof(*pattern_counts));

    unsigned n_explores = 0;
    unsigned long long accum_tracking_time = 0;

#ifdef MULTITHREADED
    const size_t n_local = hvr_n_local_vertices(ctx);

    hvr_conc_vertex_iter_t conc_iter;
    hvr_conc_vertex_iter_init(&conc_iter, n_local / (nthreads * 5), ctx);

#pragma omp parallel shared(conc_iter, n_explores, accum_tracking_time)
    {
        hvr_conc_vertex_subiter_t chunk;
        const int tid = omp_get_thread_num();
        unsigned thread_explores = 0;

        while (hvr_conc_vertex_iter_next_chunk(&conc_iter, &chunk)) {
            for (hvr_vertex_t *vertex = hvr_conc_vertex_iter_next(&chunk);
                    vertex; vertex = hvr_conc_vertex_iter_next(&chunk)) {
                assert(vertex->id != HVR_INVALID_VERTEX_ID);

                subgraph_t sub;
                memset(&sub, 0x00, sizeof(sub));
                subgraph_add_vertex(vertex->id, &sub);

                thread_explores += explore_subgraphs(vertex, &sub, ctx
#ifdef VERBOSE
                        , &accum_tracking_time
#endif
                        );

            }
        }

#pragma omp atomic
        n_explores += thread_explores;
    }
#else
    /*
     * For each local vertex, initialize a subgraph with that vertex and find
     * all subgraph patterns that can be formed with that vertex as the starting
     * point.
     */
    for (hvr_vertex_t *vertex = hvr_vertex_iter_next(iter); vertex;
            vertex = hvr_vertex_iter_next(iter)) {
        assert(vertex->id != HVR_INVALID_VERTEX_ID);

        subgraph_t sub;
        memset(&sub, 0x00, sizeof(sub));
        subgraph_add_vertex(vertex->id, &sub);

        unsigned n_this_explores = explore_subgraphs(vertex, &sub, ctx
#ifdef VERBOSE
                , &accum_tracking_time
#endif
            );
        n_explores += n_this_explores;
    }
#endif

    copy_to_local_patterns();
    sort_patterns_by_score(known_local_patterns, n_known_local_patterns);

    const unsigned long long end_search = hvr_current_time_us();

    /*
     * Update my remotely accessible best patterns from the patterns I just
     * computed locally and my neighbors' patterns
     */
    memcpy(best_patterns_buffer, best_patterns, npes * sizeof(*best_patterns));
    best_patterns_buffer[pe].iter = ctx->iter;
    best_patterns_buffer[pe].n_patterns = MIN(N_PATTERNS_SHARED,
            n_known_local_patterns);
    memcpy(best_patterns_buffer[pe].patterns, known_local_patterns,
            best_patterns_buffer[pe].n_patterns * sizeof(pattern_count_t));

    if (pe > 0) {
        // Left neighbor
        update_patterns_from(best_patterns_buffer, pe - 1);
    }

    if (pe < npes - 1) {
        // Left neighbor
        update_patterns_from(best_patterns_buffer, pe + 1);
    }

    hvr_rwlock_wlock(best_patterns_lock, pe);
    memcpy(best_patterns, best_patterns_buffer, npes * sizeof(*best_patterns));
    hvr_rwlock_wunlock(best_patterns_lock, pe);

    /*
     * Compute the globally most common patterns based on best_patterns from all
     * PEs.
     */
    n_sorted_best_patterns = 0;
    for (int p = 0; p < npes; p++) {
        // Know that I'm the only writer, so no need to rlock best_patterns here
        for (unsigned i = 0; i < best_patterns[p].n_patterns; i++) {

            int found = 0;
            for (unsigned j = 0; j < n_sorted_best_patterns; j++) {
                if (pattern_distance(&sorted_best_patterns[j].matrix,
                            &best_patterns[p].patterns[i].matrix) == 0) {
                    sorted_best_patterns[j].count +=
                        best_patterns[p].patterns[i].count;
                    found = 1;
                }
            }

            if (!found) {
                memcpy(&sorted_best_patterns[n_sorted_best_patterns],
                        &best_patterns[p].patterns[i], sizeof(pattern_count_t));
                n_sorted_best_patterns++;
            }
        }
    }

    sort_patterns_by_score(sorted_best_patterns, n_sorted_best_patterns);

#ifdef VERBOSE
    const unsigned long long end_callback = hvr_current_time_us();
#endif

    if (n_known_local_patterns > 0) {
#ifdef VERBOSE
        printf("PE %d found %u patterns on iter %d using %d "
                "visits. %f ms inserting new vertices, %f ms to search (%f "
                "counting patterns), %f ms to compute top scores. %u local "
                "vertices in total. Best score = %u, edge "
                "count = %u.\n",
                pe, n_known_local_patterns, ctx->iter,
                n_explores,
                (double)(start_search - start_callback) / 1000.0,
                (double)(end_search - start_search) / 1000.0,
                (double)accum_tracking_time / 1000.0,
                (double)(end_callback - end_search) / 1000.0,
                n_local_vertices,
                score_pattern(known_local_patterns + 0),
                adjacency_matrix_n_edges(&known_local_patterns[0].matrix));
#else
#if 0
        double search_ms = (double)(end_search - start_search) / 1000.0;
        // printf("PE %d found %u patterns on iter %d using %d "
        //         "visits for %u local verts, %f ms to search (%f visits/ms).\n",
        //         pe, n_known_local_patterns, ctx->iter,
        //         n_explores, n_local_vertices,
        //         search_ms, (double)n_explores / search_ms);
        printf("PE %d found %u patterns on iter %d using %d "
                "visits. %f ms inserting new vertices, %f ms to search (%f "
                "counting patterns), %f ms to compute top scores. %u local "
                "vertices in total. Best score = %u, edge "
                "count = %u.\n",
                pe, n_known_local_patterns, ctx->iter,
                n_explores,
                0.0,
                0.0,
                0.0,
                0.0,
                n_local_vertices,
                0,
                0);
#endif
#endif
    }
}

void might_interact(const hvr_partition_t partition,
        hvr_partition_t *interacting_partitions,
        unsigned *out_n_interacting_partitions,
        unsigned interacting_partitions_capacity,
        hvr_ctx_t ctx) {
    /*
     * If a vertex in 'partition' may create an edge with any vertices in any
     * partition in 'partitions', they might interact (so return 1).
     */
    hvr_partition_t feat1_partition = partition /
        (partitions_by_dim[1] * partitions_by_dim[2]);
    hvr_partition_t feat2_partition = (partition / partitions_by_dim[2]) %
        partitions_by_dim[1];
    hvr_partition_t feat3_partition = partition % partitions_by_dim[2];

    int64_t feat1_min = feat1_partition * partitions_size[0];
    int64_t feat1_max = (feat1_partition + 1) * partitions_size[0];

    int64_t feat2_min = feat2_partition * partitions_size[1];
    int64_t feat2_max = (feat2_partition + 1) * partitions_size[1];

    int64_t feat3_min = feat3_partition * partitions_size[2];
    int64_t feat3_max = (feat3_partition + 1) * partitions_size[2];

    feat1_min -= distance_threshold;
    feat1_max += distance_threshold;
    feat2_min -= distance_threshold;
    feat2_max += distance_threshold;
    feat3_min -= distance_threshold;
    feat3_max += distance_threshold;

    feat1_min = MAX(feat1_min, 0);
    feat2_min = MAX(feat2_min, 0);
    feat3_min = MAX(feat3_min, 0);

    feat1_max = MIN(feat1_max, domain_dim[0] - 1);
    feat2_max = MIN(feat2_max, domain_dim[1] - 1);
    feat3_max = MIN(feat3_max, domain_dim[2] - 1);

    unsigned n_interacting_partitions = 0;

    for (unsigned other_feat1_partition = feat1_min / partitions_size[0];
            other_feat1_partition < feat1_max / partitions_size[0];
            other_feat1_partition++) {
        for (unsigned other_feat2_partition = feat2_min / partitions_size[1];
                other_feat2_partition < feat2_max / partitions_size[1];
                other_feat2_partition++) {
            for (unsigned other_feat3_partition = feat3_min / partitions_size[2];
                    other_feat3_partition < feat3_max / partitions_size[2];
                    other_feat3_partition++) {

                unsigned this_part = other_feat1_partition *
                    partitions_by_dim[1] * partitions_by_dim[2] +
                    other_feat2_partition * partitions_by_dim[2] +
                    other_feat3_partition;
                assert(n_interacting_partitions + 1 <=
                        interacting_partitions_capacity);
                interacting_partitions[n_interacting_partitions++] = this_part;
            }
        }
    }
    *out_n_interacting_partitions = n_interacting_partitions;
}

void update_coupled_val(hvr_vertex_iter_t *iter, hvr_ctx_t ctx,
        hvr_vertex_t *out_coupled_metric, uint64_t n_msgs_recvd_this_iter,
        uint64_t n_msgs_sent_this_iter, uint64_t n_msgs_recvd_total,
        uint64_t n_msgs_sent_total) {
    /*
     * Find anomalies based on the patterns in sorted_best_patterns. If
     * those anomalies have edges with vertices in other nodes, couple with
     * those other nodes and produce a report on the anomalies.
     *
     * We do this by looking for patterns known_local_patterns that have a low
     * pattern_distance to the top common patterns but are not in the top common
     * patterns.
     */

    unsigned count_frequent_pattern_matches = 0;
    unsigned count_distance_too_high = 0;
    double avg_distance_too_high = 0.0;
    for (unsigned j = 0; j < n_known_local_patterns; j++) {
        pattern_count_t *local_pattern = known_local_patterns + j;

        int is_a_frequent_pattern = 0;
        int is_similar_to_a_frequent_pattern = -1;
        unsigned lowest_distance = 0;

        for (unsigned i = 0; i < MIN(n_sorted_best_patterns,
                    N_PATTERNS_TO_CONSIDER); i++) {
            pattern_count_t *frequent_pattern = sorted_best_patterns + i;

            unsigned dist = pattern_distance(&(frequent_pattern->matrix),
                    &(local_pattern->matrix));
            if (dist == 0) {
                // Identical to a frequent pattern
                is_a_frequent_pattern = 1;
                break;
            } else if (dist > 0 && dist <= MAX_DISTANCE_FOR_ANOMALY) {
                /*
                 * This defines an anomaly as a local pattern that is similar
                 * but not identical to one of the top globally identified
                 * subgraph patterns but is not itself in the top patterns.
                 */
                if (is_similar_to_a_frequent_pattern < 0) {
                    is_similar_to_a_frequent_pattern = i;
                }
            } else {
                // dist > MAX_DISTANCE_FOR_ANOMALY
                if (lowest_distance == 0 || dist < lowest_distance) {
                    lowest_distance = dist;
                }
            }
        }

        if (is_a_frequent_pattern) {
            count_frequent_pattern_matches++;
        } else if (is_similar_to_a_frequent_pattern >= 0) {
            // char buf[1024];
#ifdef VERBOSE
            printf("PE %d found potentially anomalous pattern on "
                    "iter %d!\n", pe, ctx->iter);
#endif

            // if (pe_anomalies_fp == NULL) {
            //     sprintf(buf, "pe_%d.anomalies.txt", pe);
            //     pe_anomalies_fp = fopen(buf, "w");
            //     assert(pe_anomalies_fp);
            // }

            // fprintf(pe_anomalies_fp, "Found anomaly on timestep %d\n",
            //         hvr_current_timestep(ctx));
            // fprintf(pe_anomalies_fp, "Anomaly (count=%u):\n",
            //         local_pattern->count);
            // adjacency_matrix_to_string(&local_pattern->matrix, buf, 1024);
            // fprintf(pe_anomalies_fp, buf);
            // fprintf(pe_anomalies_fp, "Regular Pattern (count=%u):\n",
            //         sorted_best_patterns[is_similar_to_a_frequent_pattern].count);
            // adjacency_matrix_to_string(
            //         &sorted_best_patterns[is_similar_to_a_frequent_pattern].matrix, buf,
            //         1024);
            // fprintf(pe_anomalies_fp, buf);
            // fprintf(pe_anomalies_fp, "\n");

            // fflush(pe_anomalies_fp);

            /*
             * Become coupled with other PEs whose vertices are parts of this
             * anomalous pattern.
             */
            // std::set<int> *other_pes = pes_sharing_local_patterns[j];
            // for (std::set<int>::iterator i = other_pes->begin(),
            //         e = other_pes->end(); i != e; i++) {
            //     int other_pe = *i;
            //     hvr_set_insert(other_pe, to_couple_with);
            // }
        } else {
            avg_distance_too_high += lowest_distance;
            count_distance_too_high++;
        }
    }

    // TODO anything useful we'd like to use the coupled metric for?
    hvr_vertex_set(0, 0.0, out_coupled_metric, ctx);

#ifdef VERBOSE
    unsigned long long time_so_far = hvr_current_time_us() - start_time;
    printf("PE %d - elapsed time = %f s, time limit = "
            "%f s, # local vertices = %u, # local patterns = %u, # frequent "
            "patterns = %u, # exact pattern matches = %u, # times distance too "
            "high = %u, avg distance too high = %f\n", pe,
            (double)time_so_far / 1000000.0,
            (double)time_limit_s,
            n_local_vertices,
            n_known_local_patterns,
            n_sorted_best_patterns,
            count_frequent_pattern_matches,
            count_distance_too_high,
            avg_distance_too_high / (double)count_distance_too_high);
#endif
}

int should_terminate(hvr_vertex_iter_t *iter, hvr_ctx_t ctx,
        hvr_vertex_t *local_coupled_val,
        hvr_vertex_t *all_coupled_vals,
        hvr_set_t *coupled_pes, int n_coupled_pes,
        int *updates_on_this_iter,
        hvr_set_t *terminated_coupled_pes,
        uint64_t n_msgs_recvd_this_iter,
        uint64_t n_msgs_sent_this_iter,
        uint64_t n_msgs_recvd_total,
        uint64_t n_msgs_sent_total) {
    return 0;
}

int main(int argc, char **argv) {
#ifdef CRAYPAT
    PAT_record(PAT_STATE_OFF);
#endif
    hvr_ctx_t hvr_ctx;
    assert(HVR_MAX_VECTOR_SIZE == 3);

    if (argc != 14) {
        fprintf(stderr, "usage: %s <time-limit-in-seconds> "
                "<distance-threshold> <domain-size-0> <domain-size-1> "
                "<domain-size-2> <pe-dim-0> <pe-dim-1> <pe-dim-2> "
                "<partition-dim-0> <partition-dim-1> <partition-dim-2>"
                "<min-vertices-to-add> <max-vertices-to-add>\n",
                argv[0]);
        return 1;
    }

    time_limit_s = atoi(argv[1]);
    distance_threshold = atof(argv[2]);
    domain_dim[0] = atoi(argv[3]);
    domain_dim[1] = atoi(argv[4]);
    domain_dim[2] = atoi(argv[5]);
    pe_chunks_by_dim[0] = atoi(argv[6]);
    pe_chunks_by_dim[1] = atoi(argv[7]);
    pe_chunks_by_dim[2] = atoi(argv[8]);
    partitions_by_dim[0] = atoi(argv[9]);
    partitions_by_dim[1] = atoi(argv[10]);
    partitions_by_dim[2] = atoi(argv[11]);
    min_n_vertices_to_add = atoi(argv[12]);
    max_n_vertices_to_add = atoi(argv[13]);

    if (min_n_vertices_to_add >= max_n_vertices_to_add) {
        fprintf(stderr, "Minimum # vertices to add must be less than the "
                "maximum. Minimum = %u, maximum = %u.\n", min_n_vertices_to_add,
                max_n_vertices_to_add);
        return 1;
    }

    for (int i = 0; i < SHMEM_REDUCE_SYNC_SIZE; i++) {
        p_sync[i] = SHMEM_SYNC_VALUE;
    }

    possible_permutations = (unsigned)pow(2,
            MAX_SUBGRAPH_VERTICES * MAX_SUBGRAPH_VERTICES);
    pattern_counts = (unsigned *)malloc(
            possible_permutations * sizeof(*pattern_counts));
    assert(pattern_counts);

    shmem_init();

    pe = shmem_my_pe();
    npes = shmem_n_pes();

    if (pe == 0) {
        printf("Domain size = %u x %u x %u\n", domain_dim[0], domain_dim[1],
                domain_dim[2]);
        printf("PE cube = %u x %u x %u\n", pe_chunks_by_dim[0],
                pe_chunks_by_dim[1], pe_chunks_by_dim[2]);
        printf("Partitions cube = %u x %u x %u\n", partitions_by_dim[0],
                partitions_by_dim[1], partitions_by_dim[2]);
    }

    assert(pe_chunks_by_dim[0] * pe_chunks_by_dim[1] * pe_chunks_by_dim[2] ==
            (unsigned)npes);
    assert(domain_dim[0] % pe_chunks_by_dim[0] == 0);
    assert(domain_dim[1] % pe_chunks_by_dim[1] == 0);
    assert(domain_dim[2] % pe_chunks_by_dim[2] == 0);

    pe_chunk_size[0] = domain_dim[0] / pe_chunks_by_dim[0];
    pe_chunk_size[1] = domain_dim[1] / pe_chunks_by_dim[1];
    pe_chunk_size[2] = domain_dim[2] / pe_chunks_by_dim[2];

    min_point[0] = PE_COORD_0(pe) * pe_chunk_size[0];
    min_point[1] = PE_COORD_1(pe) * pe_chunk_size[1];
    min_point[2] = PE_COORD_2(pe) * pe_chunk_size[2];

    max_point[0] = min_point[0] + pe_chunk_size[0];
    max_point[1] = min_point[1] + pe_chunk_size[1];
    max_point[2] = min_point[2] + pe_chunk_size[2];

    assert(domain_dim[0] % partitions_by_dim[0] == 0);
    assert(domain_dim[1] % partitions_by_dim[1] == 0);
    assert(domain_dim[2] % partitions_by_dim[2] == 0);
    partitions_size[0] = domain_dim[0] / partitions_by_dim[0];
    partitions_size[1] = domain_dim[1] / partitions_by_dim[1];
    partitions_size[2] = domain_dim[2] / partitions_by_dim[2];

    if (pe == 0) {
        printf("%d PE(s) running...\n", npes);
    }
    printf("PE %d responsible for (%u, %u, %u) -> (%u, %u, %u)\n", pe,
            min_point[0], min_point[1], min_point[2], max_point[0],
            max_point[1], max_point[2]);

    hvr_ctx_create(&hvr_ctx);

    fast_srand(123 + pe);

    hvr_init(TOTAL_PARTITIONS, // # partitions
            NULL, // update_metadata
            might_interact,
            update_coupled_val,
            actor_to_partition,
            start_time_step,
            should_have_edge,
            should_terminate,
            time_limit_s,
            MAX_SUBGRAPH_VERTICES,
            1, // send_neighbor_updates_for_explicit_subs
            hvr_ctx);

    best_patterns = (timestamped_pattern_count_t *)shmem_malloc(
            npes * sizeof(*best_patterns));
    assert(best_patterns);
    best_patterns_buffer = (timestamped_pattern_count_t *)malloc(
            npes * sizeof(*best_patterns_buffer));
    assert(best_patterns_buffer);
    neighbor_patterns_buffer = (timestamped_pattern_count_t *)malloc(
            npes * sizeof(*neighbor_patterns_buffer));
    assert(neighbor_patterns_buffer);
    best_patterns_lock = (long *)hvr_rwlock_create_n(1);
    assert(best_patterns_lock);

    sorted_best_patterns = (pattern_count_t *)malloc(
            npes * N_PATTERNS_SHARED * sizeof(*sorted_best_patterns));
    assert(sorted_best_patterns);

    *best_patterns_lock = 0;
    for (int i = 0; i < npes; i++) {
        best_patterns[i].iter = -1;
    }

    shmem_barrier_all();

    start_time = hvr_current_time_us();
#ifdef CRAYPAT
    PAT_record(PAT_STATE_ON);
#endif
    hvr_exec_info info = hvr_body(hvr_ctx);
#ifdef CRAYPAT
    PAT_record(PAT_STATE_OFF);
#endif
    elapsed_time = info.start_hvr_body_wrapup_us - info.start_hvr_body_us;

    printf("PE %d found %u patterns on iter %d using %d "
            "visits. %f ms inserting new vertices, %f ms to search (%f "
            "counting patterns), %f ms to compute top scores. %u local "
            "vertices in total. Best score = %u, edge "
            "count = %u.\n",
            pe, n_known_local_patterns, info.executed_iters,
            0,
            0.0,
            0.0,
            0.0,
            0.0,
            n_local_vertices,
            0,
            0);


    // Get a total wallclock time across all PEs
    shmem_longlong_sum_to_all(&total_time, &elapsed_time, 1, 0, 0, npes, p_wrk,
            p_sync);
    shmem_barrier_all();

    // Get a max wallclock time across all PEs
    shmem_longlong_max_to_all(&max_elapsed, &elapsed_time, 1, 0, 0, npes, p_wrk,
            p_sync);
    shmem_barrier_all();

    if (pe == 0) {
        printf("%d PEs, total CPU time = %f ms, max elapsed = %f ms, "
                "%d iterations completed on PE 0\n", npes,
                (double)total_time / 1000.0, (double)max_elapsed / 1000.0,
                info.executed_iters);
    }

    hvr_finalize(hvr_ctx);

    shmem_finalize();

    return 0;
}

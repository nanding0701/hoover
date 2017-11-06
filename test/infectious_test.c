#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <shmem.h>
#include <math.h>
#include <string.h>

#include <hoover.h>

#define PARTITION_DIM 100

#define PX 0
#define PY 1
#define INFECTED 2
#define VX 3
#define VY 4

#define PORTAL_CAPTURE_RADIUS 5.0
#define PE_ROW(this_pe) ((this_pe) / pe_cols)
#define PE_COL(this_pe) ((this_pe) % pe_cols)
#define PE_ROW_CELL_START(this_pe) ((double)PE_ROW(this_pe) * cell_dim)
#define PE_COL_CELL_START(this_pe) ((double)PE_COL(this_pe) * cell_dim)
#define CELL_ROW(y_coord) ((y_coord) / cell_dim)
#define CELL_COL(x_coord) ((x_coord) / cell_dim)
#define CELL_INDEX(cell_row, cell_col) ((cell_row) * pe_cols + (cell_col))

typedef struct _portal_t {
    int pes[2];
    struct {
        double x, y;
    } locations[2];
} portal_t;

static int npes, pe;
static int pe_rows, pe_cols;
static portal_t *portals = NULL;
static int n_global_portals = 0;
static int actors_per_cell;
static double cell_dim;
long long total_time = 0;
long long max_elapsed = 0;
long long elapsed_time = 0;
static double max_delta_velocity;
static double infection_radius;

/*
 * Construct a 2D grid, with one grid cell per PE. Build connections between
 * cells, having each cell connected to all eight neighbor cells plus some
 * randomly added long distance interactions.
 *
 * Inside of each cell of the 2D grid, generate a random number of actors that
 * perform random walks on the whole grid (possibly jumping to other cells) and
 * infect each other if within certain distances.
 */

long long p_wrk[SHMEM_REDUCE_MIN_WRKDATA_SIZE];
long p_sync[SHMEM_REDUCE_SYNC_SIZE];

static double distance(double x1, double y1, double x2, double y2) {
    double deltax = x2 - x1;
    double deltay = y2 - y1;
    return sqrt((deltay * deltay) + (deltax * deltax));
}

static double random_double_in_range(double min_val_inclusive,
        double max_val_exclusive) {
    return min_val_inclusive + (((double)rand() / (double)RAND_MAX) *
            (max_val_exclusive - min_val_inclusive));
}

static int random_int_in_range(int max_val) {
    return (rand() % max_val);
}

/*
 * Callback for the HOOVER runtime to use to determine the PE owning a given
 * vertex, and that vertex's local offset on the owner PE.
 */
void vertex_owner(vertex_id_t vertex, unsigned *out_pe,
        size_t *out_local_offset) {
    *out_pe = vertex / actors_per_cell;
    *out_local_offset = vertex % actors_per_cell;
}

uint16_t actor_to_partition(hvr_sparse_vec_t *actor, hvr_ctx_t ctx) {
    const double x = hvr_sparse_vec_get(PX, actor, ctx);
    const double y = hvr_sparse_vec_get(PY, actor, ctx);

    const double global_x_dim = (double)pe_cols * cell_dim;
    const double global_y_dim = (double)pe_rows * cell_dim;

    const double partition_x_dim = global_x_dim / (double)PARTITION_DIM;
    const double partition_y_dim = global_y_dim / (double)PARTITION_DIM;

    const int x_partition = (int)(x / partition_x_dim);
    const int y_partition = (int)(y / partition_y_dim);
    return y_partition * PARTITION_DIM + x_partition;
}

/*
 * Callback for the HOOVER runtime for updating positional or logical metadata
 * attached to each vertex based on the updated neighbors on each time step.
 */
void update_metadata(hvr_sparse_vec_t *vertex, hvr_sparse_vec_t *neighbors,
        const size_t n_neighbors, hvr_pe_set_t *couple_with, hvr_ctx_t ctx) {
    /*
     * If vertex is not already infected, update it to be infected if any of its
     * neighbors are.
     */
    if (hvr_sparse_vec_get(INFECTED, vertex, ctx) == 0.0) {
        for (int i = 0; i < n_neighbors; i++) {
            if (hvr_sparse_vec_get(INFECTED, &neighbors[i], ctx) > 0.0) {
                const int infected_by = hvr_sparse_vec_get_owning_pe(
                        &neighbors[i]);
                hvr_pe_set_insert(infected_by, couple_with);
                hvr_sparse_vec_set(INFECTED, 1.0, vertex, ctx);
                break;
            }
        }
    }

    // Update location of this cell
    double delta_vx = random_double_in_range(-max_delta_velocity,
            max_delta_velocity);
    double delta_vy = random_double_in_range(-max_delta_velocity,
            max_delta_velocity);
    double vx = hvr_sparse_vec_get(VX, vertex, ctx);
    double vy = hvr_sparse_vec_get(VY, vertex, ctx);
    double new_x = hvr_sparse_vec_get(PX, vertex, ctx) + vx;
    double new_y = hvr_sparse_vec_get(PY, vertex, ctx) + vy;

    for (int p = 0; p < n_global_portals; p++) {
        if (distance(new_x, new_y, portals[p].locations[0].x,
                    portals[p].locations[0].y) < PORTAL_CAPTURE_RADIUS) {
            new_x = portals[p].locations[1].x +
                random_double_in_range(-10.0, 10.0);
            new_y = portals[p].locations[1].y +
                random_double_in_range(-10.0, 10.0);
            break;
        }

        if (distance(new_x, new_y, portals[p].locations[1].x,
                    portals[p].locations[1].y) < PORTAL_CAPTURE_RADIUS) {
            new_x = portals[p].locations[0].x +
                random_double_in_range(-10.0, 10.0);
            new_y = portals[p].locations[0].y +
                random_double_in_range(-10.0, 10.0);
            break;
        }
    }

    double global_x_dim = (double)pe_cols * cell_dim;
    double global_y_dim = (double)pe_rows * cell_dim;
    if (new_x > global_x_dim) {
        new_x -= global_x_dim;
    }
    if (new_y > global_y_dim) {
        new_y -= global_y_dim;
    }
    if (new_x < 0.0) {
        new_x += global_x_dim;
    }
    if (new_y < 0.0) {
        new_y += global_y_dim;
    }

    hvr_sparse_vec_set(PX, new_x, vertex, ctx);
    hvr_sparse_vec_set(PY, new_y, vertex, ctx);
    hvr_sparse_vec_set(VX, vx + delta_vx, vertex, ctx);
    hvr_sparse_vec_set(VY, vy + delta_vy, vertex, ctx);
}


int update_summary_data(void *_summary, hvr_sparse_vec_t *actors,
        const int nactors, hvr_ctx_t ctx) {
    static char *new_summary = NULL;
    char *existing_summary = (char *)_summary;
    const int nbytes = ((pe_rows * pe_cols) + 8 - 1) / 8;

    if (new_summary == NULL) {
        new_summary = (char *)malloc(nbytes);
    }

    memset(new_summary, 0x00, nbytes);
    for (int a = 0; a < nactors; a++) {
        int row = CELL_ROW(hvr_sparse_vec_get(PY, &actors[a], ctx));
        int col = CELL_ROW(hvr_sparse_vec_get(PX, &actors[a], ctx));
        int cell = row * pe_cols + col;
        new_summary[cell / 8] |= (1 << (cell % 8));
    }

    int any_change = 0;
    for (int i = 0; i < nbytes; i++) {
        if (new_summary[i] != existing_summary[i]) {
            any_change = 1;
            memcpy(existing_summary, new_summary, nbytes);
            break;
        }
    }

    return any_change;
}

/*
 * Callback used to check if this PE might interact with another PE based on the
 * maximums and minimums of all vertices owned by each PE.
 */
int might_interact(const uint16_t partition, hvr_pe_set_t *partitions,
        hvr_ctx_t ctx) {
    /*
     * If partition is neighboring any partition in partitions, they might
     * interact.
     */

    // Each partition has this length on each side
    const double global_cols_dim = (double)pe_cols * cell_dim;
    const double global_rows_dim = (double)pe_rows * cell_dim;

    double cols_dim = global_cols_dim / (double)PARTITION_DIM;
    double rows_dim = global_rows_dim / (double)PARTITION_DIM;
    const int partition_row = partition / PARTITION_DIM;
    const int partition_col = partition % PARTITION_DIM;

    // Get bounding box of partitions in the grid coordinate system
    double min_row = (double)partition_row * rows_dim;
    double max_row = min_row + rows_dim;
    double min_col = (double)partition_col * cols_dim;
    double max_col = min_col + cols_dim;

    /*
     * Expand partition bounding box to include any possible points within
     * infection_radius distance.
     */
    min_row -= infection_radius;
    max_row += infection_radius;
    min_col -= infection_radius;
    max_col += infection_radius;

    int min_partition_row, min_partition_col, max_partition_row,
        max_partition_col;

    if (min_row < 0.0) min_partition_row = 0;
    else min_partition_row = (int)(min_row / rows_dim);

    if (min_col < 0.0) min_partition_col = 0;
    else min_partition_col = (int)(min_col / cols_dim);

    if (max_row >= (double)global_rows_dim) max_partition_row = PARTITION_DIM - 1;
    else max_partition_row = (int)(max_row / rows_dim);

    if (max_col >= (double)global_cols_dim) max_partition_col = PARTITION_DIM - 1;
    else max_partition_col = (int)(max_col / cols_dim);

    assert(min_partition_row <= max_partition_row);
    assert(min_partition_col <= max_partition_col);

    for (int r = min_partition_row; r <= max_partition_row; r++) {
        for (int c = min_partition_col; c <= max_partition_col; c++) {
            const int part = r * PARTITION_DIM + c;
            if (hvr_pe_set_contains(part, partitions)) {
                return 1;
            }
        }
    }
    return 0;
}

static unsigned long long last_time = 0;

/*
 * Callback used by the HOOVER runtime to check if this PE can abort out of the
 * simulation.
 */
int check_abort(hvr_sparse_vec_t *vertices, const size_t n_vertices,
        hvr_ctx_t ctx, hvr_sparse_vec_t *out_coupled_metric) {
    // Abort if all of my member vertices are infected
    size_t nset = 0;
    for (int i = 0; i < n_vertices; i++) {
        if (hvr_sparse_vec_get(INFECTED, &vertices[i], ctx) > 0.0) {
            nset++;
        }
    }

    unsigned long long this_time = hvr_current_time_us();
    printf("PE %d - timestep %lu - set %lu / %lu - %f ms\n", pe,
            hvr_current_timestep(ctx), nset, n_vertices,
            last_time == 0 ? 0 : (double)(this_time - last_time) / 1000.0);
    last_time = this_time;

    // Only really makes sense when running on one PE for testing
    // printf("\nPE %d - timestep %lu:\n", pe, hvr_current_timestep(ctx));
    // for (int i = grid_dim - 1; i >= 0; i--) {
    //     printf("  ");
    //     for (int j = 0; j < grid_dim; j++) {
    //         if (hvr_sparse_vec_get(2, &vertices[i * grid_dim + j], ctx) > 0.0) {
    //             printf(" 1");
    //         } else {
    //             printf(" 0");
    //         }
    //     }
    //     printf("\n");
    // }

    hvr_sparse_vec_set(0, (double)nset, out_coupled_metric, ctx);
    if (nset == n_vertices) {
        return 1;
    } else {
        return 0;
    }
}

int main(int argc, char **argv) {
    hvr_ctx_t hvr_ctx;

    if (argc != 10) {
        fprintf(stderr, "usage: %s <cell-dim> <# global portals> "
                "<actors per cell> <pe-rows> <pe-cols> <n-initial-infected> "
                "<max-num-timesteps> <infection-radius> <max-delta-velocity>\n",
                argv[0]);
        return 1;
    }

    cell_dim = atof(argv[1]);
    n_global_portals = atoi(argv[2]);
    actors_per_cell = atoi(argv[3]);
    pe_rows = atoi(argv[4]);
    pe_cols = atoi(argv[5]);
    const int n_initial_infected = atoi(argv[6]);
    const int max_num_timesteps = atoi(argv[7]);
    infection_radius = atof(argv[8]);
    max_delta_velocity = atof(argv[9]);

    for (int i = 0; i < SHMEM_REDUCE_SYNC_SIZE; i++) {
        p_sync[i] = SHMEM_SYNC_VALUE;
    }

    shmem_init();
    hvr_ctx_create(&hvr_ctx);

    pe = shmem_my_pe();
    npes = shmem_n_pes();
    assert(npes == pe_rows * pe_cols);

    srand(123 + pe);

    /*
     * Every cell/PE allows direct transitions of actors out of it to any of its
     * eight neighboring PEs. Each cell/PE also optionally contains what we call
     * "portals" which are essentially points in the cell which are connected to
     * points in farther away cells to model longer distance interactions (e.g.
     * plane, train routes). If an actor hits one of these portals, they are
     * "teleported" to the other end of the portal.
     */
    portals = (portal_t *)shmem_malloc(
            n_global_portals * sizeof(*portals));
    assert(portals);
    if (pe == 0) {
        fprintf(stderr, "Creating %d portals\n", n_global_portals);
        for (int p = 0; p < n_global_portals; p++) {
            const int pe0 = (rand() % npes);
            const int pe1 = (rand() % npes);

            portals[p].pes[0] = pe0;
            portals[p].pes[1] = pe1;
            portals[p].locations[0].x = random_double_in_range(
                    PE_COL_CELL_START(pe0), PE_COL_CELL_START(pe0) + cell_dim);
            portals[p].locations[0].y = random_double_in_range(
                    PE_ROW_CELL_START(pe0), PE_ROW_CELL_START(pe0) + cell_dim);
            portals[p].locations[1].x = random_double_in_range(
                    PE_COL_CELL_START(pe1), PE_COL_CELL_START(pe1) + cell_dim);
            portals[p].locations[1].y = random_double_in_range(
                    PE_ROW_CELL_START(pe1), PE_ROW_CELL_START(pe1) + cell_dim);
            fprintf(stderr, "  %d - %d : (%f, %f) - (%f, %f)\n", pe0, pe1,
                    portals[p].locations[0].x, portals[p].locations[0].y,
                    portals[p].locations[1].x, portals[p].locations[1].y);
        }
    }
    assert((n_global_portals * sizeof(*portals)) % 4 == 0);
    shmem_broadcast32(portals, portals,
            (n_global_portals * sizeof(*portals)) / 4, 0, 0, 0, npes, p_sync);
    shmem_barrier_all();

    // Seed the initial infected actors.
    int *initial_infected = (int *)shmem_malloc(
            n_initial_infected * sizeof(*initial_infected));
    assert(initial_infected);
    if (pe == 0) {
        for (int i = 0; i < n_initial_infected; i++) {
            initial_infected[i] = random_int_in_range(npes * actors_per_cell);
        }
    }
    assert(sizeof(int) == 4);
    shmem_broadcast32(initial_infected, initial_infected, n_initial_infected, 0,
            0, 0, npes, p_sync);
    shmem_barrier_all();

    // Seed the location of local actors.
    hvr_sparse_vec_t *actors = hvr_sparse_vec_create_n(actors_per_cell);
    for (int a = 0; a < actors_per_cell; a++) {
        const double x = random_double_in_range(PE_COL_CELL_START(pe),
                PE_COL_CELL_START(pe) + cell_dim);
        const double y = random_double_in_range(PE_ROW_CELL_START(pe),
                PE_ROW_CELL_START(pe) + cell_dim);
        const double vx = random_double_in_range(-0.3, 0.3);
        const double vy = random_double_in_range(-0.3, 0.3);

        hvr_sparse_vec_set_id(pe * actors_per_cell + a, &actors[a]);
        hvr_sparse_vec_set(PX, x, &actors[a], hvr_ctx);
        hvr_sparse_vec_set(PY, y, &actors[a], hvr_ctx);
        hvr_sparse_vec_set(VX, vx, &actors[a], hvr_ctx);
        hvr_sparse_vec_set(VY, vy, &actors[a], hvr_ctx);

        int is_infected = 0;
        for (int i = 0; i < n_initial_infected; i++) {
            int owner_pe = initial_infected[i] / actors_per_cell;
            if (owner_pe == pe) {
                int local_offset = initial_infected[i] % actors_per_cell;
                assert(local_offset < actors_per_cell);
                if (local_offset == a)  {
                    is_infected = 1;
                    break;
                }
            }
        }

        if (is_infected) {
            fprintf(stderr, "PE %d - local offset %d infected\n", pe, a);
            hvr_sparse_vec_set(INFECTED, 1.0, &actors[a], hvr_ctx);
        } else {
            hvr_sparse_vec_set(INFECTED, 0.0, &actors[a], hvr_ctx);
        }
    }

    hvr_init(PARTITION_DIM * PARTITION_DIM, actors_per_cell, actors,
            update_metadata, might_interact, check_abort,
            vertex_owner, actor_to_partition, infection_radius /* threshold */,
            0, 1, max_num_timesteps, hvr_ctx);

    const long long start_time = hvr_current_time_us();
    hvr_body(hvr_ctx);
    elapsed_time = hvr_current_time_us() - start_time;

    shmem_longlong_sum_to_all(&total_time, &elapsed_time, 1, 0, 0, npes, p_wrk,
            p_sync);
    shmem_barrier_all();
    shmem_longlong_max_to_all(&max_elapsed, &elapsed_time, 1, 0, 0, npes, p_wrk,
            p_sync);
    shmem_barrier_all();

    hvr_finalize(hvr_ctx);

    shmem_finalize();

    if (pe == 0) {
        printf("%d PEs, total CPU time = %f ms, max elapsed = %f ms, ~%u "
                "actors per PE\n", npes, (double)total_time / 1000.0,
                (double)max_elapsed / 1000.0, actors_per_cell);
    }

    return 0;
}

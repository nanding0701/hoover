// Compile with -DHVR_BUCKET_SIZE=5 -DHVR_MAX_CONSTANT_ATTRS=10
#include <iostream>
#include <chrono>
#include <random>
#include <cmath>

#include <shmem.h>
#include <hoover.h>


// Set to a tiny value so patch & agent can find each other iff they are at the same spot
const double connectivity_threshold = 0.01;

// Simulation space is square shaped, this value is not important
const double space_dim = 100.0;
// Put patches in square tiles, one patch per partition (they are used interchangeably in variable names)
const hvr_partition_t partition_dim = 64;
const hvr_partition_t n_partitions = partition_dim * partition_dim;
const double partition_side_len = space_dim / double(partition_dim);

// Agents are distributed evenly across all PEs
const uint64_t total_num_agents = 250000;
// Total number of initially infectious agents, also distributed evenly
const uint64_t total_num_init_Ih_agents = std::ceil(total_num_agents * 0.02);

// Four activities for each patch
// Do not change! Hard coded everywhere!
const uint64_t activities_per_patch = 4;

// Number of days for the simulation
const int n_days = 120;
// Number of agent time steps per day
// Changing this will affect dt_h/dt_v and therefore other parameters
const int agent_steps = 8;
// Number of patch time steps per agent time step
const int patch_steps = 50;

const hvr_time_t max_timestep = agent_steps * n_days;


// ***** We only care about adult female mosquitoes *****
// Natural per capita emergence rate
const double psi_v = 0.3;
// Per capita death rate , 1/mu_v is the average life span
const double mu_v = 1.0 / 14.0;
// Intrinsic growth rate
const double r_v = psi_v - mu_v;
// Carrying capacity of a patch
const double Kv = 2500;
// # of times one mosquito would want to bite per unit time
const double sigma_v = 0.5;
// # of mosquito bites an average agent can sustain per unit time
const double sigma_h = 19;
// Probability of transmission of infection from an infectious mosquito to
// a susceptible agent
const double beta_hv = 0.33;
// Probability of transmission of infection from an infectious agent to a
// susceptible mosquito
const double beta_vh = 0.33;
// Per capita rate of progression of mosquitoes from the exposed state to
// the infectious state, 1/nu_v is the average duration of the latent period
const double nu_v = 0.1;
// Per capita rate of progression of agents from the exposed state to the
// infections state
const double nu_h = 1.0 / 5.0;
// Per capita recovery rate of agents
const double mu_h = 1.0 / 6.0;
// delta_t of the agent time steps
const double dt_h = 1.0 / agent_steps;
// delta_t of the patch time steps
const double dt_v = 1.0 / (agent_steps * patch_steps);

// Probability of an exposed agent becomes infectious in a time step
const double PEtI = 1.0 - std::exp(-dt_h * nu_h);
// Probability of an infectious recovers in a time step
const double PItR = 1.0 - std::exp(-dt_h * mu_h);

// Run time constants that depends on mype and npes
int mype, npes;
uint64_t agents_per_pe, initial_infected_agents_per_pe;
hvr_partition_t patches_per_pe, patches_start, patches_end;

// ID for the two graphs
hvr_graph_id_t patch_graph, agent_graph;


// High-quality RNG
std::random_device rd;
std::mt19937_64 rng(rd());

// RNG with a fixed seed, for free mode v.s. strict mode correctness checkings
// std::mt19937_64 rng(0);


// Adult female mosquito per capita emergence function (???)
// h_v(Nv,t) = (psi_v - r_v * Nv / Kv) * Nv
//           = [psi_v * (1 - Nv / Kv) + mu_v * Nv / Kv] * Nv
double h_v(const double psi_v, const double r_v, const double Nv, const double Kv) {
    return (psi_v - r_v * Nv / Kv) * Nv;
}


// Average force of infection to mosquitoes (rate of infection for each mosquito per unit time)
// Product of: the average number of bites per mosquito
//             the probability of transmission per bite
//             the probability that a bite is on an infectious agent
// ***** This is where the ABM and the ODE "communicates" *****
double lambda_v(const double sigma_v, const double sigma_h, const double beta_hv,
                const double Nv, const double Ihh, const double Nhh) {
    // Number of bites wanted by the mosquitoes
    double demand = sigma_v * Nv;
    // Number of bites could be sustained by the available agents
    double supply = sigma_h * Nhh;
    // # of successful bites
    double b = (demand * supply) / (demand + supply);

    return b / Nv * beta_hv * (Ihh / Nhh);
}


// Rate at which agents are infected from infectious mosquitoes
// Product of: the average number of bites a typical agent gets
//             the probability of transmission per bite
//             the probability that a bite is from an infectious mosquito
double lambda_h(const double sigma_v, const double sigma_h, const double beta_vh,
                const double Nv, const double Iv, const double Nhh) {
    // Number of bites wanted by the mosquitoes
    double demand = sigma_v * Nv;
    // Number of bites could be sustained by the available agents
    double supply = sigma_h * Nhh;
    // # of successful bites
    double b = (demand * supply) / (demand + supply);

    return b / Nhh * beta_vh * (Iv / Nv);
}


// Probability that a susceptible agent in a patch k becomes infected in a time step
double p_StE(const double lambda_h, const double dt_h) {
    return 1.0 - std::exp(-lambda_h * dt_h);
}


// Initialize global constants that depends on npes & mype
void init_constants() {
    mype = shmem_my_pe();
    npes = shmem_n_pes();
    agents_per_pe = std::ceil(double(total_num_agents) / double(npes));
    initial_infected_agents_per_pe = std::ceil(double(total_num_init_Ih_agents) / double(npes));
    patches_per_pe = (n_partitions + npes - 1) / npes;
    patches_start = patches_per_pe * mype;
    patches_end = patches_start + patches_per_pe;
}


// Given the ID of a patch/partition, return the coordinates of its center
void patch_id_to_xy(const hvr_partition_t patch, double& x, double& y) {
    const uint64_t p_row = patch / partition_dim;
    const uint64_t p_col = patch % partition_dim;
    x = (double(p_row) + 0.5) * partition_side_len;
    y = (double(p_col) + 0.5) * partition_side_len;
}


// Given coordinates, return the row & column of the patch/partition it is in
void xy_to_p_row_col(const double x, const double y, uint64_t& p_row, uint64_t& p_col) {
    p_row = std::floor(x / partition_side_len);
    p_col = std::floor(y / partition_side_len);
}


// Given coordinates, return the ID of the patch/partition it is in
hvr_partition_t xy_to_patch_id(const double x, const double y) {
    uint64_t p_row, p_col;
    xy_to_p_row_col(x, y, p_row, p_col);
    return p_row * partition_dim + p_col;
}


// Initialize patch vertices, each patch has 15 attributes
void init_patches(hvr_sparse_vec_t*& patches, hvr_ctx_t ctx) {
    // Constant attributes (10 of them):
    // 0: x coordinate  1: y coordinate
    // 7 ~ 10: maximum lengths of stay of activity 0 ~ 3
    // 11 ~ 14: relative risks of being bitten (alpha) of activity 0 ~ 3
    unsigned const_features[10] = {0, 1, 7, 8, 9, 10, 11, 12, 13, 14};
    double const_vals[10] = {0, 0, 2, 4, 6, 8, 1.0, 0.9, 0.8, 0.7};
    patches = hvr_sparse_vec_create_n_with_const_attrs(patches_per_pe,
                                                       patch_graph,
                                                       const_features,
                                                       const_vals,
                                                       10,
                                                       ctx);

    for (uint64_t i = 0; i < patches_per_pe; i++) {
        double x, y;
        patch_id_to_xy(patches_start + i, x, y);

        (patches + i)->const_values[0] = x;                 // x coordinate
        (patches + i)->const_values[1] = y;                 // y coordinate

        // Dynamic attributes (5 of them):
        // Store Ihh & Nhh for the record?
        hvr_sparse_vec_set(2, 0.0, patches + i, ctx);       // lambda_h
        hvr_sparse_vec_set(3, Kv / 2.0, patches + i, ctx);  // Sv
        hvr_sparse_vec_set(4, 0.0, patches + i, ctx);       // Ev
        hvr_sparse_vec_set(5, 0.0, patches + i, ctx);       // Iv
        hvr_sparse_vec_set(6, Kv / 2.0, patches + i, ctx);  // Nv = Sv + Ev + Iv
    }
}


// Initialize agent vertices, each agent has 7 attributes
void init_agents(hvr_sparse_vec_t*& agents, hvr_ctx_t ctx) {
    // Constant attributes (10 of them):
    // 2: home x coordinate     3: home y coordinate
    unsigned const_features[2] = {2, 3};
    double const_vals[2] = {0, 0};
    agents = hvr_sparse_vec_create_n_with_const_attrs(agents_per_pe,
                                                      agent_graph,
                                                      const_features,
                                                      const_vals,
                                                      2,
                                                      ctx);

    // RNG to pick a random patch that belongs to this PE
    std::uniform_int_distribution<> pch_rng(patches_start, patches_end - 1);
    // RNG to pick a random activity between 0 ~ 3
    std::uniform_int_distribution<> act_rng(0, activities_per_patch - 1);

    for (uint64_t i = 0; i < agents_per_pe; i++) {
        // Generate patch ID & activity ID
        const hvr_partition_t patch_id = pch_rng(rng);
        const uint64_t act_id = act_rng(rng);

        double x, y;
        patch_id_to_xy(patch_id, x, y);

        (agents + i)->const_values[0] = x;            // Home x
        (agents + i)->const_values[1] = y;            // Home y

        // Health condition of an agent
        // 0.0:     susceptible
        // 1.0:     exposed
        // 2.0:     infectious
        // 3.0:     recovered (immune)
        double h = 0.0;

        // Make some of them infectious
        if (i < initial_infected_agents_per_pe)
            h = 2.0;

        // Dynamic attributes (5 of them):
        hvr_sparse_vec_set(0, x, agents + i, ctx);    // Current x
        hvr_sparse_vec_set(1, y, agents + i, ctx);    // Current y
        hvr_sparse_vec_set(4, h, agents + i, ctx);    // Health condition
        hvr_sparse_vec_set(5, double(act_id), agents + i, ctx);  // ID of current activity
        hvr_sparse_vec_set(6, 0.0, agents + i, ctx);  // Time steps stayed at current activity
    }
}


// A (very clumsy) function to find the neighbors of a patch in the square tiles of patches,
// given the row-major patch ID. The neighbors list include the patch itself.
void find_neighbor_patches(const hvr_partition_t p, hvr_partition_t* neighbors, int& n_neighbors) {
    const uint64_t p_row = p / partition_dim;
    const uint64_t p_col = p % partition_dim;

    if (p == 0) {                                       // Upper left corner
        n_neighbors = 4;
        neighbors[0] = p;
        neighbors[1] = p + 1;
        neighbors[2] = p + partition_dim;
        neighbors[3] = p + partition_dim + 1;
    } else if (p == n_partitions - 1) {                 // Lower right corner
        n_neighbors = 4;
        neighbors[0] = p;
        neighbors[1] = p - 1;
        neighbors[2] = p - partition_dim;
        neighbors[3] = p - partition_dim - 1;
    } else if (p == partition_dim - 1) {                // Upper right corner
        n_neighbors = 4;
        neighbors[0] = p - 1;
        neighbors[1] = p;
        neighbors[2] = p + partition_dim - 1;
        neighbors[3] = p + partition_dim;
    } else if (p == n_partitions - partition_dim) {     // Lower left corner
        n_neighbors = 4;
        neighbors[0] = p + 1;
        neighbors[1] = p;
        neighbors[2] = p - partition_dim + 1;
        neighbors[3] = p - partition_dim;
    } else if (p_row == 0) {                            // Upper edge
        n_neighbors = 6;
        neighbors[0] = p - 1;
        neighbors[1] = p;
        neighbors[2] = p + 1;
        neighbors[3] = p + partition_dim - 1;
        neighbors[4] = p + partition_dim;
        neighbors[5] = p + partition_dim + 1;
    } else if (p_col == 0) {                            // Left edge
        n_neighbors = 6;
        neighbors[0] = p - partition_dim;
        neighbors[1] = p - partition_dim + 1;
        neighbors[2] = p;
        neighbors[3] = p + 1;
        neighbors[4] = p + partition_dim;
        neighbors[5] = p + partition_dim + 1;
    } else if (p_row == partition_dim - 1) {            // Lower edge
        n_neighbors = 6;
        neighbors[0] = p - partition_dim - 1;
        neighbors[1] = p - partition_dim;
        neighbors[2] = p - partition_dim + 1;
        neighbors[3] = p - 1;
        neighbors[4] = p;
        neighbors[5] = p + 1;
    } else if (p_col == partition_dim - 1) {            // Right edge
        n_neighbors = 6;
        neighbors[0] = p - partition_dim - 1;
        neighbors[1] = p - partition_dim;
        neighbors[2] = p - 1;
        neighbors[3] = p;
        neighbors[4] = p + partition_dim - 1;
        neighbors[5] = p + partition_dim;
    } else {                                            // Interior
        n_neighbors = 9;
        neighbors[0] = p - partition_dim - 1;
        neighbors[1] = p - partition_dim;
        neighbors[2] = p - partition_dim + 1;
        neighbors[3] = p - 1;
        neighbors[4] = p;
        neighbors[5] = p + 1;
        neighbors[6] = p + partition_dim - 1;
        neighbors[7] = p + partition_dim;
        neighbors[8] = p + partition_dim + 1;
    }
}


// Given the home coordinates of an agent, generate a random destination that is not too far
// away from home, return the new coordinates.
void move_to_new_patch(const double home_x, const double home_y, double& new_x, double& new_y) {
    const hvr_partition_t p = xy_to_patch_id(home_x, home_y);

    // Find neighbors of the home patch
    int n_choices;
    hvr_partition_t choices[9];
    find_neighbor_patches(p, choices, n_choices);

    // Pick one from the neighbors, and return the coordinates
    std::uniform_int_distribution<> choice_rng(0, n_choices - 1);
    const hvr_partition_t choice = choices[choice_rng(rng)];
    assert(choice < n_partitions);
    patch_id_to_xy(choice, new_x, new_y);
}


// Don't need this for now
// void start_timestep(hvr_vertex_iter_t* iter, hvr_ctx_t ctx) {
// }


// Return the current patch/partition ID of an agent
hvr_partition_t actor_to_partition(hvr_sparse_vec_t *actor, hvr_ctx_t ctx) {
    const double x = hvr_sparse_vec_get(0, actor, ctx);
    const double y = hvr_sparse_vec_get(1, actor, ctx);
    return xy_to_patch_id(x, y);
}


void update_metadata(hvr_sparse_vec_t *vec,
                     hvr_sparse_vec_t *neighbors,
                     const size_t n_neighbors,
                     hvr_set_t *couple_with,
                     hvr_ctx_t ctx) {
    // Only useful for printing debug info about # of neighbors
    // const double x = hvr_sparse_vec_get(0, vec, ctx);
    // const double y = hvr_sparse_vec_get(1, vec, ctx);

    if (vec->graph == patch_graph) {    // This is a patch
        // Calculate the total # of agents, and how many of them are infectious
        // for each activitiy in this patch
        uint64_t Nh[activities_per_patch] = {};
        uint64_t Ih[activities_per_patch] = {};

        for (size_t i = 0; i < n_neighbors; i++) {
            const double h = hvr_sparse_vec_get(4, neighbors + i, ctx);
            const uint64_t act = hvr_sparse_vec_get(5, neighbors + i, ctx);
            Nh[act] += 1;

            if (h < 2.5 && h > 1.5)     // Infectious!
                Ih[act] += 1;
        }

        // Get mosquito population data from the last time step
        double Sv = hvr_sparse_vec_get(3, vec, ctx);
        double Ev = hvr_sparse_vec_get(4, vec, ctx);
        double Iv = hvr_sparse_vec_get(5, vec, ctx);
        double Nv = hvr_sparse_vec_get(6, vec, ctx);

        // Get relative risks of being bitten
        const double alpha_0 = hvr_sparse_vec_get(11, vec, ctx);
        const double alpha_1 = hvr_sparse_vec_get(12, vec, ctx);
        const double alpha_2 = hvr_sparse_vec_get(13, vec, ctx);
        const double alpha_3 = hvr_sparse_vec_get(14, vec, ctx);

        // Calculate effective numbers of agents
        const double Nhh = Nh[0] * alpha_0 + Nh[1] * alpha_1 + Nh[2] * alpha_2 + Nh[3] * alpha_3;
        const double Ihh = Ih[0] * alpha_0 + Ih[1] * alpha_1 + Ih[2] * alpha_2 + Ih[3] * alpha_3;

        // Compute the mosquitoes' life cycles: ODE solving with RK4
        // Maybe I should compute lambda_v & h_v only once?
        for (int ts_v = 0; ts_v < patch_steps; ts_v++) {
            double Sv_k1 = dt_v * (h_v(psi_v, r_v, Nv, Kv) - lambda_v(sigma_v, sigma_h, beta_hv, Nv, Ihh, Nhh) * Sv - mu_v * Sv);
            double Sv_k2 = dt_v * (h_v(psi_v, r_v, Nv, Kv) - lambda_v(sigma_v, sigma_h, beta_hv, Nv, Ihh, Nhh) * (Sv + Sv_k1/2) - mu_v * (Sv + Sv_k1/2));
            double Sv_k3 = dt_v * (h_v(psi_v, r_v, Nv, Kv) - lambda_v(sigma_v, sigma_h, beta_hv, Nv, Ihh, Nhh) * (Sv + Sv_k2/2) - mu_v * (Sv + Sv_k2/2));
            double Sv_k4 = dt_v * (h_v(psi_v, r_v, Nv, Kv) - lambda_v(sigma_v, sigma_h, beta_hv, Nv, Ihh, Nhh) * (Sv + Sv_k3) - mu_v * (Sv + Sv_k3));
            Sv += (Sv_k1 + 2 * (Sv_k2 + Sv_k3) + Sv_k4) / 6;

            double Ev_k1 = dt_v * (lambda_v(sigma_v, sigma_h, beta_hv, Nv, Ihh, Nhh) * Sv - nu_v * Ev - mu_v * Ev);
            double Ev_k2 = dt_v * (lambda_v(sigma_v, sigma_h, beta_hv, Nv, Ihh, Nhh) * (Sv + Sv_k1/2) - nu_v * (Ev + Ev_k1/2) - mu_v * (Ev + Ev_k1/2));
            double Ev_k3 = dt_v * (lambda_v(sigma_v, sigma_h, beta_hv, Nv, Ihh, Nhh) * (Sv + Sv_k2/2) - nu_v * (Ev + Ev_k2/2) - mu_v * (Ev + Ev_k2/2));
            double Ev_k4 = dt_v * (lambda_v(sigma_v, sigma_h, beta_hv, Nv, Ihh, Nhh) * (Sv + Sv_k3) - nu_v * (Ev + Ev_k3) - mu_v * (Ev + Ev_k3));
            Ev += (Ev_k1 + 2 * (Ev_k2 + Ev_k3) + Ev_k4) / 6;

            double Iv_k1 = dt_v * (nu_v * Ev - mu_v * Iv);
            double Iv_k2 = dt_v * (nu_v * (Ev + Ev_k1 / 2) - mu_v * (Iv + Iv_k1 / 2));
            double Iv_k3 = dt_v * (nu_v * (Ev + Ev_k2 / 2) - mu_v * (Iv + Iv_k2 / 2));
            double Iv_k4 = dt_v * (nu_v * (Ev + Ev_k3) - mu_v * (Iv + Iv_k3));
            Iv += (Iv_k1 + 2 * (Iv_k2 + Iv_k3) + Iv_k4) / 6;

            // Total number of mosquitoes
            Nv = Sv + Ev + Iv;
        }

        // Compute mosquito-to-host force of infection lambda_h
        const double lh = lambda_h(sigma_v, sigma_h, beta_vh, Nv, Iv, Nhh);

        hvr_sparse_vec_set(2, lh, vec, ctx);
        hvr_sparse_vec_set(3, Sv, vec, ctx);
        hvr_sparse_vec_set(4, Ev, vec, ctx);
        hvr_sparse_vec_set(5, Iv, vec, ctx);
        hvr_sparse_vec_set(6, Nv, vec, ctx);

        // Print debug info about the number of neighbors(agents) of this patch
        // std::cerr << "\033[32mLOG: In timestep " << ctx->timestep
        //           << ", patch " << xy_to_patch_id(x, y)
        //           << " from PE " << VERTEX_ID_PE(vec->id)
        //           << " has " << n_neighbors << " neighbors.\033[0m\n";
    }

    if (vec->graph == agent_graph) {    // This is an agent
        hvr_sparse_vec_t* patch = nullptr;

        // Find the patch
        for (size_t i = 0; i < n_neighbors; i++) {
            if ((neighbors + i)->graph == patch_graph) {
                patch = neighbors + i;
                break;
            }
        }

        if (patch == nullptr) {
            // Cannot find a patch!
            // We ignore this for now, pretending the agent is stuck in traffic
            // std::cerr << "\033[35mERROR: In timestep " << ctx->timestep
            //           << ", agent " << VERTEX_ID_OFFSET(vec->id)
            //           << " from PE " << VERTEX_ID_PE(vec->id)
            //           << " could not find patch " << xy_to_patch_id(x, y)
            //           << " from its " << n_neighbors << " neighbors.\033[0m\n";
            // abort();
        } else {
            // Update health status
            const double health = hvr_sparse_vec_get(4, vec, ctx);
            const int act_id = hvr_sparse_vec_get(5, vec, ctx);
            const double alpha = hvr_sparse_vec_get(act_id + 11, patch, ctx);

            // Generate a random number for health status transition
            std::uniform_real_distribution<> h_trans(0.0, 1.0);
            const double p = h_trans(rng);

            if (health < 0.5) {         // susceptible
                // Get lambda_h
                const double lh = hvr_sparse_vec_get(2, patch, ctx);
                // Calculate the probability of susceptible-to-exposed
                const double PStE = p_StE(alpha * lh, dt_h);
                if (p < PStE)           // Infect this agent
                    hvr_sparse_vec_set(4, 1.0, vec, ctx);
            } else if (health < 1.5) {  // Exposed
                if (p < PEtI)           // Become infectious
                    hvr_sparse_vec_set(4, 2.0, vec, ctx);
            } else if (health < 2.5) {  // Infectious
                if (p < PItR)           // Recover from the disease
                    hvr_sparse_vec_set(4, 3.0, vec, ctx);
            } else {
                // Immune
            }

            // Now decide if the agent should move to a new location
            const double stayed = hvr_sparse_vec_get(6, vec, ctx);
            const double max_stay = hvr_sparse_vec_get(act_id + 7, patch, ctx);

            if (stayed >= max_stay - 0.001) {
                const double home_x = hvr_sparse_vec_get(2, vec, ctx);
                const double home_y = hvr_sparse_vec_get(3, vec, ctx);
                double new_x, new_y;
                move_to_new_patch(home_x, home_y, new_x, new_y);
                hvr_sparse_vec_set(0, new_x, vec, ctx);
                hvr_sparse_vec_set(1, new_y, vec, ctx);

                // Moved to a new patch, now generate a random activity ID
                std::uniform_int_distribution<> act_rng(0, activities_per_patch - 1);
                hvr_sparse_vec_set(5, double(act_rng(rng)), vec, ctx);

                // Clear the # of time steps stayed at current activity
                hvr_sparse_vec_set(6, 0.0, vec, ctx);
            } else {
                // Increment the # of time steps stayed at current activity
                hvr_sparse_vec_set(6, stayed + 1.0, vec, ctx);
            }
        }

    }
}


// We only interact with neighboring patches/partitions
int might_interact(const hvr_partition_t partition,
                   hvr_set_t *partitions,
                   hvr_partition_t *interacting_partitions,
                   unsigned *n_interacting_partitions,
                   const unsigned interacting_partitions_capacity,
                   hvr_ctx_t ctx) {
    // Get neighbors
    int n_neighbor_patches;
    hvr_partition_t neighbor_patches[9];
    find_neighbor_patches(partition, neighbor_patches, n_neighbor_patches);

    // Are those neighbors in the provided list of partitions?
    unsigned count_interacting_partitions = 0;
    for (int i = 0; i < n_neighbor_patches; i++) {
        const hvr_partition_t p = neighbor_patches[i];
        if (hvr_set_contains(p, partitions)) {
            interacting_partitions[count_interacting_partitions++] = p;
        }
    }

    if (count_interacting_partitions > interacting_partitions_capacity) {
        std::cerr << "\033[35mERROR: Too many interacting partitions!\033[0m\n";
        abort();
    }

    *n_interacting_partitions = count_interacting_partitions;
    return count_interacting_partitions > 0;
}


// We do some statistics here, and we do not abort
int check_abort(hvr_vertex_iter_t *iter,
                hvr_ctx_t ctx,
                hvr_set_t *to_couple_with,
                hvr_sparse_vec_t *out_coupled_metric) {
    // Just so Hoover doesn't crash
    hvr_sparse_vec_set(0, 0.0, out_coupled_metric, ctx);

    uint64_t Sh = 0.0;
    uint64_t Eh = 0.0;
    uint64_t Ih = 0.0;
    uint64_t Rh = 0.0;

    // Count the number of each type of the agents
    hvr_sparse_vec_t* v = hvr_vertex_iter_next(iter);
    while (v) {
        if (v->graph == agent_graph) {
            const double health = hvr_sparse_vec_get(4, v, ctx);
            if (health < 0.5) {
                Sh += 1;
            } else if (health < 1.5) {
                Eh += 1;
            } else if (health < 2.5) {
                Ih += 1;
            } else {
                Rh += 1;
            }
        }
        v = hvr_vertex_iter_next(iter);
    }

    // Print agent statistics in CSV format
    std::cout << ctx->timestep << ", " << ctx->pe
              << ", " << Sh << ", " << Eh
              << ", " << Ih << ", " << Rh << '\n';

    // All agents of this PE have recovered
    // Do not abort here! The patches need to be working
    // if (Rh == agents_per_pe)
    //     std::cerr << "Time step " << ctx->timestep
    //               << ": All agents of PE " << ctx->pe << " are healed!\n";

    return 0;
}


int main() {
    shmem_init();

    // Initialize global constants
    init_constants();

    hvr_ctx_t ctx;
    hvr_ctx_create(&ctx);

    agent_graph = hvr_graph_create(ctx);
    patch_graph = hvr_graph_create(ctx);
    hvr_graph_id_t graphs[2] = {agent_graph, patch_graph};

    hvr_sparse_vec_t *patches, *agents;

    init_patches(patches, ctx);

    init_agents(agents, ctx);

    hvr_init(n_partitions, update_metadata, might_interact, check_abort, actor_to_partition,
             NULL, graphs, 2, connectivity_threshold, 0, 1, max_timestep, ctx);

    shmem_barrier_all();

    auto t_start = std::chrono::steady_clock::now();

    hvr_body(ctx);

    shmem_barrier_all();

    auto t_end = std::chrono::steady_clock::now();

    hvr_finalize(ctx);

    shmem_finalize();

    // Print timing info
    if (mype == 0)
        std::cout << "Time elapsed: "
                  << std::chrono::duration<double>(t_end - t_start).count()
                  << " seconds\n";
}
#include "hvr_map.h"
#include "hoover.h"

#define N_VERTICES 1000000
#define N_REPEATS 50

int main(int argc, char **argv) {
    hvr_map_t m;
    hvr_map_init(&m,
            100000,/* prealloc segs */
            "DUMMY" /* prealloc segs env var */
            );

    unsigned long long start = hvr_current_time_us();
    for (int r = 0; r < N_REPEATS; r++) {
        for (unsigned i = 0; i < N_VERTICES - N_REPEATS; i++) {
            unsigned neighbor = i + r;

            hvr_map_add(i, (void*)neighbor, 1, &m);
        }
    }
    unsigned long long elapsed = hvr_current_time_us() - start;
    printf("# vertices = %u, # repeats = %u, took %f ms\n", N_VERTICES,
            N_REPEATS, (double)elapsed / 1000.0);

    start = hvr_current_time_us();
    for (int r = 0; r < N_REPEATS; r++) {
        for (unsigned i = 0; i < N_VERTICES - N_REPEATS; i++) {
            hvr_map_get(i, &m);
        }
    }
    elapsed = hvr_current_time_us() - start;
    printf("# vertices = %u, # repeats = %u, took %f ms\n", N_VERTICES,
            N_REPEATS, (double)elapsed / 1000.0);


    return 0;
}

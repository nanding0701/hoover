#include "hvr_buffered_msgs.h"

void hvr_buffered_msgs_init(size_t nvertices, size_t pool_size,
        hvr_buffered_msgs_t *b) {
    b->buffered = (hvr_buffered_msgs_node_t **)malloc_helper(
            nvertices * sizeof(b->buffered[0]));
    assert(b->buffered);
    memset(b->buffered, 0x00, nvertices * sizeof(b->buffered[0]));
    b->nvertices = nvertices;

    b->pool = malloc_helper(pool_size);
    assert(b->pool);
    memset(b->pool, 0x00, pool_size);
    b->pool_size = pool_size;
    b->allocator = create_mspace_with_base(b->pool, pool_size, 0);
    assert(b->allocator);
}

void hvr_buffered_msgs_insert(size_t i, hvr_vertex_t *payload,
        hvr_buffered_msgs_t *b) {
    assert(i < b->nvertices);

    hvr_buffered_msgs_node_t *node =
        (hvr_buffered_msgs_node_t *)mspace_malloc(b->allocator, sizeof(*node));
    assert(node);

    memcpy(&node->vert, payload, sizeof(*payload));

    node->next = b->buffered[i];
    b->buffered[i] = node;
}

int hvr_buffered_msgs_poll(size_t i, hvr_vertex_t *out,
        hvr_buffered_msgs_t *b) {
    assert(i < b->nvertices);

    hvr_buffered_msgs_node_t *head = b->buffered[i];
    if (head) {
        memcpy(out, &(head->vert), sizeof(*out));
        b->buffered[i] = head->next;

        mspace_free(b->allocator, head);
        return 1;
    } else {
        return 0;
    }
}

size_t hvr_buffered_msgs_mem_used(hvr_buffered_msgs_t *b) {
    return b->nvertices * sizeof(b->buffered[0]) + b->pool_size;
}

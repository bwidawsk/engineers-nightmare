#include <algorithm>
#include <glm/glm.hpp>

#include "../memory.h"
#include "relative_position.h"


struct component_member {
    unsigned align, size;
};

static void
resize_pool(unsigned count,
            component_manager::component_buffer *buffer,
            void ** old_pool, void ** new_pool,
            unsigned member_count, component_member const *members) {

    component_manager::component_buffer new_buffer;

    size_t size = 0;
    unsigned worst_misalign = 0;

    for (auto i = 0u; i < member_count; i++) {
        worst_misalign = std::max(members[i].align, worst_misalign);
        size = align_size(size, members[i].align) + members[i].size * count;
    }

    size += worst_misalign;

    new_buffer.buffer = malloc(size);
    new_buffer.num = buffer->num;
    new_buffer.allocated = count;
    memset(new_buffer.buffer, 0, size);

    size_t off = (size_t)new_buffer.buffer;
    for (auto i = 0u; i < member_count; i++) {
        off = align_size(off, members[i].align);
        new_pool[i] = (void *)off;
        off += members[i].size * count;
    }

    if (buffer->allocated) {
        for (auto i = 0u; i < member_count; i++) {
            memcpy(new_pool[i], old_pool[i], buffer->num * members[i].size);
        }
        free(buffer->buffer);
    }

    *buffer = new_buffer;
}


#define ARRAY_SIZE(x)   (sizeof(x)/sizeof(*x))
#define DECLARE_MEMBER(x)       { alignof(x), sizeof(x) }


component_member relative_position_desc[] = {
    DECLARE_MEMBER(c_entity),
    DECLARE_MEMBER(glm::vec3),
    DECLARE_MEMBER(glm::mat4)
};


void
relative_position_component_manager::create_component_instance_data(unsigned count) {
    if (count <= buffer.allocated)
        return;

    relative_position_instance_data new_pool;

    resize_pool(count, &buffer, (void **)&instance_pool, (void **)&new_pool,
                ARRAY_SIZE(relative_position_desc),
                relative_position_desc);

    instance_pool = new_pool;
}

void
relative_position_component_manager::destroy_instance(instance i) {
    auto last_index = buffer.num - 1;
    auto last_entity = instance_pool.entity[last_index];
    auto current_entity = instance_pool.entity[i.index];

    instance_pool.entity[i.index] = instance_pool.entity[last_index];
    instance_pool.position[i.index] = instance_pool.position[last_index];
    instance_pool.mat[i.index] = instance_pool.mat[last_index];

    entity_instance_map[last_entity] = i.index;
    entity_instance_map.erase(current_entity);

    --buffer.num;
}

void
relative_position_component_manager::entity(c_entity const &e) {
    if (buffer.num >= buffer.allocated) {
        printf("Increasing size of relative_position buffer. Please adjust\n");
        create_component_instance_data(std::max(1u, buffer.allocated) * 2);
    }

    auto inst = lookup(e);

    instance_pool.entity[inst.index] = e;
}
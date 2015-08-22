#pragma once

#include "component_manager.h"

// toggle block component
// toggles -- see notes
// unsigned char (4 bits used)

// when placed, checks the four adjacent slots around it (on its plane)
// if any are empty, marks those to be toggled.
// on use, toggles those blocks into and out of existence
// note, this is potentially destructive

struct toggle_block_component_manager : component_manager {
    struct light_instance_data {
        c_entity *entity;
        unsigned char *toggles;
    } instance_pool;

    void create_component_instance_data(unsigned count) override;

    void destroy_instance(instance i) override;

    void entity(c_entity const &e) override;

    unsigned char &toggles(c_entity const &e) {
        auto inst = lookup(e);

        return instance_pool.toggles[inst.index];
    }
};

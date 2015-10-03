#pragma once

/* THIS FILE IS AUTOGENERATED BY gen/gen_component_impl.py; DO NOT HAND-MODIFY */

#include "component_manager.h"

struct pressure_sensor_component_manager : component_manager {
    struct instance_data {
        c_entity *entity;
        float *pressure;
        unsigned *type;
    } instance_pool;

    void create_component_instance_data(unsigned count) override;
    void destroy_instance(instance i) override;
    void entity(c_entity e) override;

    float & pressure(c_entity e) {
        auto inst = lookup(e);
        return instance_pool.pressure[inst.index];
    }

    unsigned & type(c_entity e) {
        auto inst = lookup(e);
        return instance_pool.type[inst.index];
    }

    instance_data get_instance_data(c_entity e) {
        instance_data d;
        auto inst = lookup(e);

        d.entity = instance_pool.entity + inst.index;
        d.pressure = instance_pool.pressure + inst.index;
        d.type = instance_pool.type + inst.index;

        return d;
    }
};

#pragma once
#include <btBulletDynamicsCommon.h>

#include "component/component_manager.h"
#include "mesh.h"

struct entity_type
{
    sw_mesh *sw;
    hw_mesh *hw;
    char const *name;
    btTriangleMesh *phys_mesh;
    btCollisionShape *phys_shape;
};

struct entity
{
    /* TODO: replace this completely, it's silly. */
    int x, y, z;
    entity_type *type;
    btRigidBody *phys_body;
    int face;
    c_entity ce;

    entity(int x, int y, int z, entity_type *type, int face);
    void use();
    virtual ~entity();
};
#ifndef _WIN32
#include <err.h> /* errx */
#else
#include "src/winerr.h"
#endif

#include <algorithm>
#include <epoxy/gl.h>
#include <functional>
#include <glm/glm.hpp>
#include <stdio.h>
#include <SDL.h>
#include <unordered_map>

#include "src/common.h"
#include "src/component/component_system_manager.h"
#include "src/config.h"
#include "src/input.h"
#include "src/light_field.h"
#include "src/mesh.h"
#include "src/physics.h"
#include "src/player.h"
#include "src/projectile/projectile.h"
#include "src/particle.h"
#include "src/render_data.h"
#include "src/scopetimer.h"
#include "src/shader.h"
#include "src/ship_space.h"
#include "src/text.h"
#include "src/textureset.h"
#include "src/tools/tools.h"
#include "src/wiring/wiring.h"
#include "src/wiring/wiring_data.h"


#define APP_NAME        "Engineer's Nightmare"
#define DEFAULT_WIDTH   1024
#define DEFAULT_HEIGHT  768


#define WORLD_TEXTURE_DIMENSION     32
#define MAX_WORLD_TEXTURES          64

#define MOUSE_Y_LIMIT      1.54f
#define MAX_AXIS_PER_EVENT 128

#define INITIAL_MAX_COMPONENTS 20

bool exit_requested = false;

bool draw_hud = true;
bool draw_debug_text = false;
bool draw_fps = false;

auto hfov = DEG2RAD(90.f);

en_settings game_settings;

struct {
    SDL_Window *ptr;
    SDL_GLContext gl_ctx;
    int width;
    int height;
    bool has_focus;
} wnd;

struct {
    Timer timer;

    const float fps_duration = 0.25f;

    unsigned int frame = 0;

    unsigned int fps_frame = 0;
    float fps_time = 0.f;

    float dt = 0.f;
    float fps = 0.f;

    void tick() {
        auto t = timer.touch();

        dt = (float) t.delta;   /* narrowing */
        frame++;

        fps_frame++;
        fps_time += dt;

        if (fps_time >= fps_duration) {
            fps = 1 / (fps_time / fps_frame);
            fps_time = 0.f;
            fps_frame = 0;
        }
    }
} frame_info;

struct per_camera_params {
    glm::mat4 view_proj_matrix;
    glm::mat4 inv_centered_view_proj_matrix;
    float aspect;
};

void GLAPIENTRY
gl_debug_callback(GLenum source __unused,
                  GLenum type __unused,
                  GLenum id __unused,
                  GLenum severity __unused,
                  GLsizei length __unused,
                  GLchar const *message,
                  void const *userParam __unused)
{
    printf("GL: %s\n", message);
}

frame_data *frames, *frame;
unsigned frame_index;

sw_mesh *scaffold_sw;
sw_mesh *surfs_sw[6];
GLuint simple_shader, unlit_shader, add_overlay_shader, remove_overlay_shader, ui_shader, ui_sprites_shader;
GLuint sky_shader, unlit_instanced_shader, lit_instanced_shader, particle_shader, modelspace_uv_shader;
texture_set *world_textures;
texture_set *skybox;
ship_space *ship;
player pl;
physics *phy;
unsigned char const *keys;
unsigned int mouse_buttons[input_mouse_buttons_count];
int mouse_axes[input_mouse_axes_count];
hw_mesh *scaffold_hw;
hw_mesh *surfs_hw[6];
text_renderer *text;
sprite_renderer *ui_sprites;
light_field *light;

sw_mesh *door_sw;
hw_mesh *door_hw;

extern hw_mesh *projectile_hw;
extern sw_mesh *projectile_sw;

extern hw_mesh *attachment_hw;
extern sw_mesh *attachment_sw;

extern hw_mesh *no_placement_hw;
extern sw_mesh *no_placement_sw;

extern hw_mesh *wire_hw_meshes[num_wire_types];

sprite_metrics unlit_ui_slot_sprite, lit_ui_slot_sprite;

projectile_linear_manager proj_man;
particle_manager *particle_man;

glm::mat4
mat_block_face(glm::ivec3 p, int face)
{
    static glm::vec3 offsets[] = {
        glm::vec3(1, 0, 0),
        glm::vec3(0, 0, 1),
        glm::vec3(0, 1, 0),
        glm::vec3(0, 0, 1),
        glm::vec3(0, 1, 1),
        glm::vec3(0, 0, 0)
    };

    auto tr = glm::translate(glm::mat4(1), (glm::vec3)p + offsets[face]);

    switch (face) {
    case surface_zp:
        return glm::rotate(tr, (float)M_PI, glm::vec3(1.0f, 0.0f, 0.0f));
    case surface_zm:
        return tr;
    case surface_xp:
        return glm::rotate(tr, -(float)M_PI/2, glm::vec3(0.0f, 1.0f, 0.0f));
    case surface_xm:
        return glm::rotate(tr, (float)M_PI/2, glm::vec3(0.0f, 1.0f, 0.0f));
    case surface_yp:
        return glm::rotate(tr, (float)M_PI/2, glm::vec3(1.0f, 0.0f, 0.0f));
    case surface_ym:
        return glm::rotate(tr, -(float)M_PI/2, glm::vec3(1.0f, 0.0f, 0.0f));

    default:
        return glm::mat4(1);    /* unreachable */
    }
}


struct entity_type
{
    /* static */
    char const *name;
    char const *mesh;
    int material;
    bool placed_on_surface;
    int height;

    /* loader loop does these */
    sw_mesh *sw;
    hw_mesh *hw;
    btTriangleMesh *phys_mesh;
    btCollisionShape *phys_shape;
};


entity_type entity_types[] = {
    { "Door", "mesh/single_door_frame.dae", 2, false, 2 },
    { "Frobnicator", "mesh/frobnicator.dae", 3, false, 1 },
    { "Light", "mesh/panel_4x4.dae", 8, true, 1 },
    { "Warning Light", "mesh/warning_light.dae", 8, true, 1 },
    { "Display Panel", "mesh/panel_4x4.dae", 7, true, 1 },
    { "Switch", "mesh/panel_1x1.dae", 9, true, 1 },
    { "Plaidnicator", "mesh/frobnicator.dae", 13, false, 1 },
    { "Pressure Sensor 1", "mesh/panel_1x1.dae", 12, true, 1 },
    { "Pressure Sensor 2", "mesh/panel_1x1.dae", 14, true, 1 },
    { "Sensor Comparator", "mesh/panel_1x1.dae", 13, true, 1 },
    { "Proximity Sensor", "mesh/panel_1x1.dae", 3, true, 1 },
    { "Flashlight", "mesh/no_place.dae", 3, true, 1 },
};


struct entity
{
    /* TODO: replace this completely, it's silly. */
    c_entity ce;

    entity(glm::ivec3 p, unsigned type, int face) {
        ce = c_entity::spawn();

        auto mat = mat_block_face(p, face);

        auto et = &entity_types[type];

        type_man.assign_entity(ce);
        auto type_comp = type_man.get_instance_data(ce);
        *type_comp.type = type;

        physics_man.assign_entity(ce);
        auto physics = physics_man.get_instance_data(ce);
        *physics.rigid = nullptr;
        build_static_physics_rb_mat(&mat, et->phys_shape, physics.rigid);
        /* so that we can get back to the entity from a phys raycast */
        /* TODO: change this for getting rid of entity* and for supporting submesh */
        (*physics.rigid)->setUserPointer(this);

        surface_man.assign_entity(ce);
        auto surface = surface_man.get_instance_data(ce);
        *surface.block = p;
        *surface.face = face;

        pos_man.assign_entity(ce);
        auto pos = pos_man.get_instance_data(ce);
        *pos.position = p;
        *pos.mat = mat;

        render_man.assign_entity(ce);
        auto render = render_man.get_instance_data(ce);
        *render.mesh = et->hw;

        // door
        if (type == 0) {
            power_man.assign_entity(ce);
            auto power = power_man.get_instance_data(ce);
            *power.powered = false;
            *power.required_power = 8;
            *power.max_required_power = 8;

            door_man.assign_entity(ce);
            auto door = door_man.get_instance_data(ce);
            *door.mesh = door_hw;
            *door.pos = 1.0f;
            *door.desired_pos = 1.0f;

            reader_man.assign_entity(ce);
            auto reader = reader_man.get_instance_data(ce);
            *reader.name = "desired state";
            reader.source->id = 0;
            *reader.desc = nullptr;
            *reader.data = 1.0f;
        }
        // frobnicator
        else if (type == 1) {
            power_man.assign_entity(ce);
            auto power = power_man.get_instance_data(ce);
            *power.powered = false;
            *power.required_power = 12;
            *power.max_required_power = 12;

            gas_man.assign_entity(ce);
            auto gas = gas_man.get_instance_data(ce);
            *gas.flow_rate = 0.1f;
            *gas.max_pressure = 1.0f;
            *gas.enabled = true;
        }
        // light
        else if (type == 2) {
            power_man.assign_entity(ce);
            auto power = power_man.get_instance_data(ce);
            *power.powered = false;
            *power.required_power = 6;
            *power.max_required_power = 6;

            light_man.assign_entity(ce);
            auto light = light_man.get_instance_data(ce);
            *light.intensity = 1.0f;
            *light.requested_intensity = 1.0f;

            reader_man.assign_entity(ce);
            auto reader = reader_man.get_instance_data(ce);
            *reader.name = "light brightness";
            reader.source->id = 0;
            *reader.desc = nullptr;
            *reader.data = 1.0f;
        }
        // warning light
        else if (type == 3) {
            power_man.assign_entity(ce);
            auto power = power_man.get_instance_data(ce);
            *power.powered = false;
            *power.required_power = 6;
            *power.max_required_power = 6;

            light_man.assign_entity(ce);
            auto light = light_man.get_instance_data(ce);
            *light.intensity = 1.0f;
            *light.requested_intensity = 1.0f;

            reader_man.assign_entity(ce);
            auto reader = reader_man.get_instance_data(ce);
            *reader.name = "light brightness";
            reader.source->id = 0;
            *reader.desc = comms_msg_type_sensor_comparison_state;      // temp until we have discriminator tool
            *reader.data = 1.0f;
        }
        // display panel
        else if (type == 4) {
            power_man.assign_entity(ce);
            auto power = power_man.get_instance_data(ce);
            *power.powered = false;
            *power.required_power = 4;
            *power.max_required_power = 4;

            light_man.assign_entity(ce);
            auto light = light_man.get_instance_data(ce);
            *light.intensity = 0.15f;
            *light.requested_intensity = 0.15f;

            reader_man.assign_entity(ce);
            auto reader = reader_man.get_instance_data(ce);
            *reader.name = "light brightness";
            reader.source->id = 0;
            *reader.desc = nullptr;
            *reader.data = 0.15f;
        }
        // switch
        else if (type == 5) {
            switch_man.assign_entity(ce);
            auto sw = switch_man.get_instance_data(ce);
            *sw.enabled = true;
        }
        // plaidnicator
        else if (type == 6) {
            power_provider_man.assign_entity(ce);
            auto power_provider = power_provider_man.get_instance_data(ce);
            *power_provider.max_provided = 12;
            *power_provider.provided = 12;
        }
        // pressure sensor 1
        else if (type == 7) {
            pressure_man.assign_entity(ce);
            auto pressure = pressure_man.get_instance_data(ce);
            *pressure.pressure = 0.0f;
            *pressure.type = 1;
        }
        // pressure sensor 2
        else if (type == 8) {
            pressure_man.assign_entity(ce);
            auto pressure = pressure_man.get_instance_data(ce);
            *pressure.pressure = 0.0f;
            *pressure.type = 2;
        }
        // sensor comparator
        else if (type == 9) {
            comparator_man.assign_entity(ce);
            auto comparator = comparator_man.get_instance_data(ce);
            *comparator.compare_epsilon = 0.0001f;
        }
        // proximity sensor
        else if (type == 10) {
            power_man.assign_entity(ce);
            auto power = power_man.get_instance_data(ce);
            *power.powered = false;
            *power.required_power = 1;
            *power.max_required_power = 1;

            proximity_man.assign_entity(ce);
            auto proximity_sensor = proximity_man.get_instance_data(ce);
            *proximity_sensor.range = 5;
            *proximity_sensor.is_detected = false;
        }
        // flashlight
        else if (type == 11) {
            power_man.assign_entity(ce);
            auto power = power_man.get_instance_data(ce);
            *power.powered = false; /* Flashlight starts off */
            *power.required_power = 0;
            *power.max_required_power = 0;

            light_man.assign_entity(ce);
            auto light = light_man.get_instance_data(ce);
            *light.intensity = 1.0f;
            *light.requested_intensity = 1.0f;
            *light.type = 3;
        }
    }
};


void
use_action_on_entity(ship_space *ship, c_entity ce) {
    /* used by the player */
    assert(pos_man.exists(ce) || !"All [usable] entities probably need position");

    auto pos = *pos_man.get_instance_data(ce).position;
    auto type = &entity_types[*type_man.get_instance_data(ce).type];
    printf("player using the %s at %f %f %f\n",
            type->name, pos.x, pos.y, pos.z);

    if (switch_man.exists(ce)) {
        /* publish new state on all attached comms wires */
        auto & enabled = *switch_man.get_instance_data(ce).enabled;
        enabled ^= true;

        comms_msg msg;
        msg.originator = ce;
        msg.desc = comms_msg_type_switch_state;
        msg.data = enabled ? 1.f : 0.f;
        publish_msg(ship, ce, msg);
    }
}

/* todo: support free-placed entities*/
void
place_entity_attaches(raycast_info* rc, int index, entity* e, unsigned entity_type) {
    auto const & et = entity_types[entity_type];

    for (auto wire_index = 0; wire_index < num_wire_types; ++wire_index) {
        auto wt = (wire_type)wire_index;
        for (auto i = 0u; i < et.sw->num_attach_points[wt]; ++i) {
            auto mat = mat_block_face(rc->p, index ^ 1) * et.sw->attach_points[wt][i];
            wire_attachment wa = { mat, (unsigned)ship->wire_attachments[wt].size() };
            auto attach_index = (unsigned)ship->wire_attachments[wt].size();
            ship->wire_attachments[wt].push_back(wa);
            ship->entity_to_attach_lookups[wt][e->ce].insert(attach_index);
        }
    }
}


void
set_light_level(int x, int y, int z, int level)
{
    if (x < 0 || x >= 128) return;
    if (y < 0 || y >= 128) return;
    if (z < 0 || z >= 128) return;

    int p = x + y * 128 + z * 128 * 128;
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    light->data[p] = level;
}


unsigned char
get_light_level(int x, int y, int z)
{
    if (x < 0 || x >= 128) return 0;
    if (y < 0 || y >= 128) return 0;
    if (z < 0 || z >= 128) return 0;

    return light->data[x + y*128 + z*128*128];
}


const int light_atten = 50;
/* as far as we can ever light from a light source */
const int max_light_prop = (255 + light_atten - 1) / light_atten;

bool need_lightfield_update = false;
glm::ivec3 lightfield_update_mins;
glm::ivec3 lightfield_update_maxs;


void
mark_lightfield_update(glm::ivec3 center)
{
    glm::ivec3 half_extent = glm::ivec3(max_light_prop, max_light_prop, max_light_prop);
    if (need_lightfield_update) {
        lightfield_update_mins = center - half_extent;
        lightfield_update_maxs = center + half_extent;
    }
    else {
        lightfield_update_mins = glm::min(lightfield_update_mins,
                center - half_extent);
        lightfield_update_maxs = glm::max(lightfield_update_maxs,
                center + half_extent);
        need_lightfield_update = true;
    }
}


void
update_lightfield()
{
    if (!need_lightfield_update) {
        /* nothing to do here */
        return;
    }

    /* TODO: opt for case where we're JUST adding light -- no need to clear & rebuild */
    /* This is general enough to cope with occluders & lights being added and removed. */

    /* 1. remove all existing light in the box */
    for (int k = lightfield_update_mins.z; k <= lightfield_update_maxs.z; k++)
        for (int j = lightfield_update_mins.y; j <= lightfield_update_maxs.y; j++)
            for (int i = lightfield_update_mins.x; i <= lightfield_update_maxs.x; i++)
                set_light_level(i, j, k, 0);

    /* 2. inject sources. the box is guaranteed to be big enough for max propagation
     * for all sources we'll add here. */
    for (auto i = 0u; i < light_man.buffer.num; i++) {
        auto ce = light_man.instance_pool.entity[i];
        auto pos = get_coord_containing(*pos_man.get_instance_data(ce).position);
        auto powered = *power_man.get_instance_data(ce).powered;
        if (powered) {
            set_light_level(pos.x, pos.y, pos.z, std::max(
                        (int)get_light_level(pos.x, pos.y, pos.z), (int)(255 * light_man.instance_pool.intensity[i])));
        }
    }

    /* 3. propagate max_light_prop times. this is guaranteed to be enough to cover
     * the sources' area of influence. */
    for (int pass = 0; pass < max_light_prop; pass++) {
        for (int k = lightfield_update_mins.z; k <= lightfield_update_maxs.z; k++) {
            for (int j = lightfield_update_mins.y; j <= lightfield_update_maxs.y; j++) {
                for (int i = lightfield_update_mins.x; i <= lightfield_update_maxs.x; i++) {
                    int level = get_light_level(i, j, k);

                    block *b = ship->get_block(glm::ivec3(i, j, k));
                    if (!b)
                        continue;

                    if (light_permeable(b->surfs[surface_xm]))
                        level = std::max(level, get_light_level(i - 1, j, k) - light_atten);
                    if (light_permeable(b->surfs[surface_xp]))
                        level = std::max(level, get_light_level(i + 1, j, k) - light_atten);

                    if (light_permeable(b->surfs[surface_ym]))
                        level = std::max(level, get_light_level(i, j - 1, k) - light_atten);
                    if (light_permeable(b->surfs[surface_yp]))
                        level = std::max(level, get_light_level(i, j + 1, k) - light_atten);

                    if (light_permeable(b->surfs[surface_zm]))
                        level = std::max(level, get_light_level(i, j, k - 1) - light_atten);
                    if (light_permeable(b->surfs[surface_zp]))
                        level = std::max(level, get_light_level(i, j, k + 1) - light_atten);

                    set_light_level(i, j, k, level);
                }
            }
        }
    }

    /* All done. */
    light->upload();
    need_lightfield_update = false;
}


struct game_state {
    virtual ~game_state() {}

    virtual void handle_input() = 0;
    virtual void update(float dt) = 0;
    virtual void render(frame_data *frame) = 0;
    virtual void rebuild_ui() = 0;

    static game_state *create_play_state();
    static game_state *create_menu_state();
    static game_state *create_menu_settings_state();
};


game_state *state = game_state::create_play_state();

void
set_game_state(game_state *s)
{
    if (state)
        delete state;

    state = s;
    pl.ui_dirty = true; /* state change always requires a ui rebuild. */
}

void
prepare_chunks()
{
    /* walk all the chunks -- TODO: only walk chunks that might contribute to the view */
    for (int k = ship->mins.z; k <= ship->maxs.z; k++) {
        for (int j = ship->mins.y; j <= ship->maxs.y; j++) {
            for (int i = ship->mins.x; i <= ship->maxs.x; i++) {
                chunk *ch = ship->get_chunk(glm::ivec3(i, j, k));
                if (ch) {
                    ch->prepare_render(i, j, k);
                }
            }
        }
    }
}

void
init()
{
    gas_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    light_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    physics_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    pos_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    power_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    power_provider_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    render_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    surface_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    switch_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    type_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    door_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    reader_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);
    proximity_man.create_component_instance_data(INITIAL_MAX_COMPONENTS);

    proj_man.create_projectile_data(1000);

    printf("%s starting up.\n", APP_NAME);
    printf("OpenGL version: %.1f\n", epoxy_gl_version() / 10.0f);

    if (epoxy_gl_version() < 33) {
        errx(1, "At least OpenGL 3.3 is required\n");
    }

    /* Enable GL debug extension */
    if (!epoxy_has_gl_extension("GL_KHR_debug"))
        errx(1, "No support for GL debugging, life isn't worth it.\n");

    glEnable(GL_DEBUG_OUTPUT);
    glDebugMessageCallback(reinterpret_cast<GLDEBUGPROC>(&gl_debug_callback), nullptr);

    /* Check for ARB_texture_storage */
    if (!epoxy_has_gl_extension("GL_ARB_texture_storage"))
        errx(1, "No support for ARB_texture_storage\n");

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);         /* pointers given by other libs may not be aligned */
    glEnable(GL_DEPTH_TEST);
    glPolygonOffset(-0.1f, -0.1f);

    mesher_init();

    particle_man = new particle_manager();
    particle_man->create_particle_data(1000);

    projectile_sw = load_mesh("mesh/sphere.dae");
    for (auto i = 0u; i < projectile_sw->num_vertices; ++i) {
        projectile_sw->verts[i].x *= 0.01f;
        projectile_sw->verts[i].y *= 0.01f;
        projectile_sw->verts[i].z *= 0.01f;
    }
    set_mesh_material(projectile_sw, 11);
    projectile_hw = upload_mesh(projectile_sw);

    attachment_sw = load_mesh("mesh/attach.dae");
    set_mesh_material(attachment_sw, 10);
    attachment_hw = upload_mesh(attachment_sw);

    no_placement_sw = load_mesh("mesh/no_place.dae");
    set_mesh_material(no_placement_sw, 11);
    no_placement_hw = upload_mesh(no_placement_sw);

    auto wire_sw = load_mesh("mesh/wire.dae");
    set_mesh_material(wire_sw, 12);
    wire_hw_meshes[wire_type_power] = upload_mesh(wire_sw);
    set_mesh_material(wire_sw, 14);
    wire_hw_meshes[wire_type_comms] = upload_mesh(wire_sw);

    door_sw = load_mesh("mesh/single_door.dae");
    set_mesh_material(door_sw, 2);  /* TODO: paint a new texture for this one */
    door_hw = upload_mesh(door_sw);

    scaffold_sw = load_mesh("mesh/initial_scaffold.dae");

    surfs_sw[surface_xp] = load_mesh("mesh/x_quad_p.dae");
    surfs_sw[surface_xm] = load_mesh("mesh/x_quad.dae");
    surfs_sw[surface_yp] = load_mesh("mesh/y_quad_p.dae");
    surfs_sw[surface_ym] = load_mesh("mesh/y_quad.dae");
    surfs_sw[surface_zp] = load_mesh("mesh/z_quad_p.dae");
    surfs_sw[surface_zm] = load_mesh("mesh/z_quad.dae");

    for (int i = 0; i < 6; i++)
        surfs_hw[i] = upload_mesh(surfs_sw[i]);

    for (auto i = 0u; i < sizeof(entity_types) / sizeof(entity_types[0]); i++) {
        auto t = &entity_types[i];
        t->sw = load_mesh(t->mesh);
        set_mesh_material(t->sw, t->material);
        t->hw = upload_mesh(t->sw);
        build_static_physics_mesh(t->sw, &t->phys_mesh, &t->phys_shape);
    }

    simple_shader = load_shader("shaders/simple.vert", "shaders/simple.frag");
    unlit_shader = load_shader("shaders/simple.vert", "shaders/unlit.frag");
    unlit_instanced_shader = load_shader("shaders/simple_instanced.vert", "shaders/unlit.frag");
    lit_instanced_shader = load_shader("shaders/simple_instanced.vert", "shaders/simple.frag");
    add_overlay_shader = load_shader("shaders/add_overlay.vert", "shaders/unlit.frag");
    remove_overlay_shader = load_shader("shaders/remove_overlay.vert", "shaders/unlit.frag");
    ui_shader = load_shader("shaders/ui.vert", "shaders/ui.frag");
    ui_sprites_shader = load_shader("shaders/ui_sprites.vert", "shaders/ui_sprites.frag");
    sky_shader = load_shader("shaders/sky.vert", "shaders/sky.frag");
    particle_shader = load_shader("shaders/particle.vert", "shaders/particle.frag");
    modelspace_uv_shader = load_shader("shaders/simple_modelspace_uv.vert", "shaders/simple.frag");

    scaffold_hw = upload_mesh(scaffold_sw);         /* needed for overlay */

    glUseProgram(simple_shader);

    world_textures = new texture_set(GL_TEXTURE_2D_ARRAY, WORLD_TEXTURE_DIMENSION, MAX_WORLD_TEXTURES);
    world_textures->load(0, "textures/white.png");
    world_textures->load(1, "textures/scaffold.png");
    world_textures->load(2, "textures/plate.png");
    world_textures->load(3, "textures/frobnicator.png");
    world_textures->load(4, "textures/grate.png");
    world_textures->load(5, "textures/red.png");
    world_textures->load(6, "textures/glass.png");
    world_textures->load(7, "textures/display.png");
    world_textures->load(8, "textures/light.png");
    world_textures->load(9, "textures/switch.png");
    world_textures->load(10, "textures/attach.png");
    world_textures->load(11, "textures/no_place.png");
    world_textures->load(12, "textures/wire.png");
    world_textures->load(13, "textures/plaidnicator.png");
    world_textures->load(14, "textures/comms_wire.png");
    world_textures->load(15, "textures/particle.png");
    world_textures->load(16, "textures/transparent_block.png");

    skybox = new texture_set(GL_TEXTURE_CUBE_MAP, 2048, 6);
    skybox->load(0, "textures/sky_right1.png");
    skybox->load(1, "textures/sky_left2.png");
    skybox->load(2, "textures/sky_top3.png");
    skybox->load(3, "textures/sky_bottom4.png");
    skybox->load(4, "textures/sky_front5.png");
    skybox->load(5, "textures/sky_back6.png");

    ship = ship_space::mock_ship_space();
    if( ! ship )
        errx(1, "Ship_space::mock_ship_space failed\n");

    ship->rebuild_topology();

    printf("Ship is %u chunks, %d..%d %d..%d %d..%d\n",
            (unsigned) ship->chunks.size(),
            ship->mins.x, ship->maxs.x,
            ship->mins.y, ship->maxs.y,
            ship->mins.z, ship->maxs.z);

    ship->validate();

    game_settings = load_settings(en_config_base);
    en_settings user_settings = load_settings(en_config_user);
    game_settings.merge_with(user_settings);

    frames = new frame_data[NUM_INFLIGHT_FRAMES];
    frame_index = 0;

    pl.angle = 0;
    pl.elev = 0;
    pl.pos = glm::vec3(3,2,2);
    pl.selected_slot = 1;
    pl.ui_dirty = true;
    pl.disable_gravity = false;

    phy = new physics(&pl);

    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CCW);

    text = new text_renderer("fonts/pixelmix.ttf", 16);

    ui_sprites = new sprite_renderer();
    unlit_ui_slot_sprite = ui_sprites->load("textures/ui-slot.png");
    lit_ui_slot_sprite = ui_sprites->load("textures/ui-slot-lit.png");

    printf("World vertex size: %zu bytes\n", sizeof(vertex));

    light = new light_field();
    light->bind(1);

    /* put some crap in the lightfield */
    memset(light->data, 0, sizeof(light->data));
    light->upload();

    /* prepare the chunks -- this populates the physics data */
    prepare_chunks();
}


void
resize(int width, int height)
{
    /* TODO: resize offscreen (but screen-sized) surfaces, etc. */
    glViewport(0, 0, width, height);
    wnd.width = width;
    wnd.height = height;
    printf("Resized to %dx%d\n", width, height);
}


void
destroy_entity(entity *e)
{
    /* removing block influence from this ent */
    /* this should really be componentified */
    if (surface_man.exists(e->ce)) {
        auto b = *surface_man.get_instance_data(e->ce).block;
        auto type = &entity_types[*type_man.get_instance_data(e->ce).type];

        for (auto i = 0; i < type->height; i++) {
            auto p = b + glm::ivec3(0, 0, i);
            block *bl = ship->get_block(p);
            assert(bl);
            if (bl->type == block_entity) {
                printf("emptying %d,%d,%d on remove of ent\n", p.x, p.y, p.z);
                bl->type = block_empty;

                for (auto face = 0; face < 6; face++) {
                    /* unreserve all the space */
                    bl->surf_space[face] = 0;
                }
            }
        }
    }

    comparator_man.destroy_entity_instance(e->ce);
    gas_man.destroy_entity_instance(e->ce);
    light_man.destroy_entity_instance(e->ce);
    teardown_static_physics_setup(nullptr, nullptr, physics_man.get_instance_data(e->ce).rigid);
    physics_man.destroy_entity_instance(e->ce);
    pos_man.destroy_entity_instance(e->ce);
    power_man.destroy_entity_instance(e->ce);
    power_provider_man.destroy_entity_instance(e->ce);
    pressure_man.destroy_entity_instance(e->ce);
    render_man.destroy_entity_instance(e->ce);
    surface_man.destroy_entity_instance(e->ce);
    switch_man.destroy_entity_instance(e->ce);
    type_man.destroy_entity_instance(e->ce);
    door_man.destroy_entity_instance(e->ce);
    reader_man.destroy_entity_instance(e->ce);
    proximity_man.destroy_entity_instance(e->ce);

    for (auto _type = 0; _type < num_wire_types; _type++) {
        auto type = (wire_type)_type;
        auto & entity_to_attach_lookup = ship->entity_to_attach_lookups[type];
        auto & wire_attachments = ship->wire_attachments[type];

        /* left side is the index of attach on entity that we're removing
        * right side is the index we moved from the end into left side
        * 0, 2 would be read as "attach at index 2 moved to index 0
        * and assumed that what was at index 0 is no longer valid in referencers
        */
        std::unordered_map<unsigned, unsigned> fixup_attaches_removed;
        auto entity_attaches = entity_to_attach_lookup.find(e->ce);
        if (entity_attaches != entity_to_attach_lookup.end()) {
            auto const & set = entity_attaches->second;
            auto attaches = std::vector<unsigned>(set.begin(), set.end());
            std::sort(attaches.begin(), attaches.end());

            auto att_size = attaches.size();
            for (size_t i = 0; i < att_size; ++i) {
                auto attach = attaches[i];

                // fill left side of fixup map
                fixup_attaches_removed[attach] = 0;
            }

            /* Remove relevant attaches from wire_attachments
            * relevant is an attach that isn't occupying a position
            * will get popped off as a result of moving before removing
            */
            unsigned att_index = (unsigned)attaches.size() - 1;
            auto swap_index = wire_attachments.size() - 1;
            for (auto s = wire_attachments.rbegin();
            s != wire_attachments.rend() && att_index != invalid_attach;) {

                auto from_attach = wire_attachments[swap_index];
                auto rem = attaches[att_index];
                if (swap_index > rem) {
                    wire_attachments[rem] = from_attach;
                    wire_attachments.pop_back();
                    fixup_attaches_removed[rem] = (unsigned)swap_index;
                    --swap_index;
                    ++s;
                }
                else if (swap_index == rem) {
                    wire_attachments.pop_back();
                    fixup_attaches_removed.erase(rem);
                    --swap_index;
                    ++s;
                }

                --att_index;
            }

            /* remove all segments that contain an attach on entity */
            for (auto remove_attach : attaches) {
                remove_segments_containing(ship, type, remove_attach);
            }

            /* remove attaches assigned to entity from ship lookup */
            entity_to_attach_lookup.erase(e->ce);

            for (auto lookup : fixup_attaches_removed) {
                /* we moved m to position r */
                auto r = lookup.first;
                auto m = lookup.second;

                relocate_segments_and_entity_attaches(ship, type, r, m);
            }

            attach_topo_rebuild(ship, type);
        }
    }

    delete e;
}


void
remove_ents_from_surface(glm::ivec3 b, int face)
{
    chunk *ch = ship->get_chunk_containing(b);
    for (auto it = ch->entities.begin(); it != ch->entities.end(); /* */) {
        entity *e = *it;

        /* entities may have been inserted in this chunk which don't have
         * placement on a surface. don't corrupt everything if we hit one.
         */
        if (!surface_man.exists(e->ce)) {
            ++it;
            continue;
        }

        auto surface = surface_man.get_instance_data(e->ce);
        auto p = *surface.block;
        auto f = *surface.face;

        auto type = &entity_types[*type_man.get_instance_data(e->ce).type];

        if (p.x == b.x && p.y == b.y && p.z <= b.z && p.z + type->height > b.z && f == face) {
            destroy_entity(e);
            it = ch->entities.erase(it);

            block *bl = ship->get_block(p);
            assert(bl);
            bl->surf_space[face] = 0;   /* we've popped *everything* off, it must be empty now */
        }
        else {
            ++it;
        }
    }
}


struct add_block_entity_tool : tool
{
    unsigned type = 1;

    bool can_use(raycast_info *rc) {
        if (!rc->hit || rc->inside)
            return false;

        /* don't allow placements that would cause the player to end up inside the ent and get stuck */
        if (rc->p == get_coord_containing(pl.eye) ||
            rc->p == get_coord_containing(pl.pos))
            return false;

        /* block ents can only be placed in empty space, on a scaffold */
        if (!rc->block || rc->block->type != block_support) {
            return false;
        }

        for (auto i = 0; i < entity_types[type].height; i++) {
            block *bl = ship->get_block(rc->p + glm::ivec3(0, 0, i));
            if (bl) {
                /* check for surface ents that would conflict */
                for (int face = 0; face < face_count; face++)
                    if (bl->surf_space[face])
                        return false;
            }
        }

        return true;
    }

    void use(raycast_info *rc) override {
        if (!can_use(rc))
            return;

        chunk *ch = ship->get_chunk_containing(rc->p);
        auto e = new entity(rc->p, type, surface_zm);
        ch->entities.push_back(e);

        for (auto i = 0; i < entity_types[type].height; i++) {
            auto p = rc->p + glm::ivec3(0, 0, i);
            block *bl = ship->ensure_block(p);
            bl->type = block_entity;
            printf("taking block %d,%d,%d\n", p.x, p.y, p.z);

            /* consume ALL the space on the surfaces */
            for (int face = 0; face < face_count; face++) {
                bl->surf_space[face] = ~0;
            }
        }

        place_entity_attaches(rc, surface_zp, e, type);
    }

    void alt_use(raycast_info *rc) override {}

    void long_use(raycast_info *rc) override {}

    void cycle_mode() override {
        do {
            type = (type + 1) % (sizeof(entity_types) / sizeof(*entity_types));
        } while (entity_types[type].placed_on_surface);
    }

    void preview(raycast_info *rc, frame_data *frame) override {
        if (!can_use(rc))
            return;

        auto mat = frame->alloc_aligned<glm::mat4>(1);
        *mat.ptr = mat_position(rc->p);
        mat.bind(1, frame);

        auto t = &entity_types[type];
        draw_mesh(t->hw);

        /* draw a block overlay as well around the block */
        glUseProgram(add_overlay_shader);
        draw_mesh(scaffold_hw);
        glUseProgram(simple_shader);
    }

    void get_description(char *str) override {
        auto t = &entity_types[type];
        sprintf(str, "Place %s", t->name);
    }
};


struct add_surface_entity_tool : tool
{
    unsigned type = 2;  /* bit of a hack -- this is the first with placed_on_surface set. */
                        /* note that we can't cycle_mode() in our ctor as that runs too early,
                           before the entity types are even set up. */

    bool can_use(raycast_info *rc) {
        if (!rc->hit)
            return false;

        block *bl = rc->block;

        if (!bl)
            return false;

        int index = normal_to_surface_index(rc);

        if (bl->surfs[index] == surface_none)
            return false;

        block *other_side = ship->get_block(rc->p);
        unsigned short required_space = ~0; /* TODO: make this a prop of the type + subblock placement */

        if (other_side->surf_space[index ^ 1] & required_space) {
            /* no room on the surface */
            return false;
        }

        return true;
    }

    void use(raycast_info *rc) override {
        if (!can_use(rc))
            return;

        int index = normal_to_surface_index(rc);

        block *other_side = ship->get_block(rc->p);
        unsigned short required_space = ~0; /* TODO: make this a prop of the type + subblock placement */

        chunk *ch = ship->get_chunk_containing(rc->p);
        /* the chunk we're placing into is guaranteed to exist, because there's
         * a surface facing into it */
        assert(ch);

        auto e = new entity(rc->p, type, index ^ 1);
        ch->entities.push_back(e);

        /* take the space. */
        other_side->surf_space[index ^ 1] |= required_space;

        /* mark lighting for rebuild around this point */
        mark_lightfield_update(rc->p);

        place_entity_attaches(rc, index, e, type);
    }

    void alt_use(raycast_info *rc) override {}

    void long_use(raycast_info *rc) override {}

    void cycle_mode() override {
        do {
            type = (type + 1) % (sizeof(entity_types) / sizeof(*entity_types));
        } while (!entity_types[type].placed_on_surface);
    }

    void preview(raycast_info *rc, frame_data *frame) override {
        if (!can_use(rc))
            return;

        int index = normal_to_surface_index(rc);

        auto mat = frame->alloc_aligned<glm::mat4>(1);
        *mat.ptr = mat_block_face(rc->p, index ^ 1);
        mat.bind(1, frame);

        auto t = &entity_types[type];
        draw_mesh(t->hw);

        /* draw a surface overlay here too */
        /* TODO: sub-block placement granularity -- will need a different overlay */
        mat = frame->alloc_aligned<glm::mat4>(1);
        *mat.ptr = mat_position(rc->bl);
        mat.bind(1, frame);

        glUseProgram(add_overlay_shader);
        glEnable(GL_POLYGON_OFFSET_FILL);
        draw_mesh(surfs_hw[index]);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glUseProgram(simple_shader);
    }

    void get_description(char *str) override {
        auto t = &entity_types[type];
        sprintf(str, "Place %s on surface", t->name);
    }
};


struct remove_surface_entity_tool : tool
{
    bool can_use(raycast_info *rc) {
        return rc->hit;
    }

    void use(raycast_info *rc) override {
        if (!can_use(rc))
            return;

        int index = normal_to_surface_index(rc);
        remove_ents_from_surface(rc->p, index^1);
        mark_lightfield_update(rc->p);
    }

    void alt_use(raycast_info *rc) override {}

    void long_use(raycast_info *rc) override {}

    void cycle_mode() override {}

    void preview(raycast_info *rc, frame_data *frame) override {
        if (!can_use(rc))
            return;

        int index = normal_to_surface_index(rc);
        block *other_side = ship->get_block(rc->p);

        if (!other_side || !other_side->surf_space[index ^ 1]) {
            return;
        }

        auto mat = frame->alloc_aligned<glm::mat4>(1);
        *mat.ptr = mat_position(rc->bl);
        mat.bind(1, frame);

        glUseProgram(remove_overlay_shader);
        glEnable(GL_POLYGON_OFFSET_FILL);
        draw_mesh(surfs_hw[index]);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glUseProgram(simple_shader);
    }

    void get_description(char *str) override {
        strcpy(str, "Remove surface entity");
    }
};

struct add_wiring_tool : tool
{
    enum add_wiring_state {
        aws_none,
        aws_placing,
        aws_moving,
    };

    unsigned current_attach = invalid_attach;
    wire_attachment old_attach;
    entity *old_entity = nullptr;
    wire_type type;
    add_wiring_state state = aws_none;

    add_wiring_tool() {
        type = (wire_type)0;
    }

    unsigned get_existing_attach_near(glm::vec3 const & pt, unsigned ignore = invalid_attach) {
        /* Some spatial index might be useful here. */
        auto & wire_attachments = ship->wire_attachments[type];

        for (auto i = 0u; i < wire_attachments.size(); i++) {
            auto d = glm::vec3(wire_attachments[i].transform[3][0],
                wire_attachments[i].transform[3][1],
                wire_attachments[i].transform[3][2]) - pt;
            if (glm::dot(d, d) <= 0.025f * 0.025f) {
                if (i == ignore) {
                    continue;
                }
                return i;
            }
        }

        return invalid_attach;
    }

    bool get_attach_point(glm::vec3 start, glm::vec3 dir, glm::vec3 *pt, glm::vec3 *normal, entity **hit_entity) {
        auto end = start + dir * 5.0f;

        *hit_entity = phys_raycast(start, end,
                                    phy->ghostObj, phy->dynamicsWorld);

        auto hit = phys_raycast_generic(start, end,
                                        phy->ghostObj, phy->dynamicsWorld);

        if (!hit.hit)
            return false;

        // offset 0.025 as that's how model is
        *pt = hit.hitCoord + hit.hitNormal * 0.025f;
        *normal = hit.hitNormal;

        return true;
    }

    bool can_place(ship_space *ship,
        unsigned current_attach, unsigned existing_attach,
        entity* hit_entity) {

        auto allow_placement = true;

        auto & ent_att_lookup = ship->entity_to_attach_lookups[type];

        switch (state) {
        case aws_moving:
            {
                /* allow moving attach to entity only
                 * if moving to existing attach
                 */
                if ((existing_attach == invalid_attach ||
                    existing_attach == current_attach) &&
                    hit_entity &&
                    ent_att_lookup.find(hit_entity->ce) != ent_att_lookup.end()) {
                    auto const & atts = ent_att_lookup[hit_entity->ce];
                    if (atts.size() > 0) {
                        allow_placement = false;
                    }
                }
            }
            break;
        case aws_none:
        case aws_placing:
        default:
            {
                /* allow placement on entity only if on existing attach */
                if (existing_attach == invalid_attach &&
                    hit_entity &&
                    ent_att_lookup.find(hit_entity->ce) != ent_att_lookup.end()) {
                    auto const & atts = ent_att_lookup[hit_entity->ce];
                    if (atts.size() > 0) {
                        allow_placement = false;
                    }
                }
            }
            break;
        }
        return allow_placement;
    }

    void preview(raycast_info *rc, frame_data *frame) override {
        if (!rc->hit)
            return;

        /* do a real, generic raycast */

        /* TODO: Move the assignment logic into the wiring system */

        entity *hit_entity = nullptr;
        glm::vec3 pt;
        glm::vec3 normal;

        for (auto t = 0u; t < num_wire_types; ++t) {
            ship->active_wire[t][0] = invalid_wire;
            ship->active_wire[t][1] = invalid_wire;
        }

        if (!get_attach_point(pl.eye, pl.dir, &pt, &normal, &hit_entity)) {
            return;
        }

        auto & wire_attachments = ship->wire_attachments[type];

        unsigned existing_attach = get_existing_attach_near(pt);
        unsigned existing_attach_ignore = get_existing_attach_near(pt, current_attach);

        wire_attachment a1;
        wire_attachment a2;

        auto allow_placement = can_place(ship, current_attach,
            existing_attach, hit_entity);

        switch (state) {
        case aws_placing:
            {
                if (current_attach != invalid_attach) {
                    a1 = wire_attachments[current_attach];

                    ship->active_wire[type][0] = attach_topo_find(ship, type, a1.parent);
                }

                /* placing/moving */
                if (current_attach == existing_attach) {
                    a1.transform = mat_position(pt);
                }

                if (existing_attach != invalid_attach) {
                    a2 = wire_attachments[existing_attach];
                    pt = glm::vec3(a2.transform[3]);

                    ship->active_wire[type][1] = attach_topo_find(ship, type, a2.parent);
                }
                else {
                    a2 = { mat_rotate_mesh(pt, normal) };
                }
            }
            break;
        case aws_moving:
            {
                a1 = wire_attachments[current_attach];

                if (allow_placement) {
                    glm::mat4 mat;
                    if (existing_attach_ignore != invalid_attach) {
                        mat = wire_attachments[existing_attach_ignore].transform;
                    }
                    else {
                        mat = mat_rotate_mesh(pt, normal);
                    }

                    /* todo: this is bad. we shouldn't be modifying state in preview
                     * as preview now lives in our draw loop
                     */
                    wire_attachments[current_attach].transform = mat;

                    if (current_attach == existing_attach) {
                        a1.transform = mat_position(pt);
                    }
                }

                ship->active_wire[type][0] = attach_topo_find(ship, type, a1.parent);

                if (existing_attach != invalid_attach) {
                    a2 = wire_attachments[existing_attach];
                    pt = glm::vec3(a2.transform[3]);

                    ship->active_wire[type][1] = attach_topo_find(ship, type, a2.parent);
                }
                else {
                    a2 = { mat_rotate_mesh(pt, normal) };
                }
            }
            break;
        case aws_none:
            {
                if (existing_attach != invalid_attach) {
                    a2 = wire_attachments[existing_attach];
                    pt = glm::vec3(a2.transform[3]);

                    ship->active_wire[type][1] = attach_topo_find(ship, type, a2.parent);
                }
                else {
                    a2 = { mat_rotate_mesh(pt, normal) };
                }
            }
            break;
        default:
            break;
        }

        /* if existing, place preview mesh as existing
        * otherwise use raycast info
        */
        auto mat = frame->alloc_aligned<glm::mat4>(1);
        *mat.ptr = a2.transform;
        mat.bind(1, frame);

        glUseProgram(unlit_shader);
        draw_mesh(allow_placement ? attachment_hw : no_placement_hw);
        glUseProgram(simple_shader);

        if (current_attach == invalid_attach)
            return;

        /* draw wire segment to preview attach location */
        if (allow_placement && current_attach != existing_attach) {
            mat = frame->alloc_aligned<glm::mat4>(1);
            *mat.ptr = calc_segment_matrix(a1, a2);
            mat.bind(1, frame);

            glUseProgram(unlit_shader);
            draw_mesh(wire_hw_meshes[type]);
            glUseProgram(simple_shader);
        }
    }

    void use(raycast_info *rc) override {
        entity *hit_entity = nullptr;
        glm::vec3 pt;
        glm::vec3 normal;

        if (!get_attach_point(pl.eye, pl.dir, &pt, &normal, &hit_entity))
            return;

        auto & wire_attachments = ship->wire_attachments[type];
        auto & wire_segments = ship->wire_segments[type];
        auto & entity_to_attach_lookup = ship->entity_to_attach_lookups[type];

        switch (state) {
        case aws_none:
        case aws_placing:
            {
                unsigned existing_attach = get_existing_attach_near(pt);

                if (!can_place(ship, current_attach, existing_attach,
                    hit_entity)) {

                    return;
                }

                unsigned new_attach;
                if (existing_attach == invalid_attach) {
                    new_attach = (unsigned)wire_attachments.size();
                    wire_attachment wa = { mat_rotate_mesh(pt, normal), new_attach, 0 };
                    wire_attachments.push_back(wa);
                }
                else {
                    new_attach = existing_attach;
                }

                if (current_attach != invalid_attach) {
                    wire_segment s;
                    s.first = current_attach;
                    s.second = new_attach;
                    wire_segments.push_back(s);

                    /* merge! */
                    attach_topo_unite(ship, type, current_attach, new_attach);
                }

                current_attach = new_attach;

                if (hit_entity && current_attach != invalid_attach) {
                    entity_to_attach_lookup[hit_entity->ce].insert(current_attach);
                }

                state = aws_placing;
            }
            break;
        case aws_moving:
            {
                /* did we just move to an already existing attach */
                unsigned existing_attach = get_existing_attach_near(pt, current_attach);

                /* we did move to an existing. need to merge */
                if (existing_attach != invalid_attach) {
                    relocate_segments_and_entity_attaches(ship, type, existing_attach, current_attach);

                    auto back_attach = (unsigned)wire_attachments.size() - 1;
                    /* no segments */
                    if (back_attach != invalid_attach) {
                        wire_attachments[current_attach] = wire_attachments[back_attach];
                        wire_attachments.pop_back();

                        relocate_segments_and_entity_attaches(ship, type, current_attach, back_attach);

                        attach_topo_rebuild(ship, type);
                    }

                    /* update current */
                    current_attach = existing_attach;
                }

                /* did we move to be on an entity */
                if (hit_entity && current_attach != invalid_attach) {
                    if (current_attach != existing_attach &&
                        !can_place(ship, current_attach, existing_attach,
                            hit_entity)) {

                        return;
                    }

                    entity_to_attach_lookup[hit_entity->ce].insert(current_attach);
                }

                current_attach = invalid_attach;

                state = aws_none;
            }
            break;
        default:
            break;
        }

        reduce_segments(ship, type);
    }

    void alt_use(raycast_info *rc) override {
        auto & wire_attachments = ship->wire_attachments[type];
        auto & entity_to_attach_lookup = ship->entity_to_attach_lookups[type];

        switch (state) {
        case aws_none:
            {
                /* remove existing attach, and dependent segments */
                entity *hit_entity = nullptr;
                glm::vec3 pt;
                glm::vec3 normal;

                /* nowhere to place attach, so can't find */
                if (!get_attach_point(pl.eye, pl.dir, &pt, &normal, &hit_entity)) {
                    break;
                }

                unsigned existing_attach = get_existing_attach_near(pt);
                if (existing_attach == invalid_attach) {
                    /* not pointing at an attach */
                    break;
                }

                /* remove attach from entity lookup */
                if (hit_entity) {
                    entity_to_attach_lookup[hit_entity->ce].erase(existing_attach);
                }

                unsigned attach_moving_for_delete = (unsigned)wire_attachments.size() - 1;

                auto changed = remove_segments_containing(ship, type, existing_attach);
                if (relocate_segments_and_entity_attaches(ship, type,
                    existing_attach, attach_moving_for_delete)) {
                    changed = true;
                }

                /* move attach_moving_for_delete to existing_attach, and trim off the last one. */
                wire_attachments[existing_attach] = wire_attachments[attach_moving_for_delete];
                wire_attachments.pop_back();

                /* if we changed anything, rebuild the topology */
                if (changed) {
                    attach_topo_rebuild(ship, type);
                }

                state = aws_none;
            }
            break;
        case aws_placing:
            {
                /* terminate the current run */
                if (current_attach != invalid_attach) {
                    current_attach = invalid_attach;
                }

                state = aws_none;
            }
            break;
        case aws_moving:
            {
                /* reset to old spot if moving. "cancel" */
                wire_attachments[current_attach] = old_attach;

                if (old_entity) {
                    entity_to_attach_lookup[old_entity->ce].insert(current_attach);
                    old_entity = nullptr;
                }

                current_attach = invalid_attach;
                state = aws_none;

                state = aws_none;
            }
            break;
        default:
            break;
        }
    }

    void long_use(raycast_info *rc) override {
        entity *hit_entity = nullptr;
        glm::vec3 pt;
        glm::vec3 normal;
        unsigned existing_attach;

        auto & wire_attachments = ship->wire_attachments[type];
        auto & entity_to_attach_lookup = ship->entity_to_attach_lookups[type];

        switch (state) {
        case aws_none:
            {
                if (current_attach == invalid_attach) {
                    if (!get_attach_point(pl.eye, pl.dir, &pt, &normal, &hit_entity))
                        return;

                    existing_attach = get_existing_attach_near(pt);

                    if (existing_attach == invalid_attach) {
                        return;
                    }

                    /* cast ray backwards from attach
                     * should find us the entity attach is on
                     */
                    auto att_mat = wire_attachments[existing_attach].transform;
                    auto att_rot = glm::vec3(att_mat[2][0], att_mat[2][1], att_mat[2][2]);
                    auto att_pos = glm::vec3(att_mat[3]);
                    att_rot *= -1.f;
                    get_attach_point(att_pos, att_rot, &pt, &normal, &hit_entity);

                    current_attach = existing_attach;

                    /* remove this attach from entity attaches
                     * will get added back if needed in use()/alt_use()
                     */
                    auto & lookup = entity_to_attach_lookup;
                    if (hit_entity && lookup.find(hit_entity->ce) != lookup.end()) {
                        lookup[hit_entity->ce].erase(current_attach);
                    }

                    old_attach = wire_attachments[current_attach];
                    old_entity = hit_entity;
                }
                state = aws_moving;
            }
            break;
        case aws_placing:
        case aws_moving:
        default:
            break;
        }
    }

    void cycle_mode() override {
        switch (state) {
        case aws_none:
            {
                type = (wire_type)(((unsigned)type + (unsigned)num_wire_types + 1) % (unsigned)num_wire_types);
            }
            break;
        case aws_placing:
        case aws_moving:
        default:
            break;
        }
    }

    void get_description(char *str) override {
        auto name = wire_type_names[type];
        sprintf(str, "Place %s wiring", name);
    }
};


struct flashlight_tool : tool
{
    /* A good flashlight can throw pretty far. 5m is chosen as an average
     * midrange [nice] flashlight / 10. */
    const float flashlight_throw = 5.0f;
    struct entity *flashlight = nullptr;
    glm::ivec3 last_pos;
    bool flashlight_on = false;

    /* The flashlight is just a light at some location which can be "seen"
     * by the player, move it to where the raycast hit */
    void update_light() {
        glm::ivec3 new_pos;
        bool should_light = false;
        power_component_manager::instance_data power =
            power_man.get_instance_data(flashlight->ce);

        /* FIXME: flashlight shouldn't be at eye, should be mid body */
        entity *hit_entity = phys_raycast(pl.eye, pl.eye + pl.dir * flashlight_throw,
                                          phy->ghostObj, phy->dynamicsWorld);


        if (hit_entity) {
            /* The raycast hit a mesh which needs no special considerations for
             * lighting. */
            new_pos = *pos_man.get_instance_data(hit_entity->ce).position;
            should_light = true;
        } else {
            generic_raycast_info hit = phys_raycast_generic(pl.eye,
                                                            pl.eye + pl.dir * flashlight_throw,
                                                            phy->ghostObj, phy->dynamicsWorld);

            if (hit.hit) {
                assert(ship->get_block(hit.hitCoord)); /* must be a block */

                /* The goal is to not place the lighting within the block. The
                 * perfect solution would put the light exactly on the
                 * containing block of the hit. This is not necessary. If this
                 * is a block, there must be a direct line of sight, and we
                 * should be able to simply "back out" a reasonable amount,
                 * provided it isn't behind the player
                 */

                new_pos = hit.hitCoord + hit.hitNormal * 0.5f;
                should_light = true;
            }
        }

        should_light = should_light && flashlight_on;

        new_pos = get_coord_containing(new_pos);

        /* To prevent unnecessary processing, we only want to update things if
         * the position changed, or the flashlight status changed */
        if (new_pos == last_pos && *power.powered == should_light) {
            return;
        }

        *power.powered = should_light;
        *pos_man.get_instance_data(flashlight->ce).position = new_pos;
        mark_lightfield_update(new_pos);
        mark_lightfield_update(last_pos);
        last_pos = new_pos;
    }

    void use(raycast_info *rc) override {
        if (!flashlight) {
            flashlight = new entity(rc->p, 11, surface_xp);
            last_pos = pl.pos;
        }

        flashlight_on = !flashlight_on;
        update_light();
    }

    void alt_use(raycast_info *rc) override { }

    void long_use(raycast_info *rc) override { }

    void cycle_mode() override {
        /* different flashlight focal lengths */
    }

    void preview(raycast_info *rc, frame_data *frame) override {
        if (flashlight)
            update_light();
    }

    void get_description(char *str) override {
        sprintf(str, "Ghetto flashlight");
    }
};


tool *tools[] = {
    tool::create_fire_projectile_tool(&pl),
    tool::create_add_block_tool(),
    tool::create_remove_block_tool(),
    new add_surface_tool(),
    tool::create_remove_surface_tool(),
    new add_block_entity_tool(),
    new add_surface_entity_tool(),
    new remove_surface_entity_tool(),
    new add_wiring_tool(),
    new flashlight_tool()
};


void
add_text_with_outline(char const *s, float x, float y, float r = 1, float g = 1, float b = 1)
{
    text->add(s, x - 2, y, 0, 0, 0);
    text->add(s, x + 2, y, 0, 0, 0);
    text->add(s, x, y - 2, 0, 0, 0);
    text->add(s, x, y + 2, 0, 0, 0);
    text->add(s, x, y, r, g, b);
}


struct time_accumulator
{
    float period;
    float max_period;
    float accum;

    time_accumulator(float period, float max_period) :
        period(period), max_period(max_period), accum(0.0f) {}

    void add(float dt) {
        accum = std::min(accum + dt, max_period);
    }

    bool tick()
    {
        if (accum >= period) {
            accum -= period;
            return true;
        }

        return false;
    }
};


void render() {
    float depthClearValue = 1.0f;
    glClearBufferfv(GL_DEPTH, 0, &depthClearValue);

    frame = &frames[frame_index++];
    if (frame_index >= NUM_INFLIGHT_FRAMES) {
        frame_index = 0;
    }

    frame->begin();

    pl.dir = glm::vec3(
        cosf(pl.angle) * cosf(pl.elev),
        sinf(pl.angle) * cosf(pl.elev),
        sinf(pl.elev)
        );

    /* pl.pos is center of capsule */
    pl.eye = pl.pos + glm::vec3(0, 0, pl.height / 2 - EYE_OFFSET_Z);

    auto vfov = hfov * (float)wnd.height / wnd.width;

    glm::mat4 proj = glm::perspective(vfov, (float)wnd.width / wnd.height, 0.01f, 1000.0f);
    glm::mat4 view = glm::lookAt(pl.eye, pl.eye + pl.dir, glm::vec3(0, 0, 1));
    glm::mat4 centered_view = glm::lookAt(glm::vec3(0), pl.dir, glm::vec3(0, 0, 1));

    auto camera_params = frame->alloc_aligned<per_camera_params>(1);

    camera_params.ptr->view_proj_matrix = proj * view;
    camera_params.ptr->inv_centered_view_proj_matrix = glm::inverse(proj * centered_view);
    camera_params.ptr->aspect = (float)wnd.width / wnd.height;
    camera_params.bind(0, frame);

    world_textures->bind(0);

    prepare_chunks();

    for (int k = ship->mins.z; k <= ship->maxs.z; k++) {
        for (int j = ship->mins.y; j <= ship->maxs.y; j++) {
            for (int i = ship->mins.x; i <= ship->maxs.x; i++) {
                /* TODO: prepare all the matrices first, and do ONE upload */
                chunk *ch = ship->get_chunk(glm::ivec3(i, j, k));
                if (ch) {
                    auto chunk_matrix = frame->alloc_aligned<glm::mat4>(1);
                    *chunk_matrix.ptr = mat_position(CHUNK_SIZE * glm::ivec3(i, j, k));
                    chunk_matrix.bind(1, frame);
                    draw_mesh(ch->render_chunk.mesh);
                }
            }
        }
    }

    state->render(frame);

    draw_renderables(frame);
    glUseProgram(modelspace_uv_shader);
    draw_doors(frame);

    /* draw the projectiles */
    glUseProgram(unlit_instanced_shader);
    draw_projectiles(proj_man, frame);
    glUseProgram(lit_instanced_shader);
    draw_attachments(ship, frame);
    draw_segments(ship, frame);
    glUseProgram(unlit_instanced_shader);
    draw_attachments_on_active_wire(ship, frame);
    draw_active_segments(ship, frame);

    /* draw the sky */
    glUseProgram(sky_shader);
    skybox->bind(0);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDepthFunc(GL_LESS);

    /* Draw particles with depth test on but writes off */
    glUseProgram(particle_shader);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    draw_particles(particle_man, frame);
    glDisable(GL_BLEND);

    /* Reenable depth write */
    glDepthMask(GL_TRUE);

    if (draw_hud) {
        /* draw the ui */
        glDisable(GL_DEPTH_TEST);

        glUseProgram(ui_shader);
        text->draw();
        glUseProgram(ui_sprites_shader);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ui_sprites->draw();
        glDisable(GL_BLEND);

        glEnable(GL_DEPTH_TEST);
    }

    glUseProgram(simple_shader);

    frame->end();
}


time_accumulator main_tick_accum(1/15.0f, 1.f);  /* 15Hz tick for game logic */
time_accumulator fast_tick_accum(1/60.0f, 1.f);  /* 60Hz tick for motion */

void
update()
{
    frame_info.tick();
    auto dt = frame_info.dt;

    main_tick_accum.add(dt);
    fast_tick_accum.add(dt);

    /* this absolutely must run every frame */
    state->update(dt);

    /* things that can run at a pretty slow rate */
    while (main_tick_accum.tick()) {

        /* remove any air that someone managed to get into the outside */
        {
            topo_info *t = topo_find(&ship->outside_topo_info);
            zone_info *z = ship->get_zone_info(t);
            if (z) {
                /* try as hard as you like, you cannot fill space with your air system */
                z->air_amount = 0;
            }
        }

        /* allow the entities to tick */
        tick_readers(ship);
        tick_gas_producers(ship);
        tick_power_consumers(ship);
        tick_light_components(ship);
        tick_pressure_sensors(ship);
        tick_sensor_comparators(ship);
        tick_proximity_sensors(ship, &pl);
        tick_doors(ship);

        /* rebuild lighting if needed */
        update_lightfield();

        calculate_power_wires(ship);
        propagate_comms_wires(ship);

        if (pl.ui_dirty || draw_debug_text || draw_fps) {
            text->reset();
            ui_sprites->reset();

            state->rebuild_ui();

            if (draw_fps) {
                char buf[3][256];
                float w[3] = { 0, 0, 0 }, h = 0;

                sprintf(buf[0], "%.2f", frame_info.dt * 1000);
                sprintf(buf[1], "%.2f", 1.f / frame_info.dt);
                sprintf(buf[2], "%.2f", frame_info.fps);

                text->measure(buf[0], &w[0], &h);
                text->measure(buf[1], &w[1], &h);
                text->measure(buf[2], &w[2], &h);

                add_text_with_outline(buf[0], -DEFAULT_WIDTH / 2 + (100 - w[0]), DEFAULT_HEIGHT / 2 + 100);
                add_text_with_outline(buf[1], -DEFAULT_WIDTH / 2 + (100 - w[1]), DEFAULT_HEIGHT / 2 + 82);
                add_text_with_outline(buf[2], -DEFAULT_WIDTH / 2 + (100 - w[2]), DEFAULT_HEIGHT / 2 + 64);
            }

            text->upload();

            ui_sprites->upload();
            pl.ui_dirty = false;
        }
    }

    /* character controller tick: we'd LIKE to run this off the fast_tick_accum, but it has all kinds of
     * every-frame assumptions baked in (player impulse state, etc) */
    phy->tick_controller(dt);

    while (fast_tick_accum.tick()) {

        proj_man.simulate(fast_tick_accum.period);
        particle_man->simulate(fast_tick_accum.period);

        phy->tick(fast_tick_accum.period);

    }
}



action const* get_input(en_action a) {
    return &game_settings.bindings.bindings[a];
}


struct play_state : game_state {
    entity *use_entity = nullptr;

    play_state() {
    }

    void rebuild_ui() override {
        float w = 0;
        float h = 0;
        char buf[256];
        char buf2[512];

        {
            /* Tool name down the bottom */
            tool *t = tools[pl.selected_slot];

            if (t) {
                t->get_description(buf);
            }
            else {
                strcpy(buf, "(no tool)");
            }
        }

        text->measure(".", &w, &h);
        add_text_with_outline(".", -w/2, -w/2);

        auto bind = game_settings.bindings.bindings.find(action_use_tool);
        auto key = lookup_key((*bind).second.binds.inputs[0]);
        sprintf(buf2, "%s: %s", key, buf);
        text->measure(buf2, &w, &h);
        add_text_with_outline(buf2, -w/2, -400);

        /* Gravity state (temp) */
        w = 0; h = 0;
        bind = game_settings.bindings.bindings.find(action_gravity);
        key = lookup_key((*bind).second.binds.inputs[0]);
        sprintf(buf, "Gravity: %s (%s to toggle)", pl.disable_gravity ? "OFF" : "ON", key);
        text->measure(buf, &w, &h);
        add_text_with_outline(buf, -w/2, -430);

        /* Use key affordance */
        bind = game_settings.bindings.bindings.find(action_use);
        key = lookup_key((*bind).second.binds.inputs[0]);
        if (use_entity) {
            auto type = &entity_types[*type_man.get_instance_data(use_entity->ce).type];
            sprintf(buf2, "%s Use the %s", key, type->name);
            w = 0; h = 0;
            text->measure(buf2, &w, &h);
            add_text_with_outline(buf2, -w/2, -200);
        }

        /* debug text */
        if (draw_debug_text) {
            /* Atmo status */
            glm::ivec3 eye_block = get_coord_containing(pl.eye);

            topo_info *t = topo_find(ship->get_topo_info(eye_block));
            topo_info *outside = topo_find(&ship->outside_topo_info);
            zone_info *z = ship->get_zone_info(t);
            float pressure = z ? (z->air_amount / t->size) : 0.0f;

            if (t != outside) {
                sprintf(buf2, "[INSIDE %p %d %.1f atmo]", t, t->size, pressure);
            }
            else {
                sprintf(buf2, "[OUTSIDE %p %d %.1f atmo]", t, t->size, pressure);
            }

            w = 0; h = 0;
            text->measure(buf2, &w, &h);
            add_text_with_outline(buf2, -w/2, -100);

            w = 0; h = 0;
            sprintf(buf2, "full: %d fast-unify: %d fast-nosplit: %d false-split: %d",
                    ship->num_full_rebuilds,
                    ship->num_fast_unifys,
                    ship->num_fast_nosplits,
                    ship->num_false_splits);
            text->measure(buf2, &w, &h);
            add_text_with_outline(buf2, -w/2, -150);
        }

        unsigned num_tools = sizeof(tools) / sizeof(tools[0]);
        for (unsigned i = 0; i < num_tools; i++) {
            ui_sprites->add(pl.selected_slot == i ? &lit_ui_slot_sprite : &unlit_ui_slot_sprite,
                    (i - num_tools/2.0f) * 34, -220);
        }
    }

    void update(float dt) override {
        if (wnd.has_focus && SDL_GetRelativeMouseMode() == SDL_FALSE) {
            SDL_SetRelativeMouseMode(SDL_TRUE);
        }

        if (!wnd.has_focus && SDL_GetRelativeMouseMode() != SDL_FALSE) {
            SDL_SetRelativeMouseMode(SDL_FALSE);
        }


        auto *t = tools[pl.selected_slot];

        if (t) {
            /* both tool use and overlays need the raycast itself */
            raycast_info rc;
            ship->raycast(pl.eye, pl.dir, &rc);

            /* tool use */
            if (pl.use_tool) {
                t->use(&rc);
                pl.ui_dirty = true;
            }

            if (pl.alt_use_tool) {
                t->alt_use(&rc);
                pl.ui_dirty = true;
            }

            if (pl.long_use_tool) {
                t->long_use(&rc);
                pl.ui_dirty = true;
            }

            if (pl.cycle_mode) {
                t->cycle_mode();
                pl.ui_dirty = true;
            }
        }

        /* interact with ents. do this /after/
         * anything that may delete the entity
         */
        entity *hit_ent = phys_raycast(pl.eye, pl.eye + 2.f * pl.dir,
            phy->ghostObj, phy->dynamicsWorld);
        /* can only interact with entities which have
         * the switch component
         */
        if (hit_ent && !switch_man.exists(hit_ent->ce)) {
            hit_ent = nullptr;
        }

        if (hit_ent != use_entity) {
            use_entity = hit_ent;
            pl.ui_dirty = true;
        }

        if (pl.use && hit_ent) {
            use_action_on_entity(ship, hit_ent->ce);
        }
    }

    void render(frame_data *frame) override {
        auto *t = tools[pl.selected_slot];

        if (t == nullptr) {
            return;
        }

        raycast_info rc;
        ship->raycast(pl.eye, pl.dir, &rc);

        /* tool preview */
        t->preview(&rc, frame);
    }

    void set_slot(unsigned slot) {
        /* note: all the number keys are bound, but we may not have 10 toolbelt slots.
         * just drop bogus slot requests on the floor.
         */
        if (slot < sizeof(tools) / sizeof(tools[0])) {
            pl.selected_slot = slot;
            pl.ui_dirty = true;
        }
    }

    void cycle_slot(int d) {
        unsigned num_tools = sizeof(tools) / sizeof(tools[0]);
        unsigned int cur_slot = pl.selected_slot;
        cur_slot = (cur_slot + num_tools + d) % num_tools;

        pl.selected_slot = cur_slot;
        pl.ui_dirty = true;
    }

    void handle_input() override {
        /* look */
        auto look_x     = get_input(action_look_x)->value;
        auto look_y     = get_input(action_look_y)->value;

        /* movement */
        auto moveX      = get_input(action_right)->active - get_input(action_left)->active;
        auto moveY      = get_input(action_forward)->active - get_input(action_back)->active;

        /* crouch */
        auto crouch     = get_input(action_crouch)->active;
        auto crouch_end = get_input(action_crouch)->just_inactive;

        /* momentary */
        auto jump       = get_input(action_jump)->just_active;
        auto reset      = get_input(action_reset)->just_active;
        auto use        = get_input(action_use)->just_active;
        auto cycle_mode = get_input(action_cycle_mode)->just_active;
        auto slot1      = get_input(action_slot1)->just_active;
        auto slot2      = get_input(action_slot2)->just_active;
        auto slot3      = get_input(action_slot3)->just_active;
        auto slot4      = get_input(action_slot4)->just_active;
        auto slot5      = get_input(action_slot5)->just_active;
        auto slot6      = get_input(action_slot6)->just_active;
        auto slot7      = get_input(action_slot7)->just_active;
        auto slot8      = get_input(action_slot8)->just_active;
        auto slot9      = get_input(action_slot9)->just_active;
        auto slot0      = get_input(action_slot0)->just_active;
        auto gravity    = get_input(action_gravity)->just_active;
        auto next_tool  = get_input(action_tool_next)->just_active;
        auto prev_tool  = get_input(action_tool_prev)->just_active;

        auto input_use_tool     = get_input(action_use_tool);
        auto use_tool           = input_use_tool->just_pressed;
        auto long_use_tool      = input_use_tool->held;
        auto input_alt_use_tool = get_input(action_alt_use_tool);
        auto alt_use_tool       = input_alt_use_tool->just_pressed;

        /* persistent */

        float mouse_invert = game_settings.input.mouse_invert;

        pl.angle += game_settings.input.mouse_x_sensitivity * look_x;
        pl.elev += game_settings.input.mouse_y_sensitivity * mouse_invert * look_y;

        if (pl.elev < -MOUSE_Y_LIMIT)
            pl.elev = -MOUSE_Y_LIMIT;
        if (pl.elev > MOUSE_Y_LIMIT)
            pl.elev = MOUSE_Y_LIMIT;

        pl.move = glm::vec2((float) moveX, (float) moveY);

        pl.jump          = jump;
        pl.crouch        = crouch;
        pl.reset         = reset;
        pl.crouch_end    = crouch_end;
        pl.use           = use;
        pl.cycle_mode    = cycle_mode;
        pl.gravity       = gravity;
        pl.use_tool      = use_tool;
        pl.alt_use_tool  = alt_use_tool;
        pl.long_use_tool = long_use_tool;

        // blech. Tool gets used below, then fire projectile gets hit here
        if (pl.fire_projectile) {
            auto below_eye = glm::vec3(pl.eye.x, pl.eye.y, pl.eye.z - 0.1);
            proj_man.spawn(below_eye, pl.dir);
            pl.fire_projectile = false;
        }

        if (next_tool) {
            cycle_slot(1);
        }
        if (prev_tool) {
            cycle_slot(-1);
        }

        if (slot1) set_slot(1);
        if (slot2) set_slot(2);
        if (slot3) set_slot(3);
        if (slot4) set_slot(4);
        if (slot5) set_slot(5);
        if (slot6) set_slot(6);
        if (slot7) set_slot(7);
        if (slot8) set_slot(8);
        if (slot9) set_slot(9);
        if (slot0) set_slot(0);

        /* limit to unit vector */
        float len = glm::length(pl.move);
        if (len > 0.0f)
            pl.move = pl.move / len;

        if (get_input(action_menu)->just_active) {
            set_game_state(create_menu_state());
        }
    }
};


struct menu_state : game_state
{
    typedef std::pair<char const *, std::function<void()>> menu_item;
    std::vector<menu_item> items;
    unsigned selected = 0;

    menu_state() : items() {
        items.push_back(menu_item("Resume Game", []{ set_game_state(create_play_state()); }));
        items.push_back(menu_item("Settings", []{ set_game_state(create_menu_settings_state()); }));
        items.push_back(menu_item("Exit Game", []{ exit_requested = true; }));
    }

    void update(float dt) override {
        if (wnd.has_focus && SDL_GetRelativeMouseMode() == SDL_TRUE) {
            SDL_SetRelativeMouseMode(SDL_FALSE);
        }
    }

    void render(frame_data *frame) override {
    }

    void put_item_text(char *dest, char const *src, unsigned index) {
        if (index == selected)
            sprintf(dest, "> %s <", src);
        else
            strcpy(dest, src);
    }

    void rebuild_ui() override {
        float w = 0;
        float h = 0;
        char buf[256];

        sprintf(buf, "Engineer's Nightmare");
        text->measure(buf, &w, &h);
        add_text_with_outline(buf, -w/2, 300);

        float y = 50;
        float dy = -100;

        for (auto it = items.begin(); it != items.end(); it++) {
            w = 0;
            h = 0;
            put_item_text(buf, it->first, (unsigned)(it - items.begin()));
            text->measure(buf, &w, &h);
            add_text_with_outline(buf, -w/2, y);
            y += dy;
        }
    }

    void handle_input() override {
        if (get_input(action_menu_confirm)->just_active) {
            items[selected].second();
        }

        if (get_input(action_menu_down)->just_active) {
            selected = (selected + 1) % items.size();
            pl.ui_dirty = true;
        }

        if (get_input(action_menu_up)->just_active) {
            selected = (unsigned)(selected + items.size() - 1) % items.size();
            pl.ui_dirty = true;
        }

        if (get_input(action_menu)->just_active) {
            set_game_state(create_play_state());
        }
    }
};


struct menu_settings_state : game_state
{
    typedef std::tuple<char const *, char const *, std::function<void()>> menu_item;
    std::vector<menu_item> items;
    int selected = 0;

    char const *on_text = "On";
    char const *off_text = "Off";

    char const *invert_mouse_text = "Invert Mouse: ";

    Uint32 mouse_invert_mi = 0;

    menu_settings_state() {
        mouse_invert_mi = (unsigned)items.size();
        items.push_back(menu_item(invert_mouse_text, "",
            []{ toggle_mouse_invert(); }));
            // ^^ Not real keen on requiring these to be static

        items.push_back(
            menu_item("Save Settings", "",
            []{ save_settings(game_settings); }));

        items.push_back(
            menu_item("Back", "",
            []{ set_game_state(create_menu_state()); }));
    }

    static void toggle_mouse_invert() {
    // ^^ Not real keen on requiring these to be static
        game_settings.input.mouse_invert *= -1;
    }

    void update(float dt) override {
        if (wnd.has_focus && SDL_GetRelativeMouseMode() == SDL_TRUE) {
            SDL_SetRelativeMouseMode(SDL_FALSE);
        }
    }

    void render(frame_data *frame) override {
    }

    void put_item_text(char *dest, char const *src, int index) {
        if (index == selected)
            sprintf(dest, "> %s <", src);
        else
            strcpy(dest, src);
    }

    void rebuild_ui() override {
        menu_item *invert_item = &items.at(mouse_invert_mi);
        std::get<1>(*invert_item) = game_settings.input.mouse_invert > 0 ? off_text : on_text;

        float w = 0;
        float h = 0;
        char buf[256];
        char buf2[256];

        sprintf(buf, "Engineer's Nightmare");
        text->measure(buf, &w, &h);
        add_text_with_outline(buf, -w / 2, 300);

        float y = 50;
        float dy = -100;

        for (auto it = items.begin(); it != items.end(); ++it) {
            w = 0;
            h = 0;
            sprintf(buf2, "%s%s", std::get<0>(*it), std::get<1>(*it));
            put_item_text(buf, buf2, (unsigned)(it - items.begin()));
            text->measure(buf, &w, &h);
            add_text_with_outline(buf, -w / 2, y);
            y += dy;
        }
    }


    void handle_input() override {
        if (get_input(action_menu_confirm)->just_active) {
            std::get<2>(items[selected])();
            pl.ui_dirty = true;
        }

        if (get_input(action_menu_down)->just_active) {
            selected = (selected + 1) % items.size();
            pl.ui_dirty = true;
        }

        if (get_input(action_menu_up)->just_active) {
            selected = (unsigned)(selected + items.size() - 1) % items.size();
            pl.ui_dirty = true;
        }

        if (get_input(action_menu)->just_active) {
            set_game_state(create_play_state());
        }
    }
};


game_state *game_state::create_play_state() { return new play_state; }
game_state *game_state::create_menu_state() { return new menu_state; }
game_state *game_state::create_menu_settings_state() { return new menu_settings_state; }


void
handle_input()
{
    if (wnd.has_focus) {
        set_inputs(keys, mouse_buttons, mouse_axes, game_settings.bindings.bindings);
        state->handle_input();
    }
}


void
run()
{
    for (;;) {
        auto sdl_buttons = SDL_GetRelativeMouseState(nullptr, nullptr);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_left)]      = sdl_buttons & EN_SDL_BUTTON(input_mouse_left);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_middle)]    = sdl_buttons & EN_SDL_BUTTON(input_mouse_middle);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_right)]     = sdl_buttons & EN_SDL_BUTTON(input_mouse_right);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_thumb1)]    = sdl_buttons & EN_SDL_BUTTON(input_mouse_thumb1);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_thumb2)]    = sdl_buttons & EN_SDL_BUTTON(input_mouse_thumb2);
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_wheeldown)] = false;
        mouse_buttons[EN_MOUSE_BUTTON(input_mouse_wheelup)]   = false;

        mouse_axes[EN_MOUSE_AXIS(input_mouse_x)] = 0;
        mouse_axes[EN_MOUSE_AXIS(input_mouse_y)] = 0;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                printf("Quit event caught, shutting down.\n");
                return;

            case SDL_WINDOWEVENT:
                /* We MUST support resize events even if we
                 * don't really care about resizing, because a tiling
                 * WM isn't going to give us what we asked for anyway!
                 */
                switch (e.window.event) {
                case SDL_WINDOWEVENT_RESIZED:
                    resize(e.window.data1, e.window.data2);
                    break;

                case SDL_WINDOWEVENT_FOCUS_LOST:
                    wnd.has_focus = false;
                    break;

                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    wnd.has_focus = true;
                    break;
                }
                break;

            case SDL_MOUSEMOTION:
            {
                auto x = e.motion.xrel;
                auto y = e.motion.yrel;

                x = clamp(x, -MAX_AXIS_PER_EVENT, MAX_AXIS_PER_EVENT);
                y = clamp(y, -MAX_AXIS_PER_EVENT, MAX_AXIS_PER_EVENT);

                mouse_axes[EN_MOUSE_AXIS(input_mouse_x)] += x;
                mouse_axes[EN_MOUSE_AXIS(input_mouse_y)] += y;
                break;
            }

            case SDL_MOUSEWHEEL:
                if (e.wheel.y != 0) {
                    e.wheel.y > 0
                        ? mouse_buttons[EN_MOUSE_BUTTON(input_mouse_wheelup)] = true
                        : mouse_buttons[EN_MOUSE_BUTTON(input_mouse_wheeldown)] = true;
                }
                break;
            }
        }

        /* SDL_PollEvent above has already pumped the input, so current key state is available */
        handle_input();

        update();

        render();

        SDL_GL_SwapWindow(wnd.ptr);

        if (exit_requested) return;
    }
}

int
main(int, char **)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        errx(1, "Error initializing SDL: %s\n", SDL_GetError());

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    wnd.ptr = SDL_CreateWindow(APP_NAME,
                               SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED,
                               DEFAULT_WIDTH,
                               DEFAULT_HEIGHT,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);

    if (!wnd.ptr)
        errx(1, "Failed to create window.\n");

    wnd.gl_ctx = SDL_GL_CreateContext(wnd.ptr);

    keys = SDL_GetKeyboardState(nullptr);

    resize(DEFAULT_WIDTH, DEFAULT_HEIGHT);

    init();

    run();

    return 0;
}

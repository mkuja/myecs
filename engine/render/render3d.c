#include "render/render3d.h"

#include "render/render2d.h" /* MyeHidden is shared by both renderers */

#include <raymath.h>

#include <stdio.h>

ECS_COMPONENT_DECLARE(MyeMeshInstance);
ECS_COMPONENT_DECLARE(MyeCamera3D);
ECS_COMPONENT_DECLARE(MyeLight);
ECS_COMPONENT_DECLARE(MyeRender3dConfig);

/* Queries built once at import, like the 2D renderer's. */
typedef struct MyeRender3dState {
    ecs_query_t *meshes;
    ecs_query_t *cameras;
    ecs_query_t *lights;
    Shader shader;
    bool shader_ready;
    /* Uniform locations, resolved once. */
    int loc_light_dir[MYE_MAX_LIGHTS];
    int loc_light_color[MYE_MAX_LIGHTS];
    int loc_light_enabled[MYE_MAX_LIGHTS];
    int loc_ambient;
    int loc_view_pos;
} MyeRender3dState;

ECS_COMPONENT_DECLARE(MyeRender3dState);

/* --------------------------------------------------------------- shader -- */

/* A small directional-light shader. raylib ships its lighting example as
 * `rlights.h` under examples/ rather than in the library, so the engine
 * carries its own -- kept deliberately minimal: N directional lights, one
 * ambient term, Lambert diffuse plus a Blinn-Phong highlight.
 *
 * Desktop GL wants "#version 330"; WebGL 2 wants "#version 300 es" plus an
 * explicit precision. Those are the ONLY differences: GLSL ES 300 and GLSL
 * 330 both use in/out, texture() and a declared output, so the body below is
 * shared verbatim and there is no second shader to keep in step.
 *
 * WebGL 1 (GLSL ES 100) is deliberately out of scope -- it would need
 * varying/attribute, texture2D and gl_FragColor, i.e. a genuine second
 * shader. See plan/11-web-dev-loop.md.
 *
 * highp rather than mediump: precision qualifiers are ignored on desktop but
 * mediump can visibly band lighting gradients. */
#if defined(PLATFORM_WEB)
#define MYE_GLSL_VERSION "#version 300 es\nprecision highp float;\n"
#else
#define MYE_GLSL_VERSION "#version 330\n"
#endif

static const char *vertex_shader_src =
    MYE_GLSL_VERSION
    "in vec3 vertexPosition;\n"
    "in vec2 vertexTexCoord;\n"
    "in vec3 vertexNormal;\n"
    "in vec4 vertexColor;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "uniform mat4 matNormal;\n"
    "out vec3 fragPosition;\n"
    "out vec2 fragTexCoord;\n"
    "out vec4 fragColor;\n"
    "out vec3 fragNormal;\n"
    "void main() {\n"
    "    fragPosition = vec3(matModel*vec4(vertexPosition, 1.0));\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    fragColor = vertexColor;\n"
    "    fragNormal = normalize(vec3(matNormal*vec4(vertexNormal, 1.0)));\n"
    "    gl_Position = mvp*vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *fragment_shader_src =
    MYE_GLSL_VERSION
    "in vec3 fragPosition;\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "in vec3 fragNormal;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 viewPos;\n"
    "uniform vec4 ambient;\n"
    "uniform vec3 lightDir[4];\n"
    "uniform vec4 lightColor[4];\n"
    "uniform int lightEnabled[4];\n"
    "out vec4 finalColor;\n"
    "void main() {\n"
    "    vec4 texelColor = texture(texture0, fragTexCoord);\n"
    "    vec3 normal = normalize(fragNormal);\n"
    "    vec3 viewD = normalize(viewPos - fragPosition);\n"
    "    vec3 lightAccum = vec3(0.0);\n"
    "    vec3 specAccum = vec3(0.0);\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "        if (lightEnabled[i] == 0) continue;\n"
    "        vec3 l = normalize(-lightDir[i]);\n"
    /* Lambert diffuse: how directly this surface faces the light. */
    "        float ndotl = max(dot(normal, l), 0.0);\n"
    "        lightAccum += pow(lightColor[i].rgb, vec3(2.2))*ndotl;\n"
    "        if (ndotl > 0.0) {\n"
    "            vec3 h = normalize(l + viewD);\n"
    "            specAccum += lightColor[i].rgb*pow(max(dot(normal, h), 0.0), 32.0)*0.25;\n"
    "        }\n"
    "    }\n"
    "    vec4 base = texelColor*colDiffuse*fragColor;\n"
    /* Lighting maths is only correct in LINEAR space, but colours arrive in
     * sRGB, where 128 is not half as bright as 255. So: convert in, light,
     * convert back out. Doing only the output half (the earlier bug here)
     * brightens everything and flattens the contrast. */
    "    vec3 albedo = pow(base.rgb, vec3(2.2));\n"
    "    vec3 ambientLin = pow(ambient.rgb, vec3(2.2));\n"
    "    vec3 lit = albedo*(ambientLin + lightAccum) + specAccum;\n"
    "    finalColor = vec4(pow(lit, vec3(1.0/2.2)), base.a);\n"
    "}\n";

static void resolve_uniforms(MyeRender3dState *state)
{
    state->loc_ambient = GetShaderLocation(state->shader, "ambient");
    state->loc_view_pos = GetShaderLocation(state->shader, "viewPos");

    for (int i = 0; i < MYE_MAX_LIGHTS; ++i) {
        char name[32];
        snprintf(name, sizeof name, "lightDir[%d]", i);
        state->loc_light_dir[i] = GetShaderLocation(state->shader, name);
        snprintf(name, sizeof name, "lightColor[%d]", i);
        state->loc_light_color[i] = GetShaderLocation(state->shader, name);
        snprintf(name, sizeof name, "lightEnabled[%d]", i);
        state->loc_light_enabled[i] = GetShaderLocation(state->shader, name);
    }
}

/* ---------------------------------------------------------------- passes -- */

static const MyeRender3dState *render3d_state(const ecs_world_t *world)
{
    return ecs_singleton_get(world, MyeRender3dState);
}

static bool find_active_camera(ecs_world_t *world, Camera3D *out)
{
    const MyeRender3dState *state = render3d_state(world);
    if (state == NULL || state->cameras == NULL) {
        return false;
    }

    ecs_iter_t it = ecs_query_iter(world, state->cameras);
    while (ecs_query_next(&it)) {
        const MyeCamera3D *cams = ecs_field(&it, MyeCamera3D, 0);
        for (int i = 0; i < it.count; ++i) {
            if (cams[i].active) {
                *out = cams[i].camera;
                ecs_iter_fini(&it);
                return true;
            }
        }
    }
    return false;
}

/* Pushes light state into the shader. Lights beyond MYE_MAX_LIGHTS, and
 * disabled ones, are switched off rather than silently dropped. */
static void upload_lights(ecs_world_t *world, const MyeRender3dState *state,
                          const MyeRender3dConfig *config, Camera3D camera)
{
    float ambient[4] = { (float)config->ambient.r / 255.0f,
                         (float)config->ambient.g / 255.0f,
                         (float)config->ambient.b / 255.0f, 1.0f };
    SetShaderValue(state->shader, state->loc_ambient, ambient,
                   SHADER_UNIFORM_VEC4);

    float view_pos[3] = { camera.position.x, camera.position.y,
                          camera.position.z };
    SetShaderValue(state->shader, state->loc_view_pos, view_pos,
                   SHADER_UNIFORM_VEC3);

    int used = 0;
    if (state->lights != NULL) {
        ecs_iter_t it = ecs_query_iter(world, state->lights);
        while (ecs_query_next(&it)) {
            const MyeLight *lights = ecs_field(&it, MyeLight, 0);
            for (int i = 0; i < it.count && used < MYE_MAX_LIGHTS; ++i) {
                if (!lights[i].enabled) {
                    continue;
                }
                Vector3 d = Vector3Normalize(lights[i].direction);
                float dir[3] = { d.x, d.y, d.z };
                float color[4] = {
                    (float)lights[i].color.r / 255.0f * lights[i].intensity,
                    (float)lights[i].color.g / 255.0f * lights[i].intensity,
                    (float)lights[i].color.b / 255.0f * lights[i].intensity,
                    1.0f
                };
                int on = 1;
                SetShaderValue(state->shader, state->loc_light_dir[used], dir,
                               SHADER_UNIFORM_VEC3);
                SetShaderValue(state->shader, state->loc_light_color[used],
                               color, SHADER_UNIFORM_VEC4);
                SetShaderValue(state->shader, state->loc_light_enabled[used],
                               &on, SHADER_UNIFORM_INT);
                ++used;
            }
        }
    }

    int off = 0;
    for (int i = used; i < MYE_MAX_LIGHTS; ++i) {
        SetShaderValue(state->shader, state->loc_light_enabled[i], &off,
                       SHADER_UNIFORM_INT);
    }
}

static void MyeRender3dPass(ecs_iter_t *it)
{
    const MyeRender3dConfig *config = ecs_field(it, MyeRender3dConfig, 0);
    ecs_world_t *world = it->world;

    const MyeRender3dState *state = render3d_state(world);
    if (state == NULL || state->meshes == NULL) {
        return;
    }

    Camera3D camera;
    if (!find_active_camera(world, &camera)) {
        return; /* nothing to look through: skip the pass entirely */
    }

    if (state->shader_ready) {
        upload_lights(world, state, config, camera);
    }

    BeginMode3D(camera);

    if (config->draw_grid) {
        DrawGrid(config->grid_slices, config->grid_spacing);
    }

    ecs_iter_t iter = ecs_query_iter(world, state->meshes);
    while (ecs_query_next(&iter)) {
        const MyeMeshInstance *instances = ecs_field(&iter, MyeMeshInstance, 0);
        const MyeWorldTransform *transforms =
            ecs_field(&iter, MyeWorldTransform, 1);

        for (int i = 0; i < iter.count; ++i) {
            const Model *model = mye_model_get(world, instances[i].model);
            if (model == NULL || model->meshCount == 0) {
                continue;
            }

            /* Draw each mesh with the entity's world matrix, so parenting
             * works: model.transform is the asset's own baked transform. */
            Matrix matrix = MatrixMultiply(model->transform,
                                           transforms[i].m);

            for (int m = 0; m < model->meshCount; ++m) {
                Material material = model->materials[model->meshMaterial[m]];
                if (state->shader_ready) {
                    material.shader = state->shader;
                }
                Color base = material.maps[MATERIAL_MAP_DIFFUSE].color;
                material.maps[MATERIAL_MAP_DIFFUSE].color = (Color){
                    (unsigned char)((int)base.r * instances[i].tint.r / 255),
                    (unsigned char)((int)base.g * instances[i].tint.g / 255),
                    (unsigned char)((int)base.b * instances[i].tint.b / 255),
                    (unsigned char)((int)base.a * instances[i].tint.a / 255),
                };
                DrawMesh(model->meshes[m], material, matrix);
            }
        }
    }

    EndMode3D();
}

/* -------------------------------------------------------------- helpers -- */

ecs_entity_t mye_mesh_spawn(ecs_world_t *world, mye_model model,
                            Vector3 position, Color tint)
{
    ecs_entity_t e = mye_spawn_3d(world, position);
    ecs_set(world, e, MyeMeshInstance, { .model = model, .tint = tint });
    return e;
}

ecs_entity_t mye_camera3d_spawn(ecs_world_t *world, Vector3 position,
                                Vector3 target, float fov_degrees)
{
    ecs_entity_t e = mye_entity_new(world);
    ecs_set(world, e, MyeCamera3D,
            { .camera = { .position = position,
                          .target = target,
                          .up = { 0.0f, 1.0f, 0.0f },
                          .fovy = fov_degrees,
                          .projection = CAMERA_PERSPECTIVE },
              .active = true });
    return e;
}

/* ------------------------------------------------------------- lifecycle -- */

static void render3d_fini(ecs_world_t *world, void *ctx)
{
    (void)world;
    MyeRender3dState *state = (MyeRender3dState *)ctx;
    if (state == NULL) {
        return;
    }
    if (state->meshes != NULL) ecs_query_fini(state->meshes);
    if (state->cameras != NULL) ecs_query_fini(state->cameras);
    if (state->lights != NULL) ecs_query_fini(state->lights);
    if (state->shader_ready) {
        UnloadShader(state->shader);
        state->shader_ready = false;
    }
    state->meshes = NULL;
    state->cameras = NULL;
    state->lights = NULL;
}

void MyeRender3dModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeRender3dModule);

    ECS_COMPONENT_DEFINE(world, MyeMeshInstance);
    ECS_COMPONENT_DEFINE(world, MyeCamera3D);
    ECS_COMPONENT_DEFINE(world, MyeLight);
    ECS_COMPONENT_DEFINE(world, MyeRender3dConfig);
    ECS_COMPONENT_DEFINE(world, MyeRender3dState);

    ecs_add_id(world, ecs_id(MyeRender3dConfig), EcsSingleton);
    ecs_singleton_set(world, MyeRender3dConfig,
                      { .ambient = (Color){ 60, 60, 70, 255 },
                        .draw_grid = false,
                        .grid_slices = 20,
                        .grid_spacing = 1.0f });

    ecs_add_id(world, ecs_id(MyeRender3dState), EcsSingleton);
    ecs_singleton_set(world, MyeRender3dState, { 0 });
    MyeRender3dState *state = ecs_singleton_ensure(world, MyeRender3dState);

    state->meshes = ecs_query(world, {
        .terms = {
            { .id = ecs_id(MyeMeshInstance), .inout = EcsIn },
            { .id = ecs_id(MyeWorldTransform), .inout = EcsIn },
            { .id = ecs_id(MyeHidden), .oper = EcsNot },
        },
    });
    state->cameras = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyeCamera3D), .inout = EcsIn }},
    });
    state->lights = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyeLight), .inout = EcsIn }},
    });

    const mye_engine *engine = mye_engine_get(world);
    if (engine != NULL && engine->headless) {
        return; /* components and queries only: no GL context to draw with */
    }

    state->shader = LoadShaderFromMemory(vertex_shader_src,
                                         fragment_shader_src);
    state->shader_ready = state->shader.id != 0;
    if (state->shader_ready) {
        resolve_uniforms(state);
    }

    ECS_SYSTEM(world, MyeRender3dPass, MyeOnDraw3D, MyeRender3dConfig);

    ecs_atfini(world, render3d_fini, state);
}

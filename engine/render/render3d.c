#include "render/render3d.h"

#include "render/camera.h"
#include "render/canvas.h"

#include "render/render2d.h" /* MyeHidden is shared by both renderers */

#include "core/log.h"

#include <raymath.h>
#include <rlgl.h>

#include <stdio.h>

ECS_COMPONENT_DECLARE(MyeMeshInstance);
ECS_COMPONENT_DECLARE(MyeModelAnimator);
ECS_COMPONENT_DECLARE(MyeCamera3D);
ECS_COMPONENT_DECLARE(MyeLight);
ECS_COMPONENT_DECLARE(MyeRender3dConfig);

/* Queries built once at import, like the 2D renderer's. */
typedef struct MyeRender3dState {
    ecs_query_t *meshes;
    ecs_query_t *lights;
    Shader shader;
    bool shader_ready;
    Shader pbr;
    bool pbr_ready;
    int loc_pbr_light_dir[MYE_MAX_LIGHTS];
    int loc_pbr_light_color[MYE_MAX_LIGHTS];
    int loc_pbr_light_enabled[MYE_MAX_LIGHTS];
    int loc_pbr_ambient;
    int loc_pbr_view_pos;
    int loc_pbr_metallic;
    int loc_pbr_roughness;
    int loc_pbr_has_mra;
    int loc_pbr_has_emissive;
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

/* Physically-based shading: Cook-Torrance with GGX distribution,
 * Smith geometry and Schlick's Fresnel -- the model glTF materials are
 * authored against.
 *
 * Blinn-Phong above ignores metallic, roughness, normal and emissive maps
 * entirely, so a downloaded glTF renders but looks nothing like its author
 * intended. This one reads them.
 *
 * raylib binds material map i to texture slot i and sets
 * shader.locs[SHADER_LOC_MAP_* ] to that slot, so the sampler names here are
 * wired up in resolve_pbr_uniforms rather than by convention. */
static const char *pbr_fragment_src =
    MYE_GLSL_VERSION
    "in vec3 fragPosition;\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "in vec3 fragNormal;\n"
    "uniform sampler2D albedoMap;\n"
    "uniform sampler2D mraMap;\n"      /* metallic-roughness, glTF packed */
    "uniform sampler2D normalMap;\n"
    "uniform sampler2D emissiveMap;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 viewPos;\n"
    "uniform vec4 ambient;\n"
    "uniform vec3 lightDir[4];\n"
    "uniform vec4 lightColor[4];\n"
    "uniform int lightEnabled[4];\n"
    "uniform float metallicFactor;\n"
    "uniform float roughnessFactor;\n"
    "uniform int hasMraMap;\n"
    "uniform int hasEmissiveMap;\n"
    "out vec4 finalColor;\n"
    "const float PI = 3.14159265359;\n"
    /* GGX/Trowbridge-Reitz: how much of the surface faces the halfway
     * vector. Rough surfaces spread the highlight, smooth ones tighten it. */
    "float distributionGGX(vec3 N, vec3 H, float rough) {\n"
    "    float a = rough*rough;\n"
    "    float a2 = a*a;\n"
    "    float ndoth = max(dot(N, H), 0.0);\n"
    "    float d = ndoth*ndoth*(a2 - 1.0) + 1.0;\n"
    "    return a2/(PI*d*d);\n"
    "}\n"
    /* Smith: how much the microsurface shadows and masks itself. */
    "float geometrySchlick(float ndotv, float rough) {\n"
    "    float k = (rough + 1.0)*(rough + 1.0)/8.0;\n"
    "    return ndotv/(ndotv*(1.0 - k) + k);\n"
    "}\n"
    "float geometrySmith(vec3 N, vec3 V, vec3 L, float rough) {\n"
    "    return geometrySchlick(max(dot(N, V), 0.0), rough)*\n"
    "           geometrySchlick(max(dot(N, L), 0.0), rough);\n"
    "}\n"
    /* Fresnel: everything is mirror-like at grazing angles. */
    "vec3 fresnelSchlick(float cosTheta, vec3 F0) {\n"
    "    return F0 + (1.0 - F0)*pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);\n"
    "}\n"
    "void main() {\n"
    "    vec4 base = texture(albedoMap, fragTexCoord)*colDiffuse*fragColor;\n"
    "    vec3 albedo = pow(base.rgb, vec3(2.2));\n"   /* sRGB -> linear */
    "    float metallic = metallicFactor;\n"
    "    float rough = roughnessFactor;\n"
    "    if (hasMraMap == 1) {\n"
    /* glTF packs roughness in green and metallic in blue. */
    "        vec3 mra = texture(mraMap, fragTexCoord).rgb;\n"
    "        rough *= mra.g;\n"
    "        metallic *= mra.b;\n"
    "    }\n"
    "    rough = clamp(rough, 0.04, 1.0);\n"
    "    vec3 N = normalize(fragNormal);\n"
    "    vec3 V = normalize(viewPos - fragPosition);\n"
    /* Dielectrics reflect ~4% head-on; metals reflect their albedo. */
    "    vec3 F0 = mix(vec3(0.04), albedo, metallic);\n"
    "    vec3 Lo = vec3(0.0);\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "        if (lightEnabled[i] == 0) continue;\n"
    "        vec3 L = normalize(-lightDir[i]);\n"
    "        vec3 H = normalize(V + L);\n"
    "        float ndotl = max(dot(N, L), 0.0);\n"
    "        if (ndotl <= 0.0) continue;\n"
    "        vec3 radiance = pow(lightColor[i].rgb, vec3(2.2));\n"
    "        float D = distributionGGX(N, H, rough);\n"
    "        float G = geometrySmith(N, V, L, rough);\n"
    "        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);\n"
    "        vec3 spec = (D*G*F)/max(4.0*max(dot(N, V), 0.0)*ndotl, 0.001);\n"
    /* Metals have no diffuse: what is not reflected is absorbed. */
    "        vec3 kD = (vec3(1.0) - F)*(1.0 - metallic);\n"
    "        Lo += (kD*albedo/PI + spec)*radiance*ndotl;\n"
    "    }\n"
    "    vec3 color = pow(ambient.rgb, vec3(2.2))*albedo + Lo;\n"
    "    if (hasEmissiveMap == 1) {\n"
    "        color += pow(texture(emissiveMap, fragTexCoord).rgb, vec3(2.2));\n"
    "    }\n"
    /* Reinhard tone map: lights add without bound, so bring the result back
     * into displayable range instead of clipping to flat white. */
    "    color = color/(color + vec3(1.0));\n"
    "    finalColor = vec4(pow(color, vec3(1.0/2.2)), base.a);\n"
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

static void resolve_pbr_uniforms(MyeRender3dState *state)
{
    Shader *sh = &state->pbr;

    state->loc_pbr_ambient = GetShaderLocation(*sh, "ambient");
    state->loc_pbr_view_pos = GetShaderLocation(*sh, "viewPos");
    state->loc_pbr_metallic = GetShaderLocation(*sh, "metallicFactor");
    state->loc_pbr_roughness = GetShaderLocation(*sh, "roughnessFactor");
    state->loc_pbr_has_mra = GetShaderLocation(*sh, "hasMraMap");
    state->loc_pbr_has_emissive = GetShaderLocation(*sh, "hasEmissiveMap");

    for (int i = 0; i < MYE_MAX_LIGHTS; ++i) {
        char name[32];
        snprintf(name, sizeof name, "lightDir[%d]", i);
        state->loc_pbr_light_dir[i] = GetShaderLocation(*sh, name);
        snprintf(name, sizeof name, "lightColor[%d]", i);
        state->loc_pbr_light_color[i] = GetShaderLocation(*sh, name);
        snprintf(name, sizeof name, "lightEnabled[%d]", i);
        state->loc_pbr_light_enabled[i] = GetShaderLocation(*sh, name);
    }

    /* raylib binds material map N to texture slot N and writes the slot index
     * into shader.locs[SHADER_LOC_MAP_* ]. Pointing those at our samplers is
     * what makes the glTF textures arrive. */
    sh->locs[SHADER_LOC_MAP_ALBEDO] = GetShaderLocation(*sh, "albedoMap");
    sh->locs[SHADER_LOC_MAP_METALNESS] = GetShaderLocation(*sh, "mraMap");
    sh->locs[SHADER_LOC_MAP_NORMAL] = GetShaderLocation(*sh, "normalMap");
    sh->locs[SHADER_LOC_MAP_EMISSION] = GetShaderLocation(*sh, "emissiveMap");
}

/* ---------------------------------------------------------------- passes -- */

static const MyeRender3dState *render3d_state(const ecs_world_t *world)
{
    return ecs_singleton_get(world, MyeRender3dState);
}


/* Pushes light state into the shader. Lights beyond MYE_MAX_LIGHTS, and
 * disabled ones, are switched off rather than silently dropped. */
static void upload_lights(ecs_world_t *world, const MyeRender3dState *state,
                          const MyeRender3dConfig *config, Camera3D camera)
{
    /* Both shaders expose the same light uniforms, so the only difference is
     * which set of locations to write. */
    bool pbr = config->use_pbr && state->pbr_ready;
    Shader shader = pbr ? state->pbr : state->shader;
    const int *loc_dir = pbr ? state->loc_pbr_light_dir : state->loc_light_dir;
    const int *loc_color = pbr ? state->loc_pbr_light_color
                               : state->loc_light_color;
    const int *loc_enabled = pbr ? state->loc_pbr_light_enabled
                                 : state->loc_light_enabled;
    int loc_ambient = pbr ? state->loc_pbr_ambient : state->loc_ambient;
    int loc_view = pbr ? state->loc_pbr_view_pos : state->loc_view_pos;

    float ambient[4] = { (float)config->ambient.r / 255.0f,
                         (float)config->ambient.g / 255.0f,
                         (float)config->ambient.b / 255.0f, 1.0f };
    SetShaderValue(shader, loc_ambient, ambient, SHADER_UNIFORM_VEC4);

    float view_pos[3] = { camera.position.x, camera.position.y,
                          camera.position.z };
    SetShaderValue(shader, loc_view, view_pos, SHADER_UNIFORM_VEC3);

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
                SetShaderValue(shader, loc_dir[used], dir,
                               SHADER_UNIFORM_VEC3);
                SetShaderValue(shader, loc_color[used], color,
                               SHADER_UNIFORM_VEC4);
                SetShaderValue(shader, loc_enabled[used], &on,
                               SHADER_UNIFORM_INT);
                ++used;
            }
        }
    }

    int off = 0;
    for (int i = used; i < MYE_MAX_LIGHTS; ++i) {
        SetShaderValue(shader, loc_enabled[i], &off, SHADER_UNIFORM_INT);
    }
}

/* Draws the scene once, through one camera, into one viewport. */
static void draw_through(ecs_world_t *world, const MyeRender3dState *state,
                         const MyeRender3dConfig *config, Camera3D camera,
                         Rectangle viewport, MyeSurface surface,
                         MyeCameraClear clear, ecs_entity_t target)
{
    bool use_pbr = config->use_pbr && state->pbr_ready;
    if (state->shader_ready || use_pbr) {
        upload_lights(world, state, config, camera);
    }

    mye_camera_begin_3d(viewport, surface, camera, clear);

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

            /* A mesh textured with the very canvas being drawn would sample
             * the bound framebuffer. Leave it out of its own feed. */
            if (mye_canvas_is_own_texture(world, target,
                                          instances[i].texture)) {
                continue;
            }

            /* Draw each mesh with the entity's world matrix, so parenting
             * works: model.transform is the asset's own baked transform. */
            Matrix matrix = MatrixMultiply(model->transform,
                                           transforms[i].m);

            for (int m = 0; m < model->meshCount; ++m) {
                Material material = model->materials[model->meshMaterial[m]];

                if (use_pbr) {
                    material.shader = state->pbr;

                    /* glTF carries metallic and roughness as scalar factors,
                     * optionally modulated by a packed texture. Feed both:
                     * the shader multiplies them. */
                    float metallic =
                        material.maps[MATERIAL_MAP_METALNESS].value;
                    float roughness =
                        material.maps[MATERIAL_MAP_ROUGHNESS].value;
                    int has_mra =
                        material.maps[MATERIAL_MAP_METALNESS].texture.id > 0;
                    int has_emissive =
                        material.maps[MATERIAL_MAP_EMISSION].texture.id > 0;

                    SetShaderValue(state->pbr, state->loc_pbr_metallic,
                                   &metallic, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(state->pbr, state->loc_pbr_roughness,
                                   &roughness, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(state->pbr, state->loc_pbr_has_mra,
                                   &has_mra, SHADER_UNIFORM_INT);
                    SetShaderValue(state->pbr, state->loc_pbr_has_emissive,
                                   &has_emissive, SHADER_UNIFORM_INT);
                } else if (state->shader_ready) {
                    material.shader = state->shader;
                }
                if (material.maps == NULL) {
                    continue; /* malformed material: nothing to draw with */
                }

                /* PER-INSTANCE STATE, WRITTEN INTO SHARED STATE.
                 *
                 * raylib's Material carries `maps` as a POINTER, so the
                 * struct copy above still aliases the MODEL's map array --
                 * which every entity using that model draws with. Writing a
                 * tint or a texture there is therefore not per-instance at
                 * all:
                 *
                 *   - the tint compounds, because `base` is re-read from the
                 *     already-modified shared value each frame: a 50% grey
                 *     tint halves the model's colour sixty times a second
                 *     until it is black. A WHITE tint is idempotent, which is
                 *     the only reason this was never seen;
                 *   - a per-instance texture appears on every other entity
                 *     sharing the model.
                 *
                 * A local copy of the array is not an option: DrawMesh
                 * indexes every slot up to raylib's MAX_MATERIAL_MAPS, which
                 * is private to rmodels.c, so the length would be a guess.
                 * Save the two fields, draw, put them back. */
                MaterialMap *diffuse = &material.maps[MATERIAL_MAP_DIFFUSE];
                const Texture2D saved_texture = diffuse->texture;
                const Color base = diffuse->color;

                /* Per-instance texture override, if any: this is how a canvas
                 * ends up on a surface. */
                const Texture2D *override_tex =
                    mye_texture_get(world, instances[i].texture);
                if (override_tex != NULL) {
                    diffuse->texture = *override_tex;
                }

                diffuse->color = (Color){
                    (unsigned char)((int)base.r * instances[i].tint.r / 255),
                    (unsigned char)((int)base.g * instances[i].tint.g / 255),
                    (unsigned char)((int)base.b * instances[i].tint.b / 255),
                    (unsigned char)((int)base.a * instances[i].tint.a / 255),
                };

                DrawMesh(model->meshes[m], material, matrix);

                diffuse->texture = saved_texture;
                diffuse->color = base;
            }
        }
    }

    mye_camera_end_3d();
}

/* Every active camera draws, in order: a minimap is a second camera entity
 * and nothing else. One camera is the same code path, drawing once into the
 * whole window. */
/* Draws every 3D camera rendering into `target` (0 = the window), in order.
 * Called by the window's pass below and, per canvas, by the canvas module.
 *
 * `already_cleared` says the target was just cleared by its owner, so the
 * first camera can composite onto it. Later cameras clear their own viewport
 * regardless, or they draw into the previous camera's depth buffer and
 * vanish. */
void mye_render3d_draw_cameras_for(ecs_world_t *world, ecs_entity_t target,
                                   bool already_cleared)
{
    const MyeRender3dState *state = render3d_state(world);
    if (state == NULL || state->meshes == NULL) {
        return;
    }
    const MyeRender3dConfig *config = ecs_singleton_get(world,
                                                        MyeRender3dConfig);
    if (config == NULL) {
        return;
    }

    ecs_entity_t cameras[MYE_MAX_DRAWN_CAMERAS];
    int count = mye_camera3d_collect_for(world, target, cameras,
                                         MYE_MAX_DRAWN_CAMERAS);

    /* One surface for the whole pass: every camera collected here renders
     * onto the same target by construction. */
    MyeSurface surface = mye_camera_surface(world, target);

    for (int i = 0; i < count; ++i) {
        Camera3D camera;
        if (!mye_camera3d_resolve(world, cameras[i], &camera)) {
            continue;
        }
        /* The first camera composites onto whatever the target already
         * holds: a window or a clearing canvas just wiped it, and an
         * accumulating canvas is deliberately keeping last frame's picture --
         * but even then depth must be reset, or last frame's depth values
         * reject this frame's meshes. Every later camera owns its viewport
         * outright. */
        MyeCameraClear clear = MYE_CAMERA_CLEAR_ALL;
        if (i == 0) {
            clear = already_cleared ? MYE_CAMERA_CLEAR_NONE
                                    : MYE_CAMERA_CLEAR_DEPTH;
        }
        draw_through(world, state, config, camera,
                     mye_camera_viewport(world, cameras[i]), surface, clear,
                     target);
    }
}

static void MyeRender3dPass(ecs_iter_t *it)
{
    (void)ecs_field(it, MyeRender3dConfig, 0);
    mye_render3d_draw_cameras_for(it->world, 0, true);
}

/* ------------------------------------------------------------ animation -- */

/* Advances each animator and pushes the pose into its model.
 *
 * EcsPreStore, on the main thread: UpdateModelAnimation rewrites vertex
 * buffers and bone matrices, which is GPU work. */
static void MyeModelAnimate(ecs_iter_t *it)
{
    MyeModelAnimator *animators = ecs_field(it, MyeModelAnimator, 0);
    const MyeMeshInstance *instances = ecs_field(it, MyeMeshInstance, 1);
    ecs_world_t *world = it->world;
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        if (!animators[i].playing) {
            continue;
        }

        int count = 0;
        const ModelAnimation *animations =
            mye_model_animations(world, instances[i].model, &count);
        if (animations == NULL || animators[i].animation < 0 ||
            animators[i].animation >= count) {
            continue;
        }

        const ModelAnimation *anim = &animations[animators[i].animation];
        if (anim->keyframeCount <= 0) {
            continue;
        }

        animators[i].frame += animators[i].speed * dt;
        if (animators[i].frame >= (float)anim->keyframeCount) {
            if (animators[i].loop) {
                animators[i].frame = fmodf(animators[i].frame,
                                           (float)anim->keyframeCount);
            } else {
                animators[i].frame = (float)(anim->keyframeCount - 1);
                animators[i].playing = false;
            }
        }

        const Model *model = mye_model_get(world, instances[i].model);
        if (model != NULL) {
            /* raylib 6.0 takes a fractional frame and interpolates between
             * keyframes, so slow playback is smooth rather than steppy. */
            UpdateModelAnimation(*model, *anim, animators[i].frame);
        }
    }
}

/* -------------------------------------------------------------- helpers -- */

ecs_entity_t mye_mesh_spawn(ecs_world_t *world, mye_model model,
                            Vector3 position, Color tint)
{
    ecs_entity_t e = mye_spawn_3d(world, position);
    ecs_set(world, e, MyeMeshInstance, { .model = model, .tint = tint });
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
    if (state->lights != NULL) ecs_query_fini(state->lights);
    if (state->shader_ready) {
        UnloadShader(state->shader);
        state->shader_ready = false;
    }
    if (state->pbr_ready) {
        UnloadShader(state->pbr);
        state->pbr_ready = false;
    }
    state->meshes = NULL;
    state->lights = NULL;
}

void MyeRender3dModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeRender3dModule);

    ECS_COMPONENT_DEFINE(world, MyeMeshInstance);
    ECS_COMPONENT_DEFINE(world, MyeModelAnimator);
    ECS_COMPONENT_DEFINE(world, MyeCamera3D);
    ECS_COMPONENT_DEFINE(world, MyeLight);
    ECS_COMPONENT_DEFINE(world, MyeRender3dConfig);
    ECS_COMPONENT_DEFINE(world, MyeRender3dState);

    ecs_add_id(world, ecs_id(MyeRender3dConfig), EcsSingleton);
    ecs_singleton_set(world, MyeRender3dConfig,
                      { .ambient = (Color){ 60, 60, 70, 255 },
                        .use_pbr = true,
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

    /* Same vertex shader: PBR only changes the shading, not the geometry. */
    state->pbr = LoadShaderFromMemory(vertex_shader_src, pbr_fragment_src);
    state->pbr_ready = state->pbr.id != 0;
    if (state->pbr_ready) {
        resolve_pbr_uniforms(state);
    } else {
        mye_log_warn("PBR shader failed to compile; falling back to "
                     "Blinn-Phong");
    }

    ECS_SYSTEM(world, MyeModelAnimate, EcsPreStore, MyeModelAnimator,
               [in] MyeMeshInstance);
    ECS_SYSTEM(world, MyeRender3dPass, MyeOnDraw3D, MyeRender3dConfig);

    ecs_atfini(world, render3d_fini, state);
}

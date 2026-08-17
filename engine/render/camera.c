/* Camera resolution and following. See camera.h for the design. */
#include "render/camera.h"

#include "core/log.h"
#include "render/render2d.h"
#include "render/render3d.h"

#include <raymath.h>

ECS_COMPONENT_DECLARE(MyeCameraFollow);

/* Queries built once at import rather than per call: resolving the active
 * camera happens several times a frame (both passes, plus any screen/world
 * helper a game calls). */
typedef struct MyeCameraState {
    ecs_query_t *cameras3d;
    ecs_query_t *cameras2d;
    bool warned_multiple_3d;
    bool warned_multiple_2d;
} MyeCameraState;

ECS_COMPONENT_DECLARE(MyeCameraState);

/* Const in, mutable out: the queries live here and iterating one is not a
 * logical mutation of the world, but flecs has no const query handle. The
 * cast is confined to this one function rather than spread through every
 * caller. */
static MyeCameraState *camera_state(const ecs_world_t *world)
{
    const MyeCameraState *state = ecs_singleton_get(world, MyeCameraState);
    union { const MyeCameraState *in; MyeCameraState *out; } cast = { state };
    return cast.out;
}

/* ------------------------------------------------------------- resolving -- */

/* The matrix a camera is drawn with.
 *
 * Built from the entity's own position and rotation, composed through its
 * parent's *render* transform -- not read from the camera's own world
 * matrix. That matters: a follow system moves the camera during MyeOnCamera,
 * after propagation has already run, so the camera's stored matrix is one
 * frame stale while its components are current. Parents are unaffected,
 * because they were propagated before the camera moved. */
/* The camera's parent chain as drawn, or identity for a root. */
static Matrix camera_parent_matrix(const ecs_world_t *world,
                                   ecs_entity_t camera)
{
    ecs_entity_t parent = ecs_get_target(world, camera, EcsChildOf, 0);
    if (parent == 0) {
        return MatrixIdentity();
    }
    const MyeRenderTransform *r = ecs_get(world, parent, MyeRenderTransform);
    if (r != NULL) {
        return r->m;
    }
    const MyeWorldTransform *w = ecs_get(world, parent, MyeWorldTransform);
    return w != NULL ? w->m : MatrixIdentity();
}

static Matrix camera_matrix(const ecs_world_t *world, ecs_entity_t camera)
{
    Vector3 position = { 0.0f, 0.0f, 0.0f };
    Quaternion rotation = QuaternionIdentity();

    const MyePosition3D *p3 = ecs_get(world, camera, MyePosition3D);
    if (p3 != NULL) {
        position = p3->v;
        const MyeRotation3D *r3 = ecs_get(world, camera, MyeRotation3D);
        if (r3 != NULL) {
            rotation = r3->q;
        }
    } else {
        const MyePosition2D *p2 = ecs_get(world, camera, MyePosition2D);
        if (p2 != NULL) {
            position = (Vector3){ p2->x, p2->y, 0.0f };
        }
        const MyeRotation2D *r2 = ecs_get(world, camera, MyeRotation2D);
        if (r2 != NULL) {
            rotation = QuaternionFromAxisAngle((Vector3){ 0.0f, 0.0f, 1.0f },
                                               r2->angle);
        }
    }

    /* Scale is deliberately dropped: a scaled camera is meaningless, and
     * inheriting a parent's scale would silently distort the view. */
    Matrix local = mye_trs_matrix(position, rotation,
                                  (Vector3){ 1.0f, 1.0f, 1.0f });

    return MatrixMultiply(local, camera_parent_matrix(world, camera));
}

/* A camera looks down its local -Z, the OpenGL convention raylib inherits. */
static Vector3 transform_direction(Matrix m, Vector3 v)
{
    return Vector3Normalize((Vector3){
        m.m0 * v.x + m.m4 * v.y + m.m8 * v.z,
        m.m1 * v.x + m.m5 * v.y + m.m9 * v.z,
        m.m2 * v.x + m.m6 * v.y + m.m10 * v.z,
    });
}

bool mye_camera3d_resolve(const ecs_world_t *world, ecs_entity_t camera,
                          Camera3D *out)
{
    const MyeCamera3D *cam = ecs_get(world, camera, MyeCamera3D);
    if (cam == NULL || out == NULL) {
        return false;
    }

    Matrix m = camera_matrix(world, camera);
    Vector3 position = mye_matrix_translation(m);
    Vector3 forward = transform_direction(m, (Vector3){ 0.0f, 0.0f, -1.0f });
    Vector3 up = transform_direction(m, (Vector3){ 0.0f, 1.0f, 0.0f });

    out->position = position;
    out->target = Vector3Add(position, forward);
    out->up = up;
    out->fovy = cam->fov > 0.0f ? cam->fov : 60.0f;
    out->projection = cam->projection;
    return true;
}

bool mye_camera2d_resolve(const ecs_world_t *world, ecs_entity_t camera,
                          Camera2D *out)
{
    const MyeCamera2D *cam = ecs_get(world, camera, MyeCamera2D);
    if (cam == NULL || out == NULL) {
        return false;
    }

    Matrix m = camera_matrix(world, camera);
    Vector3 position = mye_matrix_translation(m);

    out->target = (Vector2){ position.x, position.y };
    out->offset = cam->offset;
    out->rotation = cam->rotation;
    /* A zero-zoom camera shows nothing, which reads as a broken renderer. */
    out->zoom = cam->zoom != 0.0f ? cam->zoom : 1.0f;
    return true;
}

/* Shared by both dimensions: first active wins, and say something the once
 * if a scene has more than one, because which one you get is otherwise
 * arbitrary and stable enough to look deliberate. */
static ecs_entity_t first_active(const ecs_world_t *world, ecs_query_t *query,
                                 bool is_3d, bool *warned)
{
    if (query == NULL) {
        return 0;
    }

    ecs_entity_t found = 0;
    int active_count = 0;

    ecs_iter_t it = ecs_query_iter(world, query);
    while (ecs_query_next(&it)) {
        for (int i = 0; i < it.count; ++i) {
            bool active = is_3d
                              ? ecs_field(&it, MyeCamera3D, 0)[i].active
                              : ecs_field(&it, MyeCamera2D, 0)[i].active;
            if (!active) {
                continue;
            }
            ++active_count;
            if (found == 0) {
                found = it.entities[i];
            }
        }
    }

    if (active_count > 1 && !*warned) {
        *warned = true;
        const char *name = ecs_get_name(world, found);
        mye_log_warn("camera: %d active %s cameras; using '%s'. Mark exactly "
                     "one active -- which one wins is otherwise arbitrary.",
                     active_count, is_3d ? "3D" : "2D",
                     name != NULL ? name : "<unnamed>");
    }
    return found;
}

bool mye_camera3d_active(const ecs_world_t *world, Camera3D *out,
                         ecs_entity_t *out_entity)
{
    MyeCameraState *state = camera_state(world);
    if (state == NULL) {
        return false;
    }
    ecs_entity_t e = first_active(world, state->cameras3d, true,
                                  &state->warned_multiple_3d);
    if (out_entity != NULL) {
        *out_entity = e;
    }
    return e != 0 && mye_camera3d_resolve(world, e, out);
}

bool mye_camera2d_active(const ecs_world_t *world, Camera2D *out,
                         ecs_entity_t *out_entity)
{
    MyeCameraState *state = camera_state(world);
    if (state == NULL) {
        return false;
    }
    ecs_entity_t e = first_active(world, state->cameras2d, false,
                                  &state->warned_multiple_2d);
    if (out_entity != NULL) {
        *out_entity = e;
    }
    return e != 0 && mye_camera2d_resolve(world, e, out);
}

/* --------------------------------------------------------------- spawning -- */

ecs_entity_t mye_camera3d_spawn(ecs_world_t *world, Vector3 position,
                                Vector3 look_at, float fov_degrees)
{
    ecs_entity_t e = mye_spawn_3d(world, position);
    ecs_set(world, e, MyeCamera3D,
            { .fov = fov_degrees > 0.0f ? fov_degrees : 60.0f,
              .projection = CAMERA_PERSPECTIVE,
              .active = true });
    mye_camera_look_at(world, e, look_at);
    return e;
}

ecs_entity_t mye_camera2d_spawn(ecs_world_t *world, Vector2 position,
                                float zoom)
{
    const mye_engine *engine = mye_engine_get(world);
    Vector2 centre = { 0.0f, 0.0f };
    if (engine != NULL) {
        centre = (Vector2){ (float)engine->width * 0.5f,
                            (float)engine->height * 0.5f };
    }

    ecs_entity_t e = mye_spawn_2d(world, position);
    ecs_set(world, e, MyeCamera2D,
            { .zoom = zoom != 0.0f ? zoom : 1.0f,
              .rotation = 0.0f,
              /* Centre by default: the camera's position is then what ends
               * up in the middle of the window, which is what "the camera is
               * at the player" is expected to mean. */
              .offset = centre,
              .active = true });
    return e;
}

/* ---------------------------------------------------------------- aiming -- */

void mye_camera_look_at(ecs_world_t *world, ecs_entity_t camera,
                        Vector3 target)
{
    const MyePosition3D *p = ecs_get(world, camera, MyePosition3D);
    if (p == NULL) {
        mye_log_warn("camera: look_at on an entity with no MyePosition3D");
        return;
    }

    /* `target` is a WORLD point, but MyePosition3D and MyeRotation3D are in
     * the parent's space. Bring the target into that space, or a parented
     * camera would aim using two different coordinate systems and point at
     * nothing. For a root camera the parent matrix is identity and this
     * costs one no-op transform. */
    Matrix to_parent = MatrixInvert(camera_parent_matrix(world, camera));
    target = Vector3Transform(target, to_parent);

    Vector3 forward = Vector3Subtract(target, p->v);
    if (Vector3LengthSqr(forward) <= 1e-12f) {
        return; /* looking at itself: leave the rotation alone */
    }
    forward = Vector3Normalize(forward);

    /* MatrixLookAt builds a view matrix (world -> camera). Its inverse is the
     * camera's orientation in the world, which is what the rotation must be. */
    Vector3 up = { 0.0f, 1.0f, 0.0f };
    if (fabsf(Vector3DotProduct(forward, up)) > 0.999f) {
        up = (Vector3){ 0.0f, 0.0f, 1.0f }; /* straight up or down */
    }
    Matrix view = MatrixLookAt(p->v, target, up);
    Quaternion rotation = QuaternionFromMatrix(MatrixInvert(view));

    ecs_set(world, camera, MyeRotation3D, { rotation });
}

void mye_camera_set_fov(ecs_world_t *world, ecs_entity_t camera,
                        float degrees)
{
    MyeCamera3D *cam = ecs_get_mut(world, camera, MyeCamera3D);
    if (cam == NULL) {
        return;
    }
    /* Clamped rather than trusted: 0 renders nothing and 180 turns the
     * projection inside out, and both look like an engine bug. */
    if (degrees < 1.0f) degrees = 1.0f;
    if (degrees > 179.0f) degrees = 179.0f;
    cam->fov = degrees;
    ecs_modified(world, camera, MyeCamera3D);
}

float mye_camera_get_fov(const ecs_world_t *world, ecs_entity_t camera)
{
    const MyeCamera3D *cam = ecs_get(world, camera, MyeCamera3D);
    return cam != NULL ? cam->fov : 0.0f;
}

/* ------------------------------------------------- screen and world space -- */

/* raylib's non-Ex forms read the live window, so they cannot run headless.
 * The Ex forms take the size explicitly, which is what makes picking
 * testable without a window. */
static void screen_size(const ecs_world_t *world, int *width, int *height)
{
    const mye_engine *engine = mye_engine_get(world);
    *width = engine != NULL && engine->width > 0 ? engine->width : 1280;
    *height = engine != NULL && engine->height > 0 ? engine->height : 720;
}

Vector2 mye_world_to_screen(const ecs_world_t *world, Vector3 point)
{
    Camera3D camera;
    if (!mye_camera3d_active(world, &camera, NULL)) {
        return (Vector2){ 0.0f, 0.0f };
    }
    int width, height;
    screen_size(world, &width, &height);
    return GetWorldToScreenEx(point, camera, width, height);
}

Ray mye_screen_ray(const ecs_world_t *world, Vector2 screen)
{
    Camera3D camera;
    if (!mye_camera3d_active(world, &camera, NULL)) {
        return (Ray){ 0 };
    }
    int width, height;
    screen_size(world, &width, &height);
    return GetScreenToWorldRayEx(screen, camera, width, height);
}

Vector2 mye_world_to_screen_2d(const ecs_world_t *world, Vector2 point)
{
    Camera2D camera;
    if (!mye_camera2d_active(world, &camera, NULL)) {
        return point; /* no camera: world space is screen space */
    }
    return GetWorldToScreen2D(point, camera);
}

Vector2 mye_screen_to_world_2d(const ecs_world_t *world, Vector2 screen)
{
    Camera2D camera;
    if (!mye_camera2d_active(world, &camera, NULL)) {
        return screen;
    }
    return GetScreenToWorld2D(screen, camera);
}

/* ------------------------------------------------------------- following -- */

static void MyeCameraFollowUpdate(ecs_iter_t *it)
{
    MyeCameraFollow *follow = ecs_field(it, MyeCameraFollow, 0);
    ecs_world_t *world = it->world;
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        ecs_entity_t target = follow[i].target;
        if (target == 0 || !ecs_is_alive(world, target)) {
            continue; /* target gone: hold position rather than snap to origin */
        }

        /* The DRAWN position, not the simulated one. A camera that followed
         * the simulated position would trail the picture by up to a step,
         * which reads as the world sliding under the player. */
        Vector3 target_pos = mye_render_position(world, target);
        Quaternion target_rot = mye_render_rotation(world, target);
        Vector3 desired = Vector3Add(
            target_pos, Vector3RotateByQuaternion(follow[i].offset, target_rot));

        ecs_entity_t camera = it->entities[i];
        MyePosition3D *p3 = ecs_get_mut(world, camera, MyePosition3D);
        MyePosition2D *p2 =
            p3 == NULL ? ecs_get_mut(world, camera, MyePosition2D) : NULL;
        if (p3 == NULL && p2 == NULL) {
            continue;
        }

        Vector3 current = p3 != NULL ? p3->v
                                     : (Vector3){ p2->x, p2->y, 0.0f };

        /* Exponential decay rather than a fixed fraction per frame, so the
         * lag looks the same at 30 and 240 fps. */
        float t = follow[i].stiffness > 0.0f
                      ? 1.0f - expf(-follow[i].stiffness * dt)
                      : 1.0f;
        Vector3 next = Vector3Lerp(current, desired, t);

        if (p3 != NULL) {
            p3->v = next;
            ecs_modified(world, camera, MyePosition3D);
        } else {
            p2->x = next.x;
            p2->y = next.y;
            ecs_modified(world, camera, MyePosition2D);
        }
    }
}

/* ------------------------------------------------------------- lifecycle -- */

static void camera_state_fini(ecs_world_t *world, void *ctx)
{
    (void)world;
    MyeCameraState *state = (MyeCameraState *)ctx;
    if (state->cameras3d != NULL) ecs_query_fini(state->cameras3d);
    if (state->cameras2d != NULL) ecs_query_fini(state->cameras2d);
}

void MyeCameraModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeCameraModule);

    ECS_COMPONENT_DEFINE(world, MyeCameraFollow);
    ECS_COMPONENT_DEFINE(world, MyeCameraState);
    ecs_add_id(world, ecs_id(MyeCameraState), EcsSingleton);

    ecs_singleton_set(world, MyeCameraState, { 0 });
    MyeCameraState *state = ecs_singleton_ensure(world, MyeCameraState);
    state->cameras3d = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyeCamera3D), .inout = EcsIn }},
    });
    state->cameras2d = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyeCamera2D), .inout = EcsIn }},
    });

    ECS_SYSTEM(world, MyeCameraFollowUpdate, MyeOnCamera, MyeCameraFollow);

    ecs_atfini(world, camera_state_fini, state);
}

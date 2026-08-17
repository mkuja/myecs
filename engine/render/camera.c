/* Camera resolution and following. See camera.h for the design. */
#include "render/camera.h"

#include "core/log.h"
#include "render/canvas.h"
#include "render/render2d.h"
#include "render/render3d.h"

#include <raymath.h>
#include <rlgl.h>

ECS_COMPONENT_DECLARE(MyeCameraFollow);

/* Queries built once at import rather than per call: resolving the active
 * camera happens several times a frame (both passes, plus any screen/world
 * helper a game calls). */
typedef struct MyeCameraState {
    ecs_query_t *cameras3d;
    ecs_query_t *cameras2d;
    bool warned_bare_parent;
    bool warned_too_many;
    bool warned_bad_target;
} MyeCameraState;

ECS_COMPONENT_DECLARE(MyeCameraState);

static void screen_size(const ecs_world_t *world, int *width, int *height);

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
    if (w != NULL) {
        return w->m;
    }

    /* The parent has a position but no transform matrices (a mye_sprite_spawn
     * entity, say), so the camera cannot follow it and sits at the origin.
     * The sprite pass has a warning for this shape; a camera has no sprite,
     * so it needs its own. */
    MyeCameraState *state = camera_state(world);
    if (state != NULL && !state->warned_bare_parent) {
        state->warned_bare_parent = true;
        const char *name = ecs_get_name(world, parent);
        mye_log_warn("camera: parented to '%s', which has no MyeLocalTransform/"
                     "MyeWorldTransform, so the camera cannot be carried by it "
                     "and sits at the origin. Spawn the parent with "
                     "mye_spawn_2d/3d, or add both components.",
                     name != NULL ? name : "<unnamed>");
    }
    return MatrixIdentity();
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

    /* The camera's own scale is dropped: a scaled camera is meaningless. A
     * parent's scale is kept, because it places the camera correctly (a
     * child at (0,0,10) of a x2 parent really is 20 away); resolve
     * normalises the axes it extracts, so it cannot distort the view. */
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
    /* The view turns with the entity: a camera rotated by theta shows the
     * world rotated by -theta, in raylib's degrees. Read from the matrix so
     * a parent's rotation is included -- there is no second rotation field
     * on the component to disagree with this. */
    out->rotation = -atan2f(m.m1, m.m0) * RAD2DEG;
    /* A zero-zoom camera shows nothing, which reads as a broken renderer. */
    out->zoom = cam->zoom != 0.0f ? cam->zoom : 1.0f;
    return true;
}

/* The draw order of a camera, whichever dimensionality it is. */
static int camera_order(const ecs_world_t *world, ecs_entity_t camera,
                        bool is_3d)
{
    if (is_3d) {
        const MyeCamera3D *c = ecs_get(world, camera, MyeCamera3D);
        return c != NULL ? c->order : 0;
    }
    const MyeCamera2D *c = ecs_get(world, camera, MyeCamera2D);
    return c != NULL ? c->order : 0;
}

/* Every active camera, sorted by order. Insertion sort: the count is a
 * handful, and it is stable, so equal orders keep entity order and the
 * picture does not shuffle between frames. */
static int collect(const ecs_world_t *world, ecs_query_t *query, bool is_3d,
                   ecs_entity_t target, ecs_entity_t *out, int max)
{
    if (query == NULL || out == NULL || max <= 0) {
        return 0;
    }

    int count = 0;
    bool clipped = false;

    ecs_iter_t it = ecs_query_iter(world, query);
    while (ecs_query_next(&it)) {
        for (int i = 0; i < it.count; ++i) {
            bool active;
            ecs_entity_t cam_target;
            if (is_3d) {
                const MyeCamera3D *c = &ecs_field(&it, MyeCamera3D, 0)[i];
                active = c->active;
                cam_target = c->target;
            } else {
                const MyeCamera2D *c = &ecs_field(&it, MyeCamera2D, 0)[i];
                active = c->active;
                cam_target = c->target;
            }
            if (!active) {
                continue;
            }
            /* A target that is not a live canvas is malformed data, not a
             * choice: fall back to the window and say so once. */
            if (cam_target != 0 &&
                (!ecs_is_alive(world, cam_target) ||
                 !ecs_has(world, cam_target, MyeCanvas))) {
                MyeCameraState *st = camera_state(world);
                if (st != NULL && !st->warned_bad_target) {
                    st->warned_bad_target = true;
                    mye_log_warn("camera: render target %llu is not a live "
                                 "canvas; drawing to the window instead",
                                 (unsigned long long)cam_target);
                }
                cam_target = 0;
            }
            if (cam_target != target) {
                continue;
            }
            if (count >= max) {
                clipped = true;
                continue;
            }

            out[count] = it.entities[i];
            ++count;
        }
    }

    /* Sorted after the iteration, not during it: ordering needs ecs_get on
     * other entities, and doing that inside an open query iteration is what
     * broke this the first time. */
    for (int i = 1; i < count; ++i) {
        ecs_entity_t moving = out[i];
        int order = camera_order(world, moving, is_3d);
        int at = i;
        while (at > 0 && camera_order(world, out[at - 1], is_3d) > order) {
            out[at] = out[at - 1];
            --at;
        }
        out[at] = moving;
    }

    if (clipped) {
        MyeCameraState *state = camera_state(world);
        if (state != NULL && !state->warned_too_many) {
            state->warned_too_many = true;
            mye_log_warn("camera: more than %d active cameras; the rest are "
                         "not drawn", max);
        }
    }
    return count;
}

int mye_camera3d_collect_for(const ecs_world_t *world, ecs_entity_t target,
                             ecs_entity_t *out, int max)
{
    MyeCameraState *state = camera_state(world);
    return state != NULL
               ? collect(world, state->cameras3d, true, target, out, max)
               : 0;
}

int mye_camera2d_collect_for(const ecs_world_t *world, ecs_entity_t target,
                             ecs_entity_t *out, int max)
{
    MyeCameraState *state = camera_state(world);
    return state != NULL
               ? collect(world, state->cameras2d, false, target, out, max)
               : 0;
}

int mye_camera3d_collect(const ecs_world_t *world, ecs_entity_t *out, int max)
{
    return mye_camera3d_collect_for(world, 0, out, max);
}

int mye_camera2d_collect(const ecs_world_t *world, ecs_entity_t *out, int max)
{
    return mye_camera2d_collect_for(world, 0, out, max);
}

Rectangle mye_camera_viewport(const ecs_world_t *world, ecs_entity_t camera)
{
    Rectangle vp = { 0 };
    const MyeCamera3D *c3 = ecs_get(world, camera, MyeCamera3D);
    const MyeCamera2D *c2 = c3 == NULL ? ecs_get(world, camera, MyeCamera2D)
                                       : NULL;
    if (c3 != NULL) {
        vp = c3->viewport;
    } else if (c2 != NULL) {
        vp = c2->viewport;
    }

    if (vp.width <= 0.0f || vp.height <= 0.0f) {
        /* Zero means "the whole target", and the target may be a canvas
         * rather than the window. */
        ecs_entity_t target = c3 != NULL ? c3->target
                                         : (c2 != NULL ? c2->target : 0);
        int width, height;
        /* ecs_is_alive FIRST: a camera keeps pointing at a canvas that has
         * been destroyed -- collect() buckets it back onto the window and
         * warns, but does not rewrite the game's component -- and flecs
         * aborts on ecs_has for a dead entity in debug, dereferences NULL in
         * release. The fallback collect() documents has to survive here too,
         * because this is the very next thing the pass calls. */
        if (target != 0 && ecs_is_alive(world, target) &&
            ecs_has(world, target, MyeCanvas)) {
            const MyeCanvas *canvas = ecs_get(world, target, MyeCanvas);
            width = canvas->width;
            height = canvas->height;
        } else {
            screen_size(world, &width, &height);
        }
        vp = (Rectangle){ 0.0f, 0.0f, (float)width, (float)height };
    }
    return vp;
}

MyeSurface mye_camera_surface(const ecs_world_t *world, ecs_entity_t target)
{
    if (target != 0 && ecs_is_alive(world, target) &&
        ecs_has(world, target, MyeCanvas)) {
        const MyeCanvas *canvas = ecs_get(world, target, MyeCanvas);
        if (canvas != NULL) {
            return (MyeSurface){ canvas->width, canvas->height };
        }
    }
    /* The window, at its CURRENT size. GetRenderWidth/Height track resizes
     * and HiDPI; the configured size is the headless fallback, where nothing
     * draws anyway. */
    if (IsWindowReady()) {
        return (MyeSurface){ GetRenderWidth(), GetRenderHeight() };
    }
    int width, height;
    screen_size(world, &width, &height);
    return (MyeSurface){ width, height };
}

ecs_entity_t mye_camera_at_screen(const ecs_world_t *world, Vector2 screen)
{
    /* Highest order first: the topmost camera at that pixel is the one the
     * player thinks they clicked on. */
    ecs_entity_t cameras[MYE_MAX_DRAWN_CAMERAS];
    int count = mye_camera2d_collect(world, cameras, MYE_MAX_DRAWN_CAMERAS);
    for (int i = count - 1; i >= 0; --i) {
        if (CheckCollisionPointRec(screen,
                                   mye_camera_viewport(world, cameras[i]))) {
            return cameras[i];
        }
    }
    count = mye_camera3d_collect(world, cameras, MYE_MAX_DRAWN_CAMERAS);
    for (int i = count - 1; i >= 0; --i) {
        if (CheckCollisionPointRec(screen,
                                   mye_camera_viewport(world, cameras[i]))) {
            return cameras[i];
        }
    }
    return 0;
}

bool mye_camera3d_active(const ecs_world_t *world, Camera3D *out,
                         ecs_entity_t *out_entity)
{
    MyeCameraState *state = camera_state(world);
    if (state == NULL) {
        return false;
    }
    (void)state;
    ecs_entity_t cameras[MYE_MAX_DRAWN_CAMERAS];
    int count = mye_camera3d_collect(world, cameras, MYE_MAX_DRAWN_CAMERAS);
    ecs_entity_t e = count > 0 ? cameras[0] : 0;
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
    (void)state;
    ecs_entity_t cameras[MYE_MAX_DRAWN_CAMERAS];
    int count = mye_camera2d_collect(world, cameras, MYE_MAX_DRAWN_CAMERAS);
    ecs_entity_t e = count > 0 ? cameras[0] : 0;
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
    /* A view matrix is world->camera; its transpose (an orthonormal
     * matrix's inverse) is the camera's orientation in the world. */
    Quaternion rotation = QuaternionFromMatrix(MatrixTranspose(view));

    /* Written in place rather than ecs_set: a deferred write from a system
     * lands at the next merge, which under worker threads is after the draw
     * passes -- the camera would aim one frame late. */
    MyeRotation3D *rot = ecs_ensure(world, camera, MyeRotation3D);
    rot->q = rotation;
    ecs_modified(world, camera, MyeRotation3D);
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

/* ---------------------------------------------------- drawing through one -- */

/* GL's viewport origin is bottom-left; ours is top-left, like everything
 * else on screen. */
/* The surface the camera currently being drawn sits on -- the window, or a
 * canvas. Recorded here so the matching restore does not have to be told
 * twice, and so that NEITHER depends on rlgl's idea of the framebuffer size:
 * raylib writes that in BeginTextureMode and nowhere else, so it is stale
 * after a window resize and after every EndTextureMode. Drawing is main
 * thread only, between one begin and its end, so a static is the whole of
 * the required lifetime. */
static int surface_width = 0;
static int surface_height = 0;

static void set_viewport(Rectangle vp, int width, int height)
{
    surface_width = width;
    surface_height = height;

    /* The CURRENT surface's height, not the window's: inside a canvas they
     * differ, and flipping against the window there puts the viewport off
     * the edge of a smaller canvas -- the camera draws nothing, with no
     * error anywhere. */
    int x = (int)vp.x;
    int y = height - (int)(vp.y + vp.height);
    int w = (int)vp.width;
    int h = (int)vp.height;

    rlViewport(x, y, w, h);
    /* Scissor as well: the viewport confines the projection, not the pixels
     * a clear or an oversized sprite can touch. */
    rlEnableScissorTest();
    rlScissor(x, y, w, h);
}

static void restore_viewport(void)
{
    rlDisableScissorTest();
    rlViewport(0, 0, surface_width, surface_height);
}

void mye_camera_begin_3d(Rectangle viewport, MyeSurface surface,
                         Camera3D camera, MyeCameraClear clear)
{
    rlDrawRenderBatchActive();
    set_viewport(viewport, surface.width, surface.height);

    /* glClear obeys the scissor box, so this clears the viewport only.
     *
     * Clearing colour as well makes a viewport camera opaque: a minimap
     * covers what is under it rather than blending into it, and two cameras
     * sharing a rect paint over each other, newest last. An accumulating
     * canvas is the case that needs the two separated -- see
     * MyeCameraClear. */
    if (clear == MYE_CAMERA_CLEAR_DEPTH) {
        rlColorMask(false, false, false, false);
        rlClearScreenBuffers();
        rlColorMask(true, true, true, true);
    } else if (clear == MYE_CAMERA_CLEAR_ALL) {
        rlClearScreenBuffers();
    }

    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlLoadIdentity();

    /* The viewport's aspect, not the window's -- this is the whole reason
     * raylib's BeginMode3D cannot be used for a sub-rect. */
    double aspect = (double)viewport.width / (double)viewport.height;
    double near_plane = rlGetCullDistanceNear();
    double far_plane = rlGetCullDistanceFar();

    double fovy = (double)camera.fovy;
    if (camera.projection == CAMERA_ORTHOGRAPHIC) {
        double top = fovy / 2.0;
        rlOrtho(-top * aspect, top * aspect, -top, top, near_plane, far_plane);
    } else {
        double top = near_plane * tan(fovy * 0.5 * (double)DEG2RAD);
        rlFrustum(-top * aspect, top * aspect, -top, top, near_plane,
                  far_plane);
    }

    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(
        MatrixLookAt(camera.position, camera.target, camera.up)));

    rlEnableDepthTest();
}

void mye_camera_end_3d(void)
{
    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION);
    rlPopMatrix();
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlDisableDepthTest();
    restore_viewport();
}

void mye_camera_begin_2d(Rectangle viewport, MyeSurface surface,
                         Camera2D camera)
{
    rlDrawRenderBatchActive();
    set_viewport(viewport, surface.width, surface.height);

    /* raylib's screen-space projection covers the whole window, so with the
     * GL viewport narrowed the whole window would be squeezed into the rect.
     * Project the rect's own pixel extent instead, which also makes the
     * camera's offset mean "within this viewport". */
    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlLoadIdentity();
    rlOrtho(0.0, (double)viewport.width, (double)viewport.height, 0.0, -1.0,
            1.0);

    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(GetCameraMatrix2D(camera)));
}

void mye_camera_end_2d(void)
{
    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION);
    rlPopMatrix();
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    restore_viewport();
}

/* ------------------------------------------------------------- following -- */

static void MyeCameraFollowUpdate(ecs_iter_t *it)
{
    MyeCameraFollow *follow = ecs_field(it, MyeCameraFollow, 0);
    ecs_world_t *world = it->world;
    float dt = (float)it->delta_time;

    for (int i = 0; i < it->count; ++i) {
        ecs_entity_t camera = it->entities[i];
        ecs_entity_t target = follow[i].target;
        if (target == 0 || !ecs_is_alive(world, target)) {
            /* The target is gone. Do nothing: the camera stays exactly where
             * it was, as if nothing had happened. What a dead target MEANS
             * -- hold, pan to the wreck, cut to a respawn point -- is the
             * game's decision to make, and it makes it by writing the camera
             * or the follow component. The engine takes no stance, and does
             * not warn, because there is nothing wrong. */
            continue;
        }

        /* The DRAWN position, not the simulated one. A camera that followed
         * the simulated position would trail the picture by up to a step,
         * which reads as the world sliding under the player. */
        Vector3 target_pos = mye_render_position(world, target);
        Quaternion target_rot = mye_render_rotation(world, target);
        Vector3 desired = Vector3Add(
            target_pos, Vector3RotateByQuaternion(follow[i].offset, target_rot));

        /* `desired` is a world point; the camera's position is in its
         * parent's space. Convert, or a following camera that is also
         * parented lands at parent + world instead of at the target. */
        desired = Vector3Transform(
            desired, MatrixInvert(camera_parent_matrix(world, camera)));

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

    /* The queries below name MyeCamera2D and MyeCamera3D, which the
     * renderers own. Import them here rather than trusting the caller's
     * order: imported too early, every query silently matched nothing and
     * the 3D pass drew an empty screen with no error. Re-import is a no-op
     * when the engine has already done it. */
    ECS_IMPORT(world, MyeRender2dModule);
    ECS_IMPORT(world, MyeRender3dModule);

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

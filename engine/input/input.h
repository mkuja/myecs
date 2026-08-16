/* Action-based input. See plan/00-overview.md (Tier 1: input mapping).
 *
 * Gameplay code never mentions a key code. It asks whether an *action* is
 * down -- "thrust", "fire" -- and the bindings decide what produces it.
 * That is what makes rebinding, gamepads, and replay possible later.
 *
 *   enum { ACT_THRUST, ACT_FIRE, ACT_TURN };
 *   mye_input_bind_key(world, ACT_THRUST, KEY_UP);
 *   mye_input_bind_axis_keys(world, ACT_TURN, KEY_LEFT, KEY_RIGHT);
 *   ...
 *   if (mye_action_down(world, ACT_THRUST)) { ... }
 *
 * The state machine (edges, axis accumulation) is separated from raylib
 * polling so it can be unit tested headlessly -- see tests/unit/test_input.c.
 */
#ifndef MYE_INPUT_INPUT_H
#define MYE_INPUT_INPUT_H

#include "core/engine.h"

#define MYE_MAX_ACTIONS 32
#define MYE_MAX_BINDINGS 128

typedef enum mye_binding_kind {
    MYE_BIND_KEY = 0,          /* raylib KEY_* */
    MYE_BIND_MOUSE_BUTTON,     /* raylib MOUSE_BUTTON_* */
    MYE_BIND_GAMEPAD_BUTTON,   /* raylib GAMEPAD_BUTTON_* */
    MYE_BIND_GAMEPAD_AXIS,     /* raylib GAMEPAD_AXIS_* */
} mye_binding_kind;

typedef struct mye_binding {
    mye_binding_kind kind;
    int code;
    int gamepad;  /* gamepad index, for the gamepad kinds */
    float scale;  /* contribution to the action's value; -1 inverts an axis */
} mye_binding;

/* Per-action state for the current frame. */
typedef struct MyeInput {
    mye_binding bindings[MYE_MAX_BINDINGS];
    int32_t binding_action[MYE_MAX_BINDINGS]; /* which action each drives */
    int32_t binding_count;

    float value[MYE_MAX_ACTIONS];    /* analog, clamped to [-1, 1] */
    bool down[MYE_MAX_ACTIONS];      /* held this frame */
    bool prev_down[MYE_MAX_ACTIONS]; /* held last frame */
    bool pressed[MYE_MAX_ACTIONS];   /* went down this frame */
    bool released[MYE_MAX_ACTIONS];  /* came up this frame */

    /* When set, the raylib polling system leaves the state alone so tests (or
     * a replay system) can drive input themselves. */
    bool synthetic;
} MyeInput;

extern ECS_COMPONENT_DECLARE(MyeInput);

void MyeInputModuleImport(ecs_world_t *world);

/* Samples raylib and updates the action state. Called by mye_progress();
 * games do not call this directly. Does nothing when MyeInput.synthetic is
 * set, which is how tests and replays take over. */
void mye_input_poll(ecs_world_t *world);

/* ------------------------------------------------------------- bindings -- */

/* All return false if the action index is out of range or the binding table
 * is full. Several bindings may drive the same action. */
bool mye_input_bind_key(ecs_world_t *world, int action, int key);
bool mye_input_bind_mouse_button(ecs_world_t *world, int action, int button);
bool mye_input_bind_gamepad_button(ecs_world_t *world, int action, int gamepad,
                                   int button);
bool mye_input_bind_gamepad_axis(ecs_world_t *world, int action, int gamepad,
                                 int axis, float scale);
/* Two keys forming a -1 / +1 axis, e.g. left and right arrows. */
bool mye_input_bind_axis_keys(ecs_world_t *world, int action, int negative_key,
                              int positive_key);
void mye_input_clear_bindings(ecs_world_t *world);

/* ---------------------------------------------------------------- query -- */

bool mye_action_down(const ecs_world_t *world, int action);
bool mye_action_pressed(const ecs_world_t *world, int action);
bool mye_action_released(const ecs_world_t *world, int action);
float mye_action_value(const ecs_world_t *world, int action);

/* ------------------------------------------------------- state machine -- */

/* Used by the polling system, and directly by tests. Call in this order:
 *   frame_begin -> apply (any number of times) -> frame_end            */
void mye_input_frame_begin(MyeInput *input);
/* Contributes to one action. `down` ors into the held state; `value` adds to
 * the analog value (clamped at frame_end). */
void mye_input_apply(MyeInput *input, int action, bool down, float value);
void mye_input_frame_end(MyeInput *input);

#endif /* MYE_INPUT_INPUT_H */

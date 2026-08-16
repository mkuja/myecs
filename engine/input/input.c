#include "input/input.h"

#include <raylib.h>

#include <string.h>

ECS_COMPONENT_DECLARE(MyeInput);

static bool action_in_range(int action)
{
    return action >= 0 && action < MYE_MAX_ACTIONS;
}

/* ------------------------------------------------------- state machine -- */

void mye_input_frame_begin(MyeInput *input)
{
    memcpy(input->prev_down, input->down, sizeof input->down);
    memset(input->down, 0, sizeof input->down);
    memset(input->value, 0, sizeof input->value);
}

void mye_input_apply(MyeInput *input, int action, bool down, float value)
{
    if (!action_in_range(action)) {
        return;
    }
    /* Several bindings can drive one action: held wins, values sum. */
    input->down[action] = input->down[action] || down;
    input->value[action] += value;
}

void mye_input_frame_end(MyeInput *input)
{
    for (int i = 0; i < MYE_MAX_ACTIONS; ++i) {
        if (input->value[i] > 1.0f) input->value[i] = 1.0f;
        if (input->value[i] < -1.0f) input->value[i] = -1.0f;

        input->pressed[i] = input->down[i] && !input->prev_down[i];
        input->released[i] = !input->down[i] && input->prev_down[i];
    }
}

/* ------------------------------------------------------------- bindings -- */

static bool add_binding(ecs_world_t *world, int action, mye_binding binding)
{
    if (!action_in_range(action)) {
        return false;
    }
    MyeInput *input = ecs_singleton_ensure(world, MyeInput);
    if (input == NULL || input->binding_count >= MYE_MAX_BINDINGS) {
        return false;
    }

    int32_t slot = input->binding_count;
    input->bindings[slot] = binding;
    input->binding_action[slot] = action;
    input->binding_count = slot + 1;
    ecs_singleton_modified(world, MyeInput);
    return true;
}

bool mye_input_bind_key(ecs_world_t *world, int action, int key)
{
    return add_binding(world, action,
                       (mye_binding){ .kind = MYE_BIND_KEY,
                                      .code = key,
                                      .scale = 1.0f });
}

bool mye_input_bind_mouse_button(ecs_world_t *world, int action, int button)
{
    return add_binding(world, action,
                       (mye_binding){ .kind = MYE_BIND_MOUSE_BUTTON,
                                      .code = button,
                                      .scale = 1.0f });
}

bool mye_input_bind_gamepad_button(ecs_world_t *world, int action, int gamepad,
                                   int button)
{
    return add_binding(world, action,
                       (mye_binding){ .kind = MYE_BIND_GAMEPAD_BUTTON,
                                      .code = button,
                                      .gamepad = gamepad,
                                      .scale = 1.0f });
}

bool mye_input_bind_gamepad_axis(ecs_world_t *world, int action, int gamepad,
                                 int axis, float scale)
{
    return add_binding(world, action,
                       (mye_binding){ .kind = MYE_BIND_GAMEPAD_AXIS,
                                      .code = axis,
                                      .gamepad = gamepad,
                                      .scale = scale });
}

bool mye_input_bind_axis_keys(ecs_world_t *world, int action, int negative_key,
                              int positive_key)
{
    bool ok = add_binding(world, action,
                          (mye_binding){ .kind = MYE_BIND_KEY,
                                         .code = negative_key,
                                         .scale = -1.0f });
    ok = add_binding(world, action,
                     (mye_binding){ .kind = MYE_BIND_KEY,
                                    .code = positive_key,
                                    .scale = 1.0f }) &&
         ok;
    return ok;
}

void mye_input_clear_bindings(ecs_world_t *world)
{
    MyeInput *input = ecs_singleton_ensure(world, MyeInput);
    if (input != NULL) {
        input->binding_count = 0;
        ecs_singleton_modified(world, MyeInput);
    }
}

/* ---------------------------------------------------------------- query -- */

static const MyeInput *input_get(const ecs_world_t *world)
{
    return ecs_singleton_get(world, MyeInput);
}

bool mye_action_down(const ecs_world_t *world, int action)
{
    const MyeInput *input = input_get(world);
    return input != NULL && action_in_range(action) && input->down[action];
}

bool mye_action_pressed(const ecs_world_t *world, int action)
{
    const MyeInput *input = input_get(world);
    return input != NULL && action_in_range(action) && input->pressed[action];
}

bool mye_action_released(const ecs_world_t *world, int action)
{
    const MyeInput *input = input_get(world);
    return input != NULL && action_in_range(action) && input->released[action];
}

float mye_action_value(const ecs_world_t *world, int action)
{
    const MyeInput *input = input_get(world);
    if (input == NULL || !action_in_range(action)) {
        return 0.0f;
    }
    return input->value[action];
}

/* --------------------------------------------------------- raylib poll -- */

/* The only place raylib input is read. Everything above is pure state. */
static void sample_binding(MyeInput *input, const mye_binding *binding,
                           int action)
{
    switch (binding->kind) {
    case MYE_BIND_KEY: {
        bool down = IsKeyDown(binding->code);
        mye_input_apply(input, action, down, down ? binding->scale : 0.0f);
        break;
    }
    case MYE_BIND_MOUSE_BUTTON: {
        bool down = IsMouseButtonDown(binding->code);
        mye_input_apply(input, action, down, down ? binding->scale : 0.0f);
        break;
    }
    case MYE_BIND_GAMEPAD_BUTTON: {
        bool down = IsGamepadAvailable(binding->gamepad) &&
                    IsGamepadButtonDown(binding->gamepad, binding->code);
        mye_input_apply(input, action, down, down ? binding->scale : 0.0f);
        break;
    }
    case MYE_BIND_GAMEPAD_AXIS: {
        if (!IsGamepadAvailable(binding->gamepad)) {
            break;
        }
        float raw = GetGamepadAxisMovement(binding->gamepad, binding->code);
        float scaled = raw * binding->scale;
        /* Deadzone, so stick drift does not read as held. */
        bool down = scaled > 0.25f || scaled < -0.25f;
        mye_input_apply(input, action, down, down ? scaled : 0.0f);
        break;
    }
    }
}

/* Called by mye_progress() at the top of the frame, before fixed-timestep
 * systems run -- not as a system, because those steps happen outside the
 * pipeline and still need this frame's input. */
void mye_input_poll(ecs_world_t *world)
{
    MyeInput *input = ecs_singleton_ensure(world, MyeInput);
    if (input == NULL || input->synthetic) {
        return; /* absent, or driven by a test or replay instead */
    }

    mye_input_frame_begin(input);
    for (int32_t i = 0; i < input->binding_count; ++i) {
        sample_binding(input, &input->bindings[i],
                       (int)input->binding_action[i]);
    }
    mye_input_frame_end(input);
    ecs_singleton_modified(world, MyeInput);
}

void MyeInputModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeInputModule);

    ECS_COMPONENT_DEFINE(world, MyeInput);
    ecs_add_id(world, ecs_id(MyeInput), EcsSingleton);
    ecs_singleton_set(world, MyeInput, { 0 });
}

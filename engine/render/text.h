/* Text rendering. See plan/03-rendering.md ("2D pipeline") and
 * plan/06-assets.md.
 *
 * A text entity is an ordinary 2D entity: it takes its place from
 * MyePosition2D exactly as a sprite does, and can be parented, hidden with
 * MyeHidden, or driven by anything that moves a transform.
 *
 *   ecs_entity_t score = mye_text_spawn(world, "SCORE 0", 16, 16, RAYWHITE);
 *   mye_text_set(world, score, TextFormat("SCORE %d", points));
 *
 * WHERE IT DRAWS. Screen space by default, in the MyeOnDrawUI phase. That is
 * where a HUD belongs: screen space is what makes "16 pixels from the corner"
 * mean sixteen pixels whatever the camera is doing. Setting `world_space`
 * moves the entity into the world-space pass instead, where it scrolls and
 * scales with the 2D camera -- a label over a unit, a floating damage number.
 * It is a flag you set, never something the engine infers from context: text
 * that silently changed coordinate systems because of what else was on the
 * entity would be a bug nobody could see.
 *
 * TWO CAVEATS about the world-space pass, both consequences of it being its
 * own system rather than a branch inside the sprite renderer:
 *
 *   - It runs after the sprites, so world-space text composites OVER every
 *     sprite. `layer` orders text against other text, not against sprites.
 *   - It draws for the window's cameras only. A canvas (render/canvas.h)
 *     receives sprites, not text.
 *
 * STRINGS. The component OWNS its string. Assign whatever you have -- a
 * literal, a stack buffer, raylib's TextFormat ring -- and the component
 * takes a copy of it:
 *
 *   ecs_set(world, e, MyeText, { .text = TextFormat("%d", n),
 *                                .size = 20.0f, .color = RAYWHITE });
 *
 * The copy comes from the engine allocator (mye_allocator_of), which in a
 * debug build is the tracking allocator -- so a copy that is never freed
 * makes mye_shutdown() return non-zero and fails the run.
 *
 * It is freed when the component is overwritten, when it is removed, and when
 * the entity is deleted. That is done with flecs TYPE HOOKS -- copy, move,
 * dtor -- rather than with an OnSet observer, and the reason is worth stating
 * because the observer version looks like it works: an observer runs AFTER
 * the new value has overwritten the old one in place, so by the time it could
 * look, the pointer to the previous copy is already gone and the allocation
 * is unreachable. A copy hook is handed both sides and can free the old one.
 *
 * In practice: read MyeText.text freely, and never free it or assign to it
 * through ecs_get_mut. Change the string with mye_text_set (or another
 * ecs_set); change the colour, size or layer through ecs_get_mut as usual.
 */
#ifndef MYE_RENDER_TEXT_H
#define MYE_RENDER_TEXT_H

#include "asset/asset.h"
#include "core/engine.h"

#include <raylib.h>

typedef struct MyeText {
    /* What to draw. Assign anything; the component copies it and `text` then
     * points at the copy. Read it freely, never free it. */
    const char *text;

    /* A zeroed handle means raylib's built-in default font, so text drawn
     * before any font is loaded still appears. */
    mye_font font;

    /* Pixel height. 0 means the font's own base size, which for the built-in
     * default font is a very small 10 -- mye_text_spawn picks 20 instead. */
    float size;
    /* Extra pixels between glyphs. raylib's own default is 1 at the default
     * font's scale; 0 here means exactly zero extra. */
    float spacing;

    Color color;

    /* Higher draws in front, among text. */
    int16_t layer;

    /* False: screen space, in MyeOnDrawUI. True: world space, with the 2D
     * cameras. See the note above -- explicitly, never inferred. */
    bool world_space;
} MyeText;

extern ECS_COMPONENT_DECLARE(MyeText);

void MyeTextModuleImport(ecs_world_t *world);

/* Convenience: spawns a text entity at a screen position, 20px in the default
 * font with one pixel of glyph spacing. Set the component's fields for
 * anything else. */
ecs_entity_t mye_text_spawn(ecs_world_t *world, const char *text, float x,
                            float y, Color color);

/* Replaces the string, keeping every other field. Copies `text`; the previous
 * copy is freed. A no-op on an entity that has no MyeText, and on a NULL
 * string. */
void mye_text_set(ecs_world_t *world, ecs_entity_t entity, const char *text);

/* Width and height the component would occupy if drawn, for centring and
 * layout. Zero in a headless world and for an empty string: measuring needs
 * the glyph metrics, which live in the font atlas the GPU holds. */
Vector2 mye_text_measure(const ecs_world_t *world, const MyeText *text);

#endif /* MYE_RENDER_TEXT_H */

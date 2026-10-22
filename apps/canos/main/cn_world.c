/*
 * CAÑOS - the six worlds (see cn_world.h)
 */
#include "cn_world.h"

#include "aos_i18n.h"

static const cn_world_t s_worlds[CN_WORLDS] = {
    {   /* water: clear and quick, caustics on the bottom, foam at the front */
        .fluid = N_("Agua"), .place = N_("Planta de agua"), .leak = N_("¡Se inundó la planta!"),
        .c_deep = 0x06306E, .c_mid = 0x1A78D2, .c_light = 0x7CCBFF, .c_spec = 0xFFFFFF,
        .speed = 1.00f, .alpha = 0.86f, .curv = 0.55f, .wobble = 1.6f,
        .spread = 0.9f, .drop_speed = 3.2f,
        .tex = 70, .caustics = 150, .bubbles = 70, .bubble_r = 4, .fizz = 0,
        .foam = 170, .sheen = 0, .crust = 0, .spec = 150, .glow = 0,
        .pipe = CN_PIPE_STEEL, .ground = CN_GROUND_TILES, .spill = CN_SPILL_SPLASH,
        .p_hi = 0xE4ECF4, .p_mid = 0x8C9CAE, .p_lo = 0x34404C, .p_inner = 0x161D24,
        .p_accent = 0x2F7DE1,
        .g_a = 0x5C6268, .g_b = 0x4C5258, .g_line = 0x30353A,
        .ui_a = 0x1A78D2, .ui_b = 0x06306E,
    },
    {   /* fuel: amber, a little thicker, a rainbow film on top */
        .fluid = N_("Combustible"), .place = N_("Refinería"), .leak = N_("¡Combustible por todos lados!"),
        .c_deep = 0x4A2000, .c_mid = 0xC07000, .c_light = 0xFFC650, .c_spec = 0xFFF6DC,
        .speed = 0.85f, .alpha = 0.92f, .curv = 0.65f, .wobble = 1.2f,
        .spread = 0.75f, .drop_speed = 2.8f,
        .tex = 80, .caustics = 40, .bubbles = 35, .bubble_r = 4, .fizz = 0,
        .foam = 70, .sheen = 200, .crust = 0, .spec = 170, .glow = 0,
        .pipe = CN_PIPE_BANDS, .ground = CN_GROUND_TREAD, .spill = CN_SPILL_SPLASH,
        .p_hi = 0xEEEEEE, .p_mid = 0x9A9EA4, .p_lo = 0x3E4248, .p_inner = 0x15161A,
        .p_accent = 0xF2C200,
        .g_a = 0x40444A, .g_b = 0x2E3136, .g_line = 0x1E2024,
        .ui_a = 0xE08A00, .ui_b = 0x5A2A00,
    },
    {   /* acid: glowing green in glass, fizzing, fumes when it gets out */
        .fluid = N_("Ácido"), .place = N_("Laboratorio"), .leak = N_("¡El ácido se está comiendo el piso!"),
        .c_deep = 0x165000, .c_mid = 0x4CCC00, .c_light = 0xC4FF48, .c_spec = 0xF6FFDC,
        .speed = 0.95f, .alpha = 0.80f, .curv = 0.55f, .wobble = 1.4f,
        .spread = 0.85f, .drop_speed = 3.0f,
        .tex = 60, .caustics = 90, .bubbles = 60, .bubble_r = 3, .fizz = 200,
        .foam = 120, .sheen = 0, .crust = 0, .spec = 160, .glow = 150,
        .pipe = CN_PIPE_GLASS, .ground = CN_GROUND_LAB, .spill = CN_SPILL_FUMES,
        .p_hi = 0xFFFFFF, .p_mid = 0xA6C8C0, .p_lo = 0x4C665E, .p_inner = 0x1A2624,
        .p_accent = 0xE8E8E8,
        .g_a = 0x253432, .g_b = 0x1C2826, .g_line = 0x0E1615,
        .ui_a = 0x4CCC00, .ui_b = 0x124000,
    },
    {   /* crude oil: black and slow, a round glossy front, few big bubbles */
        .fluid = N_("Petróleo"), .place = N_("Yacimiento"), .leak = N_("¡Derrame de petróleo!"),
        .c_deep = 0x040302, .c_mid = 0x1E150C, .c_light = 0x5A4228, .c_spec = 0xF0E8DA,
        .speed = 0.55f, .alpha = 1.00f, .curv = 0.95f, .wobble = 0.5f,
        .spread = 0.35f, .drop_speed = 1.7f,
        .tex = 45, .caustics = 0, .bubbles = 22, .bubble_r = 7, .fizz = 0,
        .foam = 0, .sheen = 60, .crust = 0, .spec = 230, .glow = 0,
        .pipe = CN_PIPE_RUST, .ground = CN_GROUND_SAND, .spill = CN_SPILL_SPLASH,
        .p_hi = 0xC88E5C, .p_mid = 0x7C4A2A, .p_lo = 0x382012, .p_inner = 0x120A06,
        .p_accent = 0x2C2C2C,
        .g_a = 0x8E6E46, .g_b = 0x7A5A38, .g_line = 0x5E4228,
        .ui_a = 0x6A4A2A, .ui_b = 0x140C06,
    },
    {   /* lava: very slow, glowing, a dark crust floating on it */
        .fluid = N_("Lava"), .place = N_("Fundición"), .leak = N_("¡La lava se escapó del canal!"),
        .c_deep = 0x2A0400, .c_mid = 0xD03008, .c_light = 0xFFA020, .c_spec = 0xFFF2B0,
        .speed = 0.45f, .alpha = 1.00f, .curv = 0.80f, .wobble = 0.6f,
        .spread = 0.25f, .drop_speed = 1.5f,
        .tex = 110, .caustics = 0, .bubbles = 14, .bubble_r = 6, .fizz = 0,
        .foam = 0, .sheen = 0, .crust = 210, .spec = 80, .glow = 230,
        .pipe = CN_PIPE_CERAMIC, .ground = CN_GROUND_BASALT, .spill = CN_SPILL_SPARKS,
        .p_hi = 0xC0B6AA, .p_mid = 0x726860, .p_lo = 0x2E2926, .p_inner = 0x140C0A,
        .p_accent = 0xB03010,
        .g_a = 0x2C2624, .g_b = 0x1E1A19, .g_line = 0x0E0C0B,
        .ui_a = 0xE04010, .ui_b = 0x3A0800,
    },
    {   /* liquid nitrogen: nearly clear, the fastest, boiling all the way */
        .fluid = N_("Nitrógeno"), .place = N_("Criogenia"), .leak = N_("¡Una nube de nitrógeno helado!"),
        .c_deep = 0x2A6A88, .c_mid = 0x8ED8EE, .c_light = 0xE2FAFF, .c_spec = 0xFFFFFF,
        .speed = 1.25f, .alpha = 0.62f, .curv = 0.30f, .wobble = 2.2f,
        .spread = 1.2f, .drop_speed = 3.6f,
        .tex = 50, .caustics = 120, .bubbles = 110, .bubble_r = 3, .fizz = 170,
        .foam = 160, .sheen = 0, .crust = 0, .spec = 140, .glow = 0,
        .pipe = CN_PIPE_FROST, .ground = CN_GROUND_ICE, .spill = CN_SPILL_MIST,
        .p_hi = 0xF6FBFF, .p_mid = 0xB0C4D2, .p_lo = 0x566A7A, .p_inner = 0x1C2830,
        .p_accent = 0x5AC8FA,
        .g_a = 0x3C4C5A, .g_b = 0x2E3A48, .g_line = 0x1C2630,
        .ui_a = 0x5AC8FA, .ui_b = 0x1C3C58,
    },
};

const cn_world_t *cn_world(int i)
{
    if (i < 0 || i >= CN_WORLDS) i = 0;
    return &s_worlds[i];
}

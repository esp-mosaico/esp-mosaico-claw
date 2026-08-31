/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sky Shooter firmware port. This mirrors the current HTML model: bounded
 * pursuit control, straight player shots, four timed enemy roles, aimed hostile
 * volleys, combo scoring, level breathing beats, pause/results, and best score.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "air_battle_actions.h"
#include "air_battle_binds.h"
#include "air_battle_objects.h"
#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"
#include "mosaic_ui.h"
#include "music_audio.h"

#if defined(ESP_PLATFORM)
#include <sys/stat.h>
#include "claw_paths.h"
#include "nvs.h"
#endif

#define AIR_BATTLE_APP_ID 44U
#define SCREEN 480
#define PLAY_TOP 92
#define PLAYER_W 48
#define PLAYER_H 60
#define PLAYER_X0 216
#define PLAYER_Y0 372
#define PLAYER_MIN_X 12
#define PLAYER_MAX_X 468
#define PLAYER_MIN_Y 100
#define PLAYER_MAX_Y 468
#define PLAYER_SPEED 1800.0f
#define BULLET_W 4
#define BULLET_H 12
#define BULLET_MAX 8
#define BULLET_SPEED 520.0f
#define FIRE_MS 180
#define OVERDRIVE_FIRE_MS 150
#define ENEMY_W 48
#define ENEMY_H 44
#define ENEMY_MAX 7
#define ENEMY_KINDS 4
#define GUNNER_MAX 3
#define ENEMY_BULLET_W 4
#define ENEMY_BULLET_H 8
#define ENEMY_BULLET_MAX 12
#define ENEMY_BULLET_SPEED_MIN 160.0f
#define ENEMY_BULLET_SPEED_MAX 260.0f
#define ENEMY_BULLET_MAX_AIM_DEG 30.0f
#define ENEMY_FIRE_WARMUP_MS 850.0f
#define ENEMY_FIRE_TELEGRAPH_MS 380.0f
#define ENEMY_FIRE_MIN_Y 148.0f
#define ENEMY_FIRE_SAFE_GAP_Y 76.0f
#define STRIKER_DIVE_ACCEL 42.0f
#define STRIKER_SPEED_MAX 290.0f
#define BOOM_W 48
#define BOOM_H 48
#define BOOM_MAX 4
#define BOOM_MS 240
#define BOOM_FRAME_MS 80
#define SCORE_POP_MAX 6
#define SCORE_POP_MS 420
#define STAR_MAX 25
#define STAR_STEP_MS 16
#define STAR_RENDER_GROUPS 4
#define POSITION_UPDATE_MAX (3 + BULLET_MAX + ENEMY_BULLET_MAX + \
    ENEMY_MAX * 2 + BOOM_MAX + SCORE_POP_MAX + 1)
#define STAR_MIN_SPEED 18.0f
#define STAR_MAX_SPEED 72.0f
#define RAIL_DASH_MAX 7
#define RAIL_ANIMATION_MS 550
#define RAIL_GRAY4_RGB UINT32_C(0x909295)
#define RAIL_GRAY5_RGB UINT32_C(0x3B3C3D)
#define RAIL_GRAY6_RGB UINT32_C(0x181819)
#define RAIL_ACCENT_RGB UINT32_C(0xFF4C01)
#define RAIL_WARN_RGB UINT32_C(0xFDA711)
#define RAIL_RED_RGB UINT32_C(0xE8362D)
#define START_LIVES 3
#define LEVEL_MS 12000
#define LEVEL_NOTICE_MS 900
#define LEVEL_BREATH_MS 1000
#define COMBO_WINDOW_MS 1600
#define DAMAGE_COOLDOWN_MS 900
#define DAMAGE_FLASH_MS 180
#define ENTRY_MS 300
#define SCORE_MAX 9999990
#define ARC_TANGENT_MAX 260.0f
#define NVS_NAMESPACE "air_battle"
#define NVS_BEST_KEY "best"
#define NVS_AUDIO_KEY "audio"

typedef enum {
    ST_IDLE = 0,
    ST_PLAY,
    ST_PAUSED,
    ST_OVER,
} game_state_t;

typedef enum {
    ROLE_SCOUT = 0,
    ROLE_TURNER,
    ROLE_STRIKER,
    ROLE_GUNNER,
} enemy_role_t;

typedef enum {
    PATH_STRAIGHT = 0,
    PATH_DIAGONAL,
    PATH_ARC,
} enemy_path_t;

typedef struct {
    bool alive;
    float x, y;
} bullet_t;

typedef struct {
    bool alive;
    float x, y, vx, vy, angle_deg;
} enemy_bullet_t;

typedef struct {
    bool alive;
    float x, y, vx, vy;
    float origin_x, amp, phase, omega;
    float entry_ms;
    float fire_cooldown_ms, fire_every_ms;
    float turn_cooldown_ms, turn_every_ms;
    float dive_accel;
    float heading_deg;
    enemy_role_t role;
    enemy_path_t path;
    int kind;
    bool can_shoot;
    bool warning;
} enemy_t;

typedef struct {
    bool alive;
    float x, y;
    int age_ms;
} boom_t;

typedef struct {
    bool alive;
    float x, y;
    int value;
    int age_ms;
} score_pop_t;

typedef struct {
    float x, y;
    float speed;
} star_t;

typedef struct {
    int x, y;
    bool visible;
} render_item_t;

typedef struct {
    uint32_t object;
    int x, y;
    render_item_t *cached;
} position_update_t;

typedef struct {
    position_update_t items[POSITION_UPDATE_MAX];
    size_t count;
} position_batch_t;

typedef struct {
    bool valid;
    render_item_t player;
    render_item_t exhaust[2];
    int star_x[STAR_MAX], star_y[STAR_MAX];
    render_item_t bullets[BULLET_MAX];
    render_item_t enemy_bullets[ENEMY_BULLET_MAX];
    render_item_t enemies[ENEMY_MAX][ENEMY_KINDS];
    render_item_t warnings[ENEMY_MAX];
    render_item_t booms[BOOM_MAX][3];
    render_item_t score_pops[SCORE_POP_MAX];
    int score_pop_value[SCORE_POP_MAX];
    render_item_t aim;
    int score, best_score, level, lives;
    int pause_state, audio_enabled, level_progress;
    bool combo_visible;
    int combo, multiplier, combo_progress;
    bool level_notice_visible;
    int level_notice_level;
    const char *level_unlock;
    bool new_best_visible, flash_visible;
    bool overlay_visible, results_visible;
    game_state_t overlay_state;
    int result_score, result_best, result_combo;
} render_cache_t;

typedef struct {
    int level;
    float level_progress;
    float spawn_interval_ms;
    float speed_scale;
    float turner_chance;
    float striker_chance;
    float gunner_chance;
    int shooter_cap;
    int enemy_bullet_cap;
    float enemy_bullet_speed;
    int enemy_volley_size;
    float enemy_fire_interval_ms;
    int enemy_cap;
} difficulty_t;

typedef struct {
    game_state_t state;
    float player_x, player_y;
    float target_x, target_y;
    int score, best_score, best_at_run_start;
    int lives;
    int active_ms;
    int level;
    int level_notice_ms;
    int spawn_pause_ms;
    int new_best_notice_ms;
    int combo, max_combo, combo_ms, multiplier;
    int invuln_ms, damage_flash_ms;
    int haptic_cooldown_ms;
    int exhaust_ms;
    int star_step_acc_ms;
    int star_render_group;
    float fire_acc, spawn_acc;
    uint32_t rng;
    uint32_t star_rng;
    bool stars_dirty;
    int pending_role;
    const char *unlock_label;
    bool new_best;
    bool audio_enabled;
    bool preferences_loaded;
    bullet_t bullets[BULLET_MAX];
    enemy_bullet_t enemy_bullets[ENEMY_BULLET_MAX];
    enemy_t enemies[ENEMY_MAX];
    boom_t booms[BOOM_MAX];
    score_pop_t score_pops[SCORE_POP_MAX];
    star_t stars[STAR_MAX];
} air_battle_t;

static air_battle_t s_game;
static render_cache_t s_render;
static music_audio_handle_t s_audio;
static int s_audio_poll_ms;
static int s_rail_render_key = -1;

static const uint16_t s_bullet_bind[BULLET_MAX] = {
    GSP_BIND_BULLET_0_VISIBLE, GSP_BIND_BULLET_1_VISIBLE,
    GSP_BIND_BULLET_2_VISIBLE, GSP_BIND_BULLET_3_VISIBLE,
    GSP_BIND_BULLET_4_VISIBLE, GSP_BIND_BULLET_5_VISIBLE,
    GSP_BIND_BULLET_6_VISIBLE, GSP_BIND_BULLET_7_VISIBLE,
};
static const uint32_t s_bullet_obj[BULLET_MAX] = {
    GSP_OBJ_KEY_BULLET_0, GSP_OBJ_KEY_BULLET_1,
    GSP_OBJ_KEY_BULLET_2, GSP_OBJ_KEY_BULLET_3,
    GSP_OBJ_KEY_BULLET_4, GSP_OBJ_KEY_BULLET_5,
    GSP_OBJ_KEY_BULLET_6, GSP_OBJ_KEY_BULLET_7,
};
static const uint16_t s_enemy_bullet_bind[ENEMY_BULLET_MAX] = {
    GSP_BIND_ENEMY_BULLET_0_VISIBLE, GSP_BIND_ENEMY_BULLET_1_VISIBLE,
    GSP_BIND_ENEMY_BULLET_2_VISIBLE, GSP_BIND_ENEMY_BULLET_3_VISIBLE,
    GSP_BIND_ENEMY_BULLET_4_VISIBLE, GSP_BIND_ENEMY_BULLET_5_VISIBLE,
    GSP_BIND_ENEMY_BULLET_6_VISIBLE, GSP_BIND_ENEMY_BULLET_7_VISIBLE,
    GSP_BIND_ENEMY_BULLET_8_VISIBLE, GSP_BIND_ENEMY_BULLET_9_VISIBLE,
    GSP_BIND_ENEMY_BULLET_10_VISIBLE, GSP_BIND_ENEMY_BULLET_11_VISIBLE,
};
static const uint32_t s_enemy_bullet_obj[ENEMY_BULLET_MAX] = {
    GSP_OBJ_KEY_ENEMY_BULLET_0, GSP_OBJ_KEY_ENEMY_BULLET_1,
    GSP_OBJ_KEY_ENEMY_BULLET_2, GSP_OBJ_KEY_ENEMY_BULLET_3,
    GSP_OBJ_KEY_ENEMY_BULLET_4, GSP_OBJ_KEY_ENEMY_BULLET_5,
    GSP_OBJ_KEY_ENEMY_BULLET_6, GSP_OBJ_KEY_ENEMY_BULLET_7,
    GSP_OBJ_KEY_ENEMY_BULLET_8, GSP_OBJ_KEY_ENEMY_BULLET_9,
    GSP_OBJ_KEY_ENEMY_BULLET_10, GSP_OBJ_KEY_ENEMY_BULLET_11,
};
static const uint16_t s_enemy_bind[ENEMY_MAX][ENEMY_KINDS] = {
    {GSP_BIND_ENEMY_0_0_VISIBLE, GSP_BIND_ENEMY_0_1_VISIBLE,
     GSP_BIND_ENEMY_0_2_VISIBLE, GSP_BIND_ENEMY_0_3_VISIBLE},
    {GSP_BIND_ENEMY_1_0_VISIBLE, GSP_BIND_ENEMY_1_1_VISIBLE,
     GSP_BIND_ENEMY_1_2_VISIBLE, GSP_BIND_ENEMY_1_3_VISIBLE},
    {GSP_BIND_ENEMY_2_0_VISIBLE, GSP_BIND_ENEMY_2_1_VISIBLE,
     GSP_BIND_ENEMY_2_2_VISIBLE, GSP_BIND_ENEMY_2_3_VISIBLE},
    {GSP_BIND_ENEMY_3_0_VISIBLE, GSP_BIND_ENEMY_3_1_VISIBLE,
     GSP_BIND_ENEMY_3_2_VISIBLE, GSP_BIND_ENEMY_3_3_VISIBLE},
    {GSP_BIND_ENEMY_4_0_VISIBLE, GSP_BIND_ENEMY_4_1_VISIBLE,
     GSP_BIND_ENEMY_4_2_VISIBLE, GSP_BIND_ENEMY_4_3_VISIBLE},
    {GSP_BIND_ENEMY_5_0_VISIBLE, GSP_BIND_ENEMY_5_1_VISIBLE,
     GSP_BIND_ENEMY_5_2_VISIBLE, GSP_BIND_ENEMY_5_3_VISIBLE},
    {GSP_BIND_ENEMY_6_0_VISIBLE, GSP_BIND_ENEMY_6_1_VISIBLE,
     GSP_BIND_ENEMY_6_2_VISIBLE, GSP_BIND_ENEMY_6_3_VISIBLE},
};
static const uint32_t s_enemy_obj[ENEMY_MAX][ENEMY_KINDS] = {
    {GSP_OBJ_KEY_ENEMY_0_0, GSP_OBJ_KEY_ENEMY_0_1,
     GSP_OBJ_KEY_ENEMY_0_2, GSP_OBJ_KEY_ENEMY_0_3},
    {GSP_OBJ_KEY_ENEMY_1_0, GSP_OBJ_KEY_ENEMY_1_1,
     GSP_OBJ_KEY_ENEMY_1_2, GSP_OBJ_KEY_ENEMY_1_3},
    {GSP_OBJ_KEY_ENEMY_2_0, GSP_OBJ_KEY_ENEMY_2_1,
     GSP_OBJ_KEY_ENEMY_2_2, GSP_OBJ_KEY_ENEMY_2_3},
    {GSP_OBJ_KEY_ENEMY_3_0, GSP_OBJ_KEY_ENEMY_3_1,
     GSP_OBJ_KEY_ENEMY_3_2, GSP_OBJ_KEY_ENEMY_3_3},
    {GSP_OBJ_KEY_ENEMY_4_0, GSP_OBJ_KEY_ENEMY_4_1,
     GSP_OBJ_KEY_ENEMY_4_2, GSP_OBJ_KEY_ENEMY_4_3},
    {GSP_OBJ_KEY_ENEMY_5_0, GSP_OBJ_KEY_ENEMY_5_1,
     GSP_OBJ_KEY_ENEMY_5_2, GSP_OBJ_KEY_ENEMY_5_3},
    {GSP_OBJ_KEY_ENEMY_6_0, GSP_OBJ_KEY_ENEMY_6_1,
     GSP_OBJ_KEY_ENEMY_6_2, GSP_OBJ_KEY_ENEMY_6_3},
};
static const uint16_t s_warning_bind[ENEMY_MAX] = {
    GSP_BIND_ENEMY_WARNING_0_VISIBLE, GSP_BIND_ENEMY_WARNING_1_VISIBLE,
    GSP_BIND_ENEMY_WARNING_2_VISIBLE, GSP_BIND_ENEMY_WARNING_3_VISIBLE,
    GSP_BIND_ENEMY_WARNING_4_VISIBLE, GSP_BIND_ENEMY_WARNING_5_VISIBLE,
    GSP_BIND_ENEMY_WARNING_6_VISIBLE,
};
static const uint32_t s_warning_obj[ENEMY_MAX] = {
    GSP_OBJ_KEY_ENEMY_WARNING_0, GSP_OBJ_KEY_ENEMY_WARNING_1,
    GSP_OBJ_KEY_ENEMY_WARNING_2, GSP_OBJ_KEY_ENEMY_WARNING_3,
    GSP_OBJ_KEY_ENEMY_WARNING_4, GSP_OBJ_KEY_ENEMY_WARNING_5,
    GSP_OBJ_KEY_ENEMY_WARNING_6,
};
static const uint16_t s_boom_bind[BOOM_MAX][3] = {
    {GSP_BIND_BOOM_0_0_VISIBLE, GSP_BIND_BOOM_0_1_VISIBLE,
     GSP_BIND_BOOM_0_2_VISIBLE},
    {GSP_BIND_BOOM_1_0_VISIBLE, GSP_BIND_BOOM_1_1_VISIBLE,
     GSP_BIND_BOOM_1_2_VISIBLE},
    {GSP_BIND_BOOM_2_0_VISIBLE, GSP_BIND_BOOM_2_1_VISIBLE,
     GSP_BIND_BOOM_2_2_VISIBLE},
    {GSP_BIND_BOOM_3_0_VISIBLE, GSP_BIND_BOOM_3_1_VISIBLE,
     GSP_BIND_BOOM_3_2_VISIBLE},
};
static const uint32_t s_boom_obj[BOOM_MAX][3] = {
    {GSP_OBJ_KEY_BOOM_0_0, GSP_OBJ_KEY_BOOM_0_1,
     GSP_OBJ_KEY_BOOM_0_2},
    {GSP_OBJ_KEY_BOOM_1_0, GSP_OBJ_KEY_BOOM_1_1,
     GSP_OBJ_KEY_BOOM_1_2},
    {GSP_OBJ_KEY_BOOM_2_0, GSP_OBJ_KEY_BOOM_2_1,
     GSP_OBJ_KEY_BOOM_2_2},
    {GSP_OBJ_KEY_BOOM_3_0, GSP_OBJ_KEY_BOOM_3_1,
     GSP_OBJ_KEY_BOOM_3_2},
};
static const uint16_t s_score_pop_bind[SCORE_POP_MAX] = {
    GSP_BIND_SCORE_POP_0_VISIBLE, GSP_BIND_SCORE_POP_1_VISIBLE,
    GSP_BIND_SCORE_POP_2_VISIBLE, GSP_BIND_SCORE_POP_3_VISIBLE,
    GSP_BIND_SCORE_POP_4_VISIBLE, GSP_BIND_SCORE_POP_5_VISIBLE,
};
static const uint16_t s_score_pop_text_bind[SCORE_POP_MAX] = {
    GSP_BIND_SCORE_POP_0_TEXT, GSP_BIND_SCORE_POP_1_TEXT,
    GSP_BIND_SCORE_POP_2_TEXT, GSP_BIND_SCORE_POP_3_TEXT,
    GSP_BIND_SCORE_POP_4_TEXT, GSP_BIND_SCORE_POP_5_TEXT,
};
static const uint32_t s_score_pop_obj[SCORE_POP_MAX] = {
    GSP_OBJ_KEY_SCORE_POP_0, GSP_OBJ_KEY_SCORE_POP_1,
    GSP_OBJ_KEY_SCORE_POP_2, GSP_OBJ_KEY_SCORE_POP_3,
    GSP_OBJ_KEY_SCORE_POP_4, GSP_OBJ_KEY_SCORE_POP_5,
};
static const uint32_t s_star_obj[STAR_MAX] = {
    GSP_OBJ_KEY_STAR_0, GSP_OBJ_KEY_STAR_1,
    GSP_OBJ_KEY_STAR_2, GSP_OBJ_KEY_STAR_3,
    GSP_OBJ_KEY_STAR_4, GSP_OBJ_KEY_STAR_5,
    GSP_OBJ_KEY_STAR_6, GSP_OBJ_KEY_STAR_7,
    GSP_OBJ_KEY_STAR_8, GSP_OBJ_KEY_STAR_9,
    GSP_OBJ_KEY_STAR_10, GSP_OBJ_KEY_STAR_11,
    GSP_OBJ_KEY_STAR_12, GSP_OBJ_KEY_STAR_13,
    GSP_OBJ_KEY_STAR_14, GSP_OBJ_KEY_STAR_15,
    GSP_OBJ_KEY_STAR_16, GSP_OBJ_KEY_STAR_17,
    GSP_OBJ_KEY_STAR_18, GSP_OBJ_KEY_STAR_19,
    GSP_OBJ_KEY_STAR_20, GSP_OBJ_KEY_STAR_21,
    GSP_OBJ_KEY_STAR_22, GSP_OBJ_KEY_STAR_23,
    GSP_OBJ_KEY_STAR_24,
};
static const uint16_t s_rail_line_bind[2][3][3] = {
    {
        {GSP_BIND_RAIL_LEFT_0_VERTICAL_COLOR,
         GSP_BIND_RAIL_LEFT_0_TOP_COLOR,
         GSP_BIND_RAIL_LEFT_0_BOTTOM_COLOR},
        {GSP_BIND_RAIL_LEFT_1_VERTICAL_COLOR,
         GSP_BIND_RAIL_LEFT_1_TOP_COLOR,
         GSP_BIND_RAIL_LEFT_1_BOTTOM_COLOR},
        {GSP_BIND_RAIL_LEFT_2_VERTICAL_COLOR,
         GSP_BIND_RAIL_LEFT_2_TOP_COLOR,
         GSP_BIND_RAIL_LEFT_2_BOTTOM_COLOR},
    },
    {
        {GSP_BIND_RAIL_RIGHT_0_VERTICAL_COLOR,
         GSP_BIND_RAIL_RIGHT_0_TOP_COLOR,
         GSP_BIND_RAIL_RIGHT_0_BOTTOM_COLOR},
        {GSP_BIND_RAIL_RIGHT_1_VERTICAL_COLOR,
         GSP_BIND_RAIL_RIGHT_1_TOP_COLOR,
         GSP_BIND_RAIL_RIGHT_1_BOTTOM_COLOR},
        {GSP_BIND_RAIL_RIGHT_2_VERTICAL_COLOR,
         GSP_BIND_RAIL_RIGHT_2_TOP_COLOR,
         GSP_BIND_RAIL_RIGHT_2_BOTTOM_COLOR},
    },
};
static const uint16_t s_rail_node_bind[2][3] = {
    {GSP_BIND_RAIL_LEFT_0_NODE_COLOR, GSP_BIND_RAIL_LEFT_1_NODE_COLOR,
     GSP_BIND_RAIL_LEFT_2_NODE_COLOR},
    {GSP_BIND_RAIL_RIGHT_0_NODE_COLOR, GSP_BIND_RAIL_RIGHT_1_NODE_COLOR,
     GSP_BIND_RAIL_RIGHT_2_NODE_COLOR},
};
static const uint32_t s_rail_mid_obj[2] = {
    GSP_OBJ_KEY_RAIL_LEFT_1, GSP_OBJ_KEY_RAIL_RIGHT_1,
};
static const uint16_t s_rail_mid_dash_bind[2][RAIL_DASH_MAX] = {
    {
        GSP_BIND_RAIL_LEFT_MID_DASH_0_VISIBLE,
        GSP_BIND_RAIL_LEFT_MID_DASH_1_VISIBLE,
        GSP_BIND_RAIL_LEFT_MID_DASH_2_VISIBLE,
        GSP_BIND_RAIL_LEFT_MID_DASH_3_VISIBLE,
        GSP_BIND_RAIL_LEFT_MID_DASH_4_VISIBLE,
        GSP_BIND_RAIL_LEFT_MID_DASH_5_VISIBLE,
        GSP_BIND_RAIL_LEFT_MID_DASH_6_VISIBLE,
    },
    {
        GSP_BIND_RAIL_RIGHT_MID_DASH_0_VISIBLE,
        GSP_BIND_RAIL_RIGHT_MID_DASH_1_VISIBLE,
        GSP_BIND_RAIL_RIGHT_MID_DASH_2_VISIBLE,
        GSP_BIND_RAIL_RIGHT_MID_DASH_3_VISIBLE,
        GSP_BIND_RAIL_RIGHT_MID_DASH_4_VISIBLE,
        GSP_BIND_RAIL_RIGHT_MID_DASH_5_VISIBLE,
        GSP_BIND_RAIL_RIGHT_MID_DASH_6_VISIBLE,
    },
};

static const mosaic_app_route_t s_air_battle_routes[] = {
    {.action_id = GSP_ACT_ID_GAME_EXIT, .target_name = "mosaic-hub"},
};

static float clampf(float value, float minimum, float maximum)
{
    return value < minimum ? minimum
        : value > maximum ? maximum : value;
}

static bool overlap(float ax, float ay, float aw, float ah,
                    float bx, float by, float bw, float bh)
{
    return ax < bx + bw && ax + aw > bx &&
           ay < by + bh && ay + ah > by;
}

static float random_from(uint32_t *state)
{
    uint32_t z = (*state += 0x6D2B79F5U);
    z = (z ^ (z >> 15U)) * (z | 1U);
    z ^= z + ((z ^ (z >> 7U)) * (z | 61U));
    z ^= z >> 14U;
    return (float)z / 4294967296.0f;
}

static float random_unit(void)
{
    return random_from(&s_game.rng);
}

static float star_random_unit(void)
{
    return random_from(&s_game.star_rng);
}

static void respawn_star(star_t *star, bool spread_over_playfield)
{
    star->x = 4.0f + star_random_unit() * (SCREEN - 10.0f);
    star->y = spread_over_playfield
        ? PLAY_TOP + star_random_unit() * (SCREEN - PLAY_TOP)
        : PLAY_TOP - 2.0f - star_random_unit() * 26.0f;
    star->speed = STAR_MIN_SPEED +
        star_random_unit() * (STAR_MAX_SPEED - STAR_MIN_SPEED);
}

static void reset_stars(void)
{
    for (int index = 0; index < STAR_MAX; ++index) {
        respawn_star(&s_game.stars[index], true);
    }
    s_game.star_step_acc_ms = 0;
    s_game.star_render_group = -1;
    s_game.stars_dirty = true;
}

static void update_stars(int ms)
{
    s_game.star_step_acc_ms += ms;
    const int steps = s_game.star_step_acc_ms / STAR_STEP_MS;
    if (steps == 0) return;
    s_game.star_step_acc_ms -= steps * STAR_STEP_MS;
    const float elapsed = steps * STAR_STEP_MS / 1000.0f;
    for (int index = 0; index < STAR_MAX; ++index) {
        star_t *star = &s_game.stars[index];
        star->y += star->speed * elapsed;
        if (star->y > SCREEN) {
            respawn_star(star, false);
        }
    }
    if (s_game.star_render_group < 0) {
        s_game.star_render_group = 0;
    } else {
        s_game.star_render_group =
            (s_game.star_render_group + steps) % STAR_RENDER_GROUPS;
    }
    s_game.stars_dirty = true;
}

static void invalidate_render_cache(void)
{
    s_render.valid = false;
    s_rail_render_key = -1;
}

static void render_visible_if_changed(
    esp_gsp_handle_t ui, uint16_t bind, bool visible, bool *cached)
{
    if (!s_render.valid || *cached != visible) {
        if (esp_gsp_set_visible(ui, bind, visible) == ESP_OK) {
            *cached = visible;
        }
    }
}

static void queue_position_if_changed(
    position_batch_t *batch, uint32_t object, int x, int y,
    render_item_t *cached)
{
    if (!s_render.valid || cached->x != x || cached->y != y) {
        if (batch->count < POSITION_UPDATE_MAX) {
            batch->items[batch->count++] = (position_update_t) {
                .object = object,
                .x = x,
                .y = y,
                .cached = cached,
            };
        }
    }
}

static void render_position_batch(
    esp_gsp_handle_t ui, const position_batch_t *batch)
{
    gsp_component_property_update_t updates[ESP_GSP_COMPONENT_BATCH_MAX];
    const size_t item_limit = ESP_GSP_COMPONENT_BATCH_MAX / 2U;
    size_t offset = 0;
    while (offset < batch->count) {
        const size_t remaining = batch->count - offset;
        const size_t item_count =
            remaining < item_limit ? remaining : item_limit;
        for (size_t index = 0; index < item_count; ++index) {
            const position_update_t *item = &batch->items[offset + index];
            updates[index * 2U] = (gsp_component_property_update_t) {
                .component = item->object,
                .property = GSP_PROPERTY_KEY_X,
                .value = {.type = GSP_VALUE_I32, .data.i32 = item->x},
            };
            updates[index * 2U + 1U] = (gsp_component_property_update_t) {
                .component = item->object,
                .property = GSP_PROPERTY_KEY_Y,
                .value = {.type = GSP_VALUE_I32, .data.i32 = item->y},
            };
        }
        if (esp_gsp_component_set_properties(
                ui, updates, item_count * 2U) == ESP_OK) {
            for (size_t index = 0; index < item_count; ++index) {
                const position_update_t *item =
                    &batch->items[offset + index];
                item->cached->x = item->x;
                item->cached->y = item->y;
            }
        }
        offset += item_count;
    }
}

static int combo_multiplier(int combo)
{
    int multiplier = 1 + (combo > 0 ? combo / 5 : 0);
    return multiplier > 4 ? 4 : multiplier;
}

static difficulty_t difficulty_at(int active_ms)
{
    const float depth = (float)active_ms / (float)LEVEL_MS;
    const int level = 1 + (int)floorf(depth);
    const float turner = depth < 1.0f ? 0.0f :
        fminf(0.34f, 0.18f + 0.16f * (1.0f - expf(-0.28f * (depth - 1.0f))));
    const float striker = depth < 2.0f ? 0.0f :
        fminf(0.25f, 0.13f + 0.12f * (1.0f - expf(-0.24f * (depth - 2.0f))));
    const float gunner = depth < 3.0f ? 0.0f :
        fminf(0.28f, 0.10f + 0.18f * (1.0f - expf(-0.18f * (depth - 3.0f))));
    const float late = fmaxf(0.0f, depth - 3.0f);
    int shooter_cap = depth < 3.0f ? 0 : 1 + (level - 4) / 4;
    if (shooter_cap > GUNNER_MAX) shooter_cap = GUNNER_MAX;
    int enemy_bullet_cap = depth < 3.0f ? 0 : 4 + (level - 4) / 2;
    if (enemy_bullet_cap > ENEMY_BULLET_MAX) {
        enemy_bullet_cap = ENEMY_BULLET_MAX;
    }
    int enemy_cap = 3 + (int)floorf(depth / 2.5f);
    if (enemy_cap > ENEMY_MAX) enemy_cap = ENEMY_MAX;
    return (difficulty_t) {
        .level = level,
        .level_progress = depth - floorf(depth),
        .spawn_interval_ms = 330.0f + 570.0f * expf(-0.16f * depth),
        .speed_scale = 1.0f + 0.95f * (1.0f - expf(-0.11f * depth)),
        .turner_chance = turner,
        .striker_chance = striker,
        .gunner_chance = gunner,
        .shooter_cap = shooter_cap,
        .enemy_bullet_cap = enemy_bullet_cap,
        .enemy_bullet_speed = ENEMY_BULLET_SPEED_MIN +
            (ENEMY_BULLET_SPEED_MAX - ENEMY_BULLET_SPEED_MIN) *
            (1.0f - expf(-0.16f * late)),
        .enemy_volley_size = level < 8 ? 1 : level < 12 ? 2 : 3,
        .enemy_fire_interval_ms = 820.0f + 680.0f * expf(-0.16f * late),
        .enemy_cap = enemy_cap,
    };
}

static void load_preferences(void)
{
    if (s_game.preferences_loaded) {
        return;
    }
    s_game.preferences_loaded = true;
    s_game.audio_enabled = true;
#if defined(ESP_PLATFORM)
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint32_t best = 0;
    uint8_t audio = 1;
    if (nvs_get_u32(nvs, NVS_BEST_KEY, &best) == ESP_OK) {
        s_game.best_score = best > SCORE_MAX ? SCORE_MAX : (int)best;
    }
    if (nvs_get_u8(nvs, NVS_AUDIO_KEY, &audio) == ESP_OK) {
        s_game.audio_enabled = audio != 0;
    }
    nvs_close(nvs);
#endif
}

static void save_preferences(void)
{
#if defined(ESP_PLATFORM)
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    (void)nvs_set_u32(nvs, NVS_BEST_KEY, (uint32_t)s_game.best_score);
    (void)nvs_set_u8(nvs, NVS_AUDIO_KEY, s_game.audio_enabled ? 1U : 0U);
    (void)nvs_commit(nvs);
    nvs_close(nvs);
#endif
}

static void battle_audio_start(void)
{
    if (!s_game.audio_enabled || s_game.state != ST_PLAY) {
        return;
    }
    if (s_audio == NULL && music_audio_create(&s_audio) != ESP_OK) {
        return;
    }
#if defined(ESP_PLATFORM)
    char path[256];
    struct stat info;
    if (claw_paths_join(
            CLAW_PATH_DATA, "music/space-quest-loop.ogg",
            path, sizeof(path)) != ESP_OK ||
            stat(path, &info) != 0 || info.st_size <= 0) {
        return;
    }
    (void)music_audio_play(s_audio, path, (uint64_t)info.st_size);
#endif
    s_audio_poll_ms = 0;
}

static void battle_audio_pause(void)
{
    if (s_audio != NULL) {
        (void)music_audio_pause(s_audio);
    }
}

static void battle_audio_resume(void)
{
    if (!s_game.audio_enabled || s_game.state != ST_PLAY) {
        return;
    }
    if (s_audio == NULL || music_audio_resume(s_audio) != ESP_OK) {
        battle_audio_start();
    }
}

static void battle_audio_stop(void)
{
    if (s_audio != NULL) {
        (void)music_audio_stop(s_audio);
    }
    s_audio_poll_ms = 0;
}

static void battle_audio_tick(int ms)
{
    if (!s_game.audio_enabled || s_game.state != ST_PLAY ||
            s_audio == NULL) {
        return;
    }
    s_audio_poll_ms += ms;
    if (s_audio_poll_ms < 250) {
        return;
    }
    s_audio_poll_ms = 0;
    music_audio_status_t status;
    if (music_audio_status(s_audio, &status) == ESP_OK &&
            status.state == MUSIC_AUDIO_FINISHED) {
        battle_audio_start();
    }
}

static void reset_combo(void)
{
    s_game.combo = 0;
    s_game.combo_ms = 0;
    s_game.multiplier = 1;
}

static void clear_combat(void)
{
    memset(s_game.bullets, 0, sizeof(s_game.bullets));
    memset(s_game.enemy_bullets, 0, sizeof(s_game.enemy_bullets));
    memset(s_game.enemies, 0, sizeof(s_game.enemies));
    memset(s_game.booms, 0, sizeof(s_game.booms));
    memset(s_game.score_pops, 0, sizeof(s_game.score_pops));
}

static void reset_play(void)
{
    const int best = s_game.best_score;
    const bool audio = s_game.audio_enabled;
    const bool loaded = s_game.preferences_loaded;
    const uint32_t rng = s_game.rng != 0 ? s_game.rng : 7U;
    const uint32_t star_rng = s_game.star_rng != 0
        ? s_game.star_rng : 0xA17B3C5DU;
    memset(&s_game, 0, sizeof(s_game));
    s_game.best_score = best;
    s_game.best_at_run_start = best;
    s_game.audio_enabled = audio;
    s_game.preferences_loaded = loaded;
    s_game.state = ST_PLAY;
    s_game.player_x = PLAYER_X0;
    s_game.player_y = PLAYER_Y0;
    s_game.target_x = PLAYER_X0;
    s_game.target_y = PLAYER_Y0;
    s_game.lives = START_LIVES;
    s_game.level = 1;
    s_game.multiplier = 1;
    s_game.pending_role = -1;
    s_game.rng = rng;
    s_game.star_rng = star_rng;
    reset_stars();
    invalidate_render_cache();
}

static void reset_idle(void)
{
    const int best = s_game.best_score;
    const bool audio = s_game.audio_enabled;
    const bool loaded = s_game.preferences_loaded;
    const uint32_t star_rng = s_game.star_rng != 0
        ? s_game.star_rng : 0xA17B3C5DU;
    memset(&s_game, 0, sizeof(s_game));
    s_game.best_score = best;
    s_game.audio_enabled = audio;
    s_game.preferences_loaded = loaded;
    s_game.state = ST_IDLE;
    s_game.player_x = PLAYER_X0;
    s_game.player_y = PLAYER_Y0;
    s_game.target_x = PLAYER_X0;
    s_game.target_y = PLAYER_Y0;
    s_game.lives = START_LIVES;
    s_game.level = 1;
    s_game.multiplier = 1;
    s_game.pending_role = -1;
    s_game.rng = 7U;
    s_game.star_rng = star_rng;
    reset_stars();
    invalidate_render_cache();
}

static int first_free_bullet(void)
{
    for (int index = 0; index < BULLET_MAX; ++index) {
        if (!s_game.bullets[index].alive) return index;
    }
    return -1;
}

static int first_free_enemy(void)
{
    for (int index = 0; index < ENEMY_MAX; ++index) {
        if (!s_game.enemies[index].alive) return index;
    }
    return -1;
}

static int alive_enemy_count(void)
{
    int count = 0;
    for (int index = 0; index < ENEMY_MAX; ++index) {
        count += s_game.enemies[index].alive;
    }
    return count;
}

static int alive_gunner_count(void)
{
    int count = 0;
    for (int index = 0; index < ENEMY_MAX; ++index) {
        count += s_game.enemies[index].alive &&
            s_game.enemies[index].role == ROLE_GUNNER;
    }
    return count;
}

static int alive_enemy_bullet_count(void)
{
    int count = 0;
    for (int index = 0; index < ENEMY_BULLET_MAX; ++index) {
        count += s_game.enemy_bullets[index].alive;
    }
    return count;
}

static int first_free_enemy_bullet(void)
{
    for (int index = 0; index < ENEMY_BULLET_MAX; ++index) {
        if (!s_game.enemy_bullets[index].alive) return index;
    }
    return -1;
}

static enemy_role_t pick_role(
    const difficulty_t *difficulty, int forced_role)
{
    if (forced_role >= ROLE_SCOUT && forced_role <= ROLE_GUNNER) {
        return (enemy_role_t)forced_role;
    }
    const float roll = random_unit();
    const float gunner = alive_gunner_count() < difficulty->shooter_cap
        ? difficulty->gunner_chance : 0.0f;
    if (roll < gunner) return ROLE_GUNNER;
    if (roll < gunner + difficulty->striker_chance) return ROLE_STRIKER;
    if (roll < gunner + difficulty->striker_chance +
            difficulty->turner_chance) {
        return ROLE_TURNER;
    }
    return ROLE_SCOUT;
}

static float bounded_arc_omega(float base, float amplitude, float speed_scale)
{
    return fminf(
        base * (1.0f + 0.2f * (speed_scale - 1.0f)),
        ARC_TANGENT_MAX / fmaxf(amplitude, 1.0f));
}

static void configure_enemy(
    enemy_t *enemy, enemy_role_t role, const difficulty_t *difficulty)
{
    const float min_x = PLAYER_MIN_X;
    const float max_x = PLAYER_MAX_X - ENEMY_W;
    memset(enemy, 0, sizeof(*enemy));
    enemy->alive = true;
    enemy->y = PLAY_TOP;
    enemy->entry_ms = ENTRY_MS;
    enemy->role = role;
    enemy->turn_cooldown_ms = INFINITY;
    enemy->turn_every_ms = INFINITY;
    enemy->fire_cooldown_ms = INFINITY;
    enemy->fire_every_ms = INFINITY;
    if (role == ROLE_TURNER) {
        const bool from_left = random_unit() < 0.5f;
        enemy->kind = 2;
        enemy->path = PATH_DIAGONAL;
        enemy->x = clampf(
            from_left ? min_x + random_unit() * 80.0f
                      : max_x - random_unit() * 80.0f,
            min_x, max_x);
        enemy->vx = (from_left ? 1.0f : -1.0f) *
            (70.0f + random_unit() * 50.0f) * difficulty->speed_scale;
        enemy->vy = (65.0f + random_unit() * 40.0f) *
            difficulty->speed_scale;
        enemy->turn_every_ms = 650.0f + random_unit() * 350.0f;
        enemy->turn_cooldown_ms = enemy->turn_every_ms *
            (0.55f + random_unit() * 0.35f);
    } else if (role == ROLE_GUNNER) {
        float amplitude = 36.0f + random_unit() * 38.0f;
        float span = max_x - min_x - 2.0f * amplitude;
        if (span < 8.0f) {
            amplitude = 36.0f;
            span = max_x - min_x - 2.0f * amplitude;
        }
        enemy->kind = 3;
        enemy->path = PATH_ARC;
        enemy->amp = amplitude;
        enemy->origin_x = min_x + amplitude + random_unit() * span;
        enemy->phase = random_unit() * 6.2831853f;
        enemy->x = clampf(
            enemy->origin_x + amplitude * sinf(enemy->phase),
            min_x, max_x);
        enemy->vy = (48.0f + random_unit() * 24.0f) *
            difficulty->speed_scale;
        enemy->omega = bounded_arc_omega(
            1.6f + random_unit() * 1.1f, amplitude,
            difficulty->speed_scale);
        enemy->can_shoot = true;
        enemy->fire_every_ms = difficulty->enemy_fire_interval_ms *
            (0.92f + random_unit() * 0.16f);
        enemy->fire_cooldown_ms =
            ENEMY_FIRE_WARMUP_MS + random_unit() * 360.0f;
    } else if (role == ROLE_STRIKER) {
        enemy->kind = 1;
        enemy->path = PATH_STRAIGHT;
        enemy->x = min_x + random_unit() * (max_x - min_x);
        enemy->vy = (102.0f + random_unit() * 34.0f) *
            difficulty->speed_scale;
        enemy->dive_accel = STRIKER_DIVE_ACCEL * difficulty->speed_scale;
    } else {
        enemy->kind = 0;
        enemy->path = PATH_STRAIGHT;
        enemy->x = min_x + random_unit() * (max_x - min_x);
        enemy->vy = (70.0f + random_unit() * 55.0f) *
            difficulty->speed_scale;
    }
}

static bool spawn_enemy(const difficulty_t *difficulty, int forced_role)
{
    const int slot = first_free_enemy();
    if (slot < 0) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        enemy_t candidate;
        configure_enemy(
            &candidate, pick_role(difficulty, forced_role), difficulty);
        const float center = candidate.x + ENEMY_W / 2.0f;
        bool crowded = false;
        for (int index = 0; index < ENEMY_MAX; ++index) {
            const enemy_t *other = &s_game.enemies[index];
            if (other->alive && other->y < PLAY_TOP + ENEMY_H + 40 &&
                    fabsf(center - (other->x + ENEMY_W / 2.0f)) < 72.0f) {
                crowded = true;
                break;
            }
        }
        const bool player_near_top =
            s_game.player_y < PLAY_TOP + 150.0f;
        const bool player_too_close = player_near_top &&
            fabsf(center - (s_game.player_x + PLAYER_W / 2.0f)) < 96.0f;
        if (!crowded && !player_too_close) {
            s_game.enemies[slot] = candidate;
            return true;
        }
    }
    return false;
}

static void spawn_boom(float x, float y, float w, float h)
{
    int slot = -1;
    int oldest = 0;
    for (int index = 0; index < BOOM_MAX; ++index) {
        if (!s_game.booms[index].alive) {
            slot = index;
            break;
        }
        if (s_game.booms[index].age_ms > s_game.booms[oldest].age_ms) {
            oldest = index;
        }
    }
    if (slot < 0) slot = oldest;
    s_game.booms[slot] = (boom_t) {
        .alive = true,
        .x = x + w / 2.0f - BOOM_W / 2.0f,
        .y = y + h / 2.0f - BOOM_H / 2.0f,
    };
}

static int score_for_role(enemy_role_t role)
{
    switch (role) {
    case ROLE_TURNER: return 15;
    case ROLE_STRIKER: return 20;
    case ROLE_GUNNER: return 25;
    case ROLE_SCOUT:
    default: return 10;
    }
}

static void spawn_score_pop(float x, float y, int value)
{
    int slot = -1;
    int oldest = 0;
    for (int index = 0; index < SCORE_POP_MAX; ++index) {
        if (!s_game.score_pops[index].alive) {
            slot = index;
            break;
        }
        if (s_game.score_pops[index].age_ms >
                s_game.score_pops[oldest].age_ms) {
            oldest = index;
        }
    }
    if (slot < 0) slot = oldest;
    s_game.score_pops[slot] = (score_pop_t) {
        .alive = true,
        .x = x - 60.0f,
        .y = y - 12.0f,
        .value = value,
    };
}

static void register_kill(enemy_t *enemy)
{
    s_game.combo = s_game.combo_ms > 0 ? s_game.combo + 1 : 1;
    s_game.combo_ms = COMBO_WINDOW_MS;
    s_game.multiplier = combo_multiplier(s_game.combo);
    if (s_game.haptic_cooldown_ms == 0) {
        (void)mosaic_ui_haptic_feedback(
            s_game.multiplier == 4 ? 18U : 12U);
        s_game.haptic_cooldown_ms = 70;
    }
    if (s_game.combo > s_game.max_combo) s_game.max_combo = s_game.combo;
    int value = score_for_role(enemy->role) * s_game.multiplier;
    if (value > SCORE_MAX - s_game.score) value = SCORE_MAX - s_game.score;
    s_game.score += value;
    if (s_game.score > s_game.best_score) s_game.best_score = s_game.score;
    if (!s_game.new_best && s_game.score > s_game.best_at_run_start) {
        s_game.new_best = true;
        s_game.new_best_notice_ms = 1200;
    }
    if (value > 0) {
        spawn_score_pop(
            enemy->x + ENEMY_W / 2.0f,
            enemy->y + ENEMY_H / 2.0f, value);
    }
}

static bool lose_life(void)
{
    if (s_game.invuln_ms > 0) return false;
    s_game.lives--;
    s_game.invuln_ms = DAMAGE_COOLDOWN_MS;
    s_game.damage_flash_ms = DAMAGE_FLASH_MS;
    reset_combo();
    if (s_game.lives <= 0) {
        s_game.lives = 0;
        s_game.state = ST_OVER;
        clear_combat();
        s_game.level_notice_ms = 0;
        s_game.new_best_notice_ms = 0;
        s_game.spawn_pause_ms = 0;
        battle_audio_stop();
        save_preferences();
    }
    return true;
}

static void move_player_target(int x, int y)
{
    s_game.target_x = clampf(
        (float)x - PLAYER_W / 2.0f,
        PLAYER_MIN_X, PLAYER_MAX_X - PLAYER_W);
    s_game.target_y = clampf(
        (float)y - PLAYER_H / 2.0f,
        PLAYER_MIN_Y, PLAYER_MAX_Y - PLAYER_H);
}

static void advance_player(int ms)
{
    const float dx = s_game.target_x - s_game.player_x;
    const float dy = s_game.target_y - s_game.player_y;
    const float distance = sqrtf(dx * dx + dy * dy);
    if (distance <= 0.001f) return;
    const float maximum = PLAYER_SPEED * (float)ms / 1000.0f;
    if (distance <= maximum) {
        s_game.player_x = s_game.target_x;
        s_game.player_y = s_game.target_y;
    } else {
        s_game.player_x += dx * maximum / distance;
        s_game.player_y += dy * maximum / distance;
    }
}

static void fire_player(void)
{
    const int slot = first_free_bullet();
    if (slot < 0) return;
    s_game.bullets[slot] = (bullet_t) {
        .alive = true,
        .x = s_game.player_x + PLAYER_W / 2.0f - BULLET_W / 2.0f,
        .y = s_game.player_y - BULLET_H,
    };
}

static void fire_enemy(enemy_t *enemy, const difficulty_t *difficulty)
{
    int available = difficulty->enemy_bullet_cap -
        alive_enemy_bullet_count();
    if (available <= 0) return;
    int count = difficulty->enemy_volley_size;
    if (count > available) count = available;
    const float from_x = enemy->x + ENEMY_W / 2.0f;
    const float from_y = enemy->y + ENEMY_H - 2.0f;
    const float target_x = s_game.player_x + PLAYER_W / 2.0f;
    const float target_y = s_game.player_y + PLAYER_H / 2.0f;
    const float dy = fmaxf(1.0f, target_y - from_y);
    const float max_dx = tanf(ENEMY_BULLET_MAX_AIM_DEG *
        0.01745329252f) * dy;
    const float dx = clampf(target_x - from_x, -max_dx, max_dx);
    float center = atan2f(dx, dy) * 57.2957795f;
    const float half_spread = count == 1 ? 0.0f : count == 2 ? 7.0f : 10.0f;
    center = clampf(
        center, -ENEMY_BULLET_MAX_AIM_DEG + half_spread,
        ENEMY_BULLET_MAX_AIM_DEG - half_spread);
    for (int volley = 0; volley < count; ++volley) {
        const int slot = first_free_enemy_bullet();
        if (slot < 0) break;
        const float offset = count == 1 ? 0.0f
            : count == 2 ? (volley == 0 ? -7.0f : 7.0f)
            : (float)(volley - 1) * 10.0f;
        const float angle = center + offset;
        const float radians = angle * 0.01745329252f;
        s_game.enemy_bullets[slot] = (enemy_bullet_t) {
            .alive = true,
            .x = from_x - ENEMY_BULLET_W / 2.0f,
            .y = from_y,
            .vx = sinf(radians) * difficulty->enemy_bullet_speed,
            .vy = cosf(radians) * difficulty->enemy_bullet_speed,
            .angle_deg = angle,
        };
    }
    enemy->fire_cooldown_ms += enemy->fire_every_ms;
}

static void set_unlock_for_level(int level)
{
    s_game.pending_role = -1;
    s_game.unlock_label = NULL;
    if (level == 2) {
        s_game.pending_role = ROLE_TURNER;
        s_game.unlock_label = "TURNER · CHANGES COURSE";
    } else if (level == 3) {
        s_game.pending_role = ROLE_STRIKER;
        s_game.unlock_label = "STRIKER · SPEEDS UP";
    } else if (level == 4) {
        s_game.pending_role = ROLE_GUNNER;
        s_game.unlock_label = "GUNNER · DODGE FIRE";
    } else if (level == 8) {
        s_game.unlock_label = "GUNNER · DUAL FIRE";
    } else if (level == 12) {
        s_game.unlock_label = "GUNNER · TRIPLE FIRE";
    }
}

static void resolve_player_shot_hits(void)
{
    for (int bullet_index = 0; bullet_index < BULLET_MAX; ++bullet_index) {
        bullet_t *bullet = &s_game.bullets[bullet_index];
        if (!bullet->alive) continue;
        for (int enemy_index = 0; enemy_index < ENEMY_MAX; ++enemy_index) {
            enemy_t *enemy = &s_game.enemies[enemy_index];
            if (!enemy->alive || enemy->entry_ms > 0) continue;
            if (overlap(enemy->x, enemy->y, ENEMY_W, ENEMY_H,
                        bullet->x, bullet->y, BULLET_W, BULLET_H)) {
                spawn_boom(enemy->x, enemy->y, ENEMY_W, ENEMY_H);
                register_kill(enemy);
                enemy->alive = false;
                bullet->alive = false;
                break;
            }
        }
    }
}

static void sim_step(int ms)
{
    if (ms < 0) ms = 0;
    if (ms > 48) ms = 48;
    if (s_game.state == ST_IDLE || s_game.state == ST_PLAY) {
        update_stars(ms);
    }
    if (s_game.state != ST_PLAY) return;
    battle_audio_tick(ms);
    if (s_game.invuln_ms > 0) {
        s_game.invuln_ms -= ms;
        if (s_game.invuln_ms < 0) s_game.invuln_ms = 0;
    }
    if (s_game.damage_flash_ms > 0) {
        s_game.damage_flash_ms -= ms;
        if (s_game.damage_flash_ms < 0) s_game.damage_flash_ms = 0;
    }
    if (s_game.haptic_cooldown_ms > 0) {
        s_game.haptic_cooldown_ms -= ms;
        if (s_game.haptic_cooldown_ms < 0) {
            s_game.haptic_cooldown_ms = 0;
        }
    }
    if (s_game.level_notice_ms > 0) {
        s_game.level_notice_ms -= ms;
        if (s_game.level_notice_ms < 0) s_game.level_notice_ms = 0;
    }
    if (s_game.new_best_notice_ms > 0) {
        s_game.new_best_notice_ms -= ms;
        if (s_game.new_best_notice_ms < 0) s_game.new_best_notice_ms = 0;
    }
    const bool combo_frozen = s_game.spawn_pause_ms > 0;
    if (s_game.spawn_pause_ms > 0) {
        s_game.spawn_pause_ms -= ms;
        if (s_game.spawn_pause_ms < 0) s_game.spawn_pause_ms = 0;
    }
    if (s_game.combo_ms > 0 && !combo_frozen) {
        s_game.combo_ms -= ms;
        if (s_game.combo_ms <= 0) reset_combo();
    }
    for (int index = 0; index < BOOM_MAX; ++index) {
        if (s_game.booms[index].alive &&
                (s_game.booms[index].age_ms += ms) >= BOOM_MS) {
            s_game.booms[index].alive = false;
        }
    }
    for (int index = 0; index < SCORE_POP_MAX; ++index) {
        if (s_game.score_pops[index].alive &&
                (s_game.score_pops[index].age_ms += ms) >= SCORE_POP_MS) {
            s_game.score_pops[index].alive = false;
        }
    }
    advance_player(ms);
    s_game.active_ms += ms;
    const difficulty_t difficulty = difficulty_at(s_game.active_ms);
    if (difficulty.level > s_game.level) {
        s_game.level = difficulty.level;
        set_unlock_for_level(s_game.level);
        if (s_game.pending_role == ROLE_GUNNER &&
                alive_gunner_count() >= difficulty.shooter_cap) {
            s_game.pending_role = -1;
        }
        s_game.level_notice_ms = LEVEL_NOTICE_MS;
        s_game.spawn_pause_ms = LEVEL_BREATH_MS;
        s_game.spawn_acc = difficulty.spawn_interval_ms;
        memset(s_game.enemy_bullets, 0, sizeof(s_game.enemy_bullets));
        for (int index = 0; index < ENEMY_MAX; ++index) {
            if (s_game.enemies[index].alive &&
                    s_game.enemies[index].can_shoot) {
                s_game.enemies[index].fire_cooldown_ms =
                    fmaxf(s_game.enemies[index].fire_cooldown_ms,
                          ENEMY_FIRE_TELEGRAPH_MS);
            }
        }
    }
    s_game.exhaust_ms += ms;
    if (s_game.spawn_pause_ms > 0) {
        resolve_player_shot_hits();
        return;
    }

    s_game.fire_acc += ms;
    const int fire_every = s_game.multiplier == 4
        ? OVERDRIVE_FIRE_MS : FIRE_MS;
    if (s_game.fire_acc >= fire_every) {
        s_game.fire_acc -= fire_every;
        fire_player();
    }
    const float step = (float)ms / 1000.0f;
    for (int index = 0; index < BULLET_MAX; ++index) {
        bullet_t *bullet = &s_game.bullets[index];
        if (!bullet->alive) continue;
        bullet->y -= BULLET_SPEED * step;
        if (bullet->y + BULLET_H <= PLAY_TOP - 24) {
            bullet->alive = false;
        }
    }

    s_game.spawn_acc += ms;
    if (alive_enemy_count() >= difficulty.enemy_cap) {
        if (s_game.spawn_acc > difficulty.spawn_interval_ms) {
            s_game.spawn_acc = difficulty.spawn_interval_ms;
        }
    } else if (s_game.spawn_acc >= difficulty.spawn_interval_ms) {
        if (spawn_enemy(&difficulty, s_game.pending_role)) {
            s_game.spawn_acc = fmaxf(
                0.0f, s_game.spawn_acc - difficulty.spawn_interval_ms);
            s_game.pending_role = -1;
        } else {
            s_game.spawn_acc = fmaxf(
                0.0f, difficulty.spawn_interval_ms - 100.0f);
        }
    }

    for (int index = 0; index < ENEMY_MAX; ++index) {
        enemy_t *enemy = &s_game.enemies[index];
        if (!enemy->alive) continue;
        enemy->warning = false;
        if (enemy->entry_ms > 0) {
            enemy->entry_ms -= ms;
            if (enemy->entry_ms < 0) enemy->entry_ms = 0;
            continue;
        }
        if (enemy->role == ROLE_STRIKER) {
            enemy->vy = fminf(
                STRIKER_SPEED_MAX, enemy->vy + enemy->dive_accel * step);
        }
        enemy->y += enemy->vy * step;
        if (enemy->path == PATH_DIAGONAL) {
            enemy->turn_cooldown_ms -= ms;
            if (enemy->turn_cooldown_ms <= 0) {
                enemy->vx = -enemy->vx;
                enemy->turn_cooldown_ms += enemy->turn_every_ms;
            }
            enemy->x += enemy->vx * step;
            if (enemy->x <= PLAYER_MIN_X ||
                    enemy->x >= PLAYER_MAX_X - ENEMY_W) {
                enemy->vx = -enemy->vx;
                enemy->x = clampf(
                    enemy->x, PLAYER_MIN_X, PLAYER_MAX_X - ENEMY_W);
            }
            enemy->heading_deg = atan2f(enemy->vx, enemy->vy) * 57.2957795f;
        } else if (enemy->path == PATH_ARC) {
            enemy->phase += enemy->omega * step;
            enemy->vx = enemy->amp * cosf(enemy->phase) * enemy->omega;
            enemy->x = clampf(
                enemy->origin_x + enemy->amp * sinf(enemy->phase),
                PLAYER_MIN_X, PLAYER_MAX_X - ENEMY_W);
            enemy->heading_deg = atan2f(enemy->vx, enemy->vy) * 57.2957795f;
        }
    }

    /* Player shots resolve first, so a destroyed Gunner cannot fire. */
    resolve_player_shot_hits();

    const float hit_x = s_game.player_x + 8.0f;
    const float hit_y = s_game.player_y + 10.0f;
    const float hit_w = PLAYER_W - 16.0f;
    const float hit_h = PLAYER_H - 18.0f;
    for (int index = 0; index < ENEMY_BULLET_MAX; ++index) {
        enemy_bullet_t *bullet = &s_game.enemy_bullets[index];
        if (!bullet->alive) continue;
        bullet->x += bullet->vx * step;
        bullet->y += bullet->vy * step;
        if (bullet->y >= SCREEN + 24 || bullet->x + ENEMY_BULLET_W <= -24 ||
                bullet->x >= SCREEN + 24) {
            bullet->alive = false;
            continue;
        }
        if (overlap(bullet->x, bullet->y,
                    ENEMY_BULLET_W, ENEMY_BULLET_H,
                    hit_x, hit_y, hit_w, hit_h)) {
            const bool damaged = lose_life();
            bullet->alive = false;
            if (damaged && s_game.state == ST_PLAY) {
                spawn_boom(
                    s_game.player_x, s_game.player_y, PLAYER_W, PLAYER_H);
            }
            if (s_game.state == ST_OVER) return;
        }
    }

    for (int index = 0; index < ENEMY_MAX; ++index) {
        enemy_t *enemy = &s_game.enemies[index];
        if (!enemy->alive || !enemy->can_shoot || enemy->entry_ms > 0) {
            continue;
        }
        const bool far_enough = enemy->y >= ENEMY_FIRE_MIN_Y &&
            enemy->y + ENEMY_H + ENEMY_FIRE_SAFE_GAP_Y < s_game.player_y;
        if (!far_enough) {
            enemy->fire_cooldown_ms = fmaxf(
                enemy->fire_cooldown_ms, ENEMY_FIRE_TELEGRAPH_MS);
            continue;
        }
        enemy->fire_cooldown_ms -= ms;
        enemy->warning =
            enemy->fire_cooldown_ms <= ENEMY_FIRE_TELEGRAPH_MS;
        if (enemy->fire_cooldown_ms <= 0) {
            fire_enemy(enemy, &difficulty);
            enemy->warning = false;
        }
    }

    for (int index = 0; index < ENEMY_MAX; ++index) {
        enemy_t *enemy = &s_game.enemies[index];
        if (!enemy->alive) continue;
        if (enemy->entry_ms <= 0 &&
                overlap(enemy->x + 6.0f, enemy->y + 5.0f,
                        ENEMY_W - 12.0f, ENEMY_H - 10.0f,
                        hit_x, hit_y, hit_w, hit_h)) {
            const bool damaged = lose_life();
            if (damaged && s_game.state == ST_PLAY) {
                spawn_boom(enemy->x, enemy->y, ENEMY_W, ENEMY_H);
            }
            enemy->alive = false;
            if (s_game.state == ST_OVER) return;
        } else if (enemy->y + ENEMY_H >= SCREEN) {
            (void)lose_life();
            enemy->alive = false;
            if (s_game.state == ST_OVER) return;
        }
    }
}

static bool render_star_positions(esp_gsp_handle_t ui)
{
    gsp_component_property_update_t updates[STAR_MAX * 2];
    size_t count = 0;
    for (int index = 0; index < STAR_MAX; ++index) {
        if (s_render.valid && s_game.star_render_group >= 0 &&
                index % STAR_RENDER_GROUPS != s_game.star_render_group) {
            continue;
        }
        const int x = (int)lroundf(s_game.stars[index].x);
        const int y = (int)lroundf(s_game.stars[index].y);
        if (s_render.valid &&
                s_render.star_x[index] == x &&
                s_render.star_y[index] == y) {
            continue;
        }
        updates[count++] = (gsp_component_property_update_t) {
            .component = s_star_obj[index],
            .property = GSP_PROPERTY_KEY_X,
            .value = {.type = GSP_VALUE_I32, .data.i32 = x},
        };
        updates[count++] = (gsp_component_property_update_t) {
            .component = s_star_obj[index],
            .property = GSP_PROPERTY_KEY_Y,
            .value = {.type = GSP_VALUE_I32, .data.i32 = y},
        };
    }
    size_t offset = 0;
    while (offset < count) {
        const size_t remaining = count - offset;
        const size_t chunk = remaining < ESP_GSP_COMPONENT_BATCH_MAX
            ? remaining : ESP_GSP_COMPONENT_BATCH_MAX;
        if (esp_gsp_component_set_properties(
                ui, updates + offset, chunk) != ESP_OK) {
            return false;
        }
        offset += chunk;
    }
    for (int index = 0; index < STAR_MAX; ++index) {
        if (!s_render.valid || s_game.star_render_group < 0 ||
                index % STAR_RENDER_GROUPS ==
                    s_game.star_render_group) {
            s_render.star_x[index] =
                (int)lroundf(s_game.stars[index].x);
            s_render.star_y[index] =
                (int)lroundf(s_game.stars[index].y);
        }
    }
    return true;
}

static void render_gameplay(esp_gsp_handle_t ui)
{
    position_batch_t positions = {0};
    const int px = (int)lroundf(s_game.player_x);
    const int py = (int)lroundf(s_game.player_y);
    const bool player_visible = s_game.state != ST_OVER &&
        (s_game.invuln_ms == 0 || (s_game.invuln_ms / 80) % 2 == 0);
    queue_position_if_changed(
        &positions, GSP_OBJ_KEY_PLAYER, px, py, &s_render.player);
    render_visible_if_changed(
        ui, GSP_BIND_PLAYER_VISIBLE, player_visible,
        &s_render.player.visible);

    const int exhaust_x = px + PLAYER_W / 2 - 8;
    const int exhaust_y = py + PLAYER_H - 4;
    const int exhaust_frame = (s_game.exhaust_ms / 80) & 1;
    static const uint32_t exhaust_obj[2] = {
        GSP_OBJ_KEY_EXHAUST_0, GSP_OBJ_KEY_EXHAUST_1,
    };
    static const uint16_t exhaust_bind[2] = {
        GSP_BIND_EXHAUST_0_VISIBLE, GSP_BIND_EXHAUST_1_VISIBLE,
    };
    for (int frame = 0; frame < 2; ++frame) {
        const bool visible = player_visible && exhaust_frame == frame;
        render_visible_if_changed(
            ui, exhaust_bind[frame], visible,
            &s_render.exhaust[frame].visible);
        if (visible) {
            queue_position_if_changed(
                &positions, exhaust_obj[frame], exhaust_x, exhaust_y,
                &s_render.exhaust[frame]);
        }
    }

    if (s_game.stars_dirty && render_star_positions(ui)) {
        s_game.stars_dirty = false;
    }

    for (int index = 0; index < BULLET_MAX; ++index) {
        const bullet_t *bullet = &s_game.bullets[index];
        render_visible_if_changed(
            ui, s_bullet_bind[index], bullet->alive,
            &s_render.bullets[index].visible);
        if (bullet->alive) {
            queue_position_if_changed(
                &positions, s_bullet_obj[index],
                (int)lroundf(bullet->x), (int)lroundf(bullet->y),
                &s_render.bullets[index]);
        }
    }
    for (int index = 0; index < ENEMY_BULLET_MAX; ++index) {
        const enemy_bullet_t *bullet = &s_game.enemy_bullets[index];
        render_visible_if_changed(
            ui, s_enemy_bullet_bind[index], bullet->alive,
            &s_render.enemy_bullets[index].visible);
        if (bullet->alive) {
            queue_position_if_changed(
                &positions, s_enemy_bullet_obj[index],
                (int)lroundf(bullet->x) - 2,
                (int)lroundf(bullet->y) - 3,
                &s_render.enemy_bullets[index]);
        }
    }
    for (int index = 0; index < ENEMY_MAX; ++index) {
        const enemy_t *enemy = &s_game.enemies[index];
        for (int kind = 0; kind < ENEMY_KINDS; ++kind) {
            const bool visible = enemy->alive && enemy->kind == kind;
            render_visible_if_changed(
                ui, s_enemy_bind[index][kind], visible,
                &s_render.enemies[index][kind].visible);
            if (visible) {
                queue_position_if_changed(
                    &positions, s_enemy_obj[index][kind],
                    (int)lroundf(enemy->x), (int)lroundf(enemy->y),
                    &s_render.enemies[index][kind]);
            }
        }
        const bool warning = enemy->alive && enemy->warning;
        render_visible_if_changed(
            ui, s_warning_bind[index], warning,
            &s_render.warnings[index].visible);
        if (warning) {
            queue_position_if_changed(
                &positions, s_warning_obj[index],
                (int)lroundf(enemy->x) + 13,
                (int)lroundf(enemy->y) + ENEMY_H,
                &s_render.warnings[index]);
        }
    }
    for (int index = 0; index < BOOM_MAX; ++index) {
        const boom_t *boom = &s_game.booms[index];
        int active_frame = boom->alive ? boom->age_ms / BOOM_FRAME_MS : 0;
        if (active_frame > 2) active_frame = 2;
        for (int frame = 0; frame < 3; ++frame) {
            const bool visible = boom->alive && frame == active_frame;
            render_visible_if_changed(
                ui, s_boom_bind[index][frame], visible,
                &s_render.booms[index][frame].visible);
            if (visible) {
                queue_position_if_changed(
                    &positions, s_boom_obj[index][frame],
                    (int)lroundf(boom->x), (int)lroundf(boom->y),
                    &s_render.booms[index][frame]);
            }
        }
    }
    for (int index = 0; index < SCORE_POP_MAX; ++index) {
        const score_pop_t *pop = &s_game.score_pops[index];
        render_visible_if_changed(
            ui, s_score_pop_bind[index], pop->alive,
            &s_render.score_pops[index].visible);
        if (pop->alive) {
            if (!s_render.valid ||
                    s_render.score_pop_value[index] != pop->value) {
                char text[16];
                snprintf(text, sizeof(text), "+%d", pop->value);
                (void)esp_gsp_set_text(
                    ui, s_score_pop_text_bind[index], text);
                s_render.score_pop_value[index] = pop->value;
            }
            queue_position_if_changed(
                &positions, s_score_pop_obj[index],
                (int)lroundf(pop->x),
                (int)lroundf(
                    pop->y - pop->age_ms * 18.0f / SCORE_POP_MS),
                &s_render.score_pops[index]);
        }
    }

    const int muzzle_x = px + PLAYER_W / 2;
    const int aim_x = muzzle_x - 1;
    const int aim_y = py - 82;
    const bool aim_visible = s_game.state == ST_PLAY;
    render_visible_if_changed(
        ui, GSP_BIND_AIM_GUIDE_VISIBLE, aim_visible,
        &s_render.aim.visible);
    if (aim_visible) {
        queue_position_if_changed(
            &positions, GSP_OBJ_KEY_AIM_GUIDE, aim_x, aim_y,
            &s_render.aim);
    }
    render_position_batch(ui, &positions);
}

static uint32_t rail_scaled_color(uint32_t rgb888, int opacity)
{
    const uint32_t red = ((rgb888 >> 16) & 0xFFU) * (uint32_t)opacity / 255U;
    const uint32_t green = ((rgb888 >> 8) & 0xFFU) *
        (uint32_t)opacity / 255U;
    const uint32_t blue = (rgb888 & 0xFFU) * (uint32_t)opacity / 255U;
    return ((red & 0xF8U) << 8) | ((green & 0xFCU) << 3) | (blue >> 3);
}

static void rail_set_module_colors(
    esp_gsp_handle_t ui, int module, uint32_t line_color,
    uint32_t node_color)
{
    for (int side = 0; side < 2; ++side) {
        for (int line = 0; line < 3; ++line) {
            (void)esp_gsp_set_color(
                ui, s_rail_line_bind[side][module][line], line_color);
        }
        (void)esp_gsp_set_color(
            ui, s_rail_node_bind[side][module], node_color);
    }
}

static void rail_set_middle_dashed(esp_gsp_handle_t ui, bool dashed)
{
    for (int side = 0; side < 2; ++side) {
        if (dashed) {
            (void)esp_gsp_set_color(
                ui, s_rail_line_bind[side][1][0], 0);
        }
        for (int segment = 0; segment < RAIL_DASH_MAX; ++segment) {
            (void)esp_gsp_set_visible(
                ui, s_rail_mid_dash_bind[side][segment], dashed);
        }
    }
}

static void render_telemetry_rails(esp_gsp_handle_t ui)
{
    enum {
        RAIL_READY = 0,
        RAIL_PLAY,
        RAIL_PAUSED,
        RAIL_HIT,
        RAIL_OVER,
        RAIL_BEST,
    };
    int style = RAIL_PLAY;
    if (s_game.state == ST_IDLE) {
        style = RAIL_READY;
    } else if (s_game.state == ST_PAUSED) {
        style = RAIL_PAUSED;
    } else if (s_game.state == ST_OVER) {
        style = s_game.new_best ? RAIL_BEST : RAIL_OVER;
    } else if (s_game.damage_flash_ms > 0) {
        style = RAIL_HIT;
    }
    const bool active = s_game.state == ST_PLAY &&
        (s_game.level_notice_ms > 0 || s_game.combo >= 2);
    const int phase = active
        ? (s_game.active_ms % RAIL_ANIMATION_MS) * 3 /
            RAIL_ANIMATION_MS
        : 0;
    const int render_key = style * 4 + (active ? phase + 1 : 0);
    if (render_key == s_rail_render_key) return;
    s_rail_render_key = render_key;

    uint32_t line_rgb = RAIL_GRAY5_RGB;
    uint32_t node_rgb = RAIL_ACCENT_RGB;
    int base_opacity = 173;
    if (style == RAIL_PLAY) {
        base_opacity = 184;
    } else if (style == RAIL_PAUSED) {
        line_rgb = RAIL_GRAY4_RGB;
        base_opacity = 184;
    } else if (style == RAIL_OVER) {
        line_rgb = RAIL_GRAY6_RGB;
        node_rgb = RAIL_GRAY5_RGB;
        base_opacity = 209;
    } else if (style == RAIL_BEST) {
        node_rgb = RAIL_WARN_RGB;
        base_opacity = 230;
    }

    for (int module = 0; module < 3; ++module) {
        uint32_t module_line_rgb = line_rgb;
        uint32_t module_node_rgb = node_rgb;
        int module_opacity = base_opacity;
        if (style == RAIL_READY && module == 1) {
            module_opacity = base_opacity * 97 / 255;
        }
        if (style == RAIL_READY && module == 2) {
            module_node_rgb = RAIL_WARN_RGB;
        }
        if (style == RAIL_HIT) {
            if (module == 1) {
                module_line_rgb = RAIL_RED_RGB;
                module_node_rgb = RAIL_RED_RGB;
            } else {
                module_opacity = base_opacity * 46 / 255;
            }
        }
        if (active && module == 1) {
            static const uint8_t phase_opacity[3] = {97, 150, 202};
            module_line_rgb = RAIL_ACCENT_RGB;
            module_node_rgb = RAIL_WARN_RGB;
            module_opacity = base_opacity * phase_opacity[phase] / 255;
        }
        rail_set_module_colors(
            ui, module,
            rail_scaled_color(module_line_rgb, module_opacity),
            rail_scaled_color(module_node_rgb, module_opacity));
    }

    const bool dashed = style == RAIL_HIT && !active;
    rail_set_middle_dashed(ui, dashed);
    static const int phase_y[3] = {8, 3, -3};
    const int middle_y = 234 + (active ? phase_y[phase] : 0);
    (void)esp_gsp_component_set_position(
        ui, s_rail_mid_obj[0], 17, middle_y);
    (void)esp_gsp_component_set_position(
        ui, s_rail_mid_obj[1], 453, middle_y);
}

static void render_hud(esp_gsp_handle_t ui)
{
    char text[64];
    if (!s_render.valid || s_render.score != s_game.score) {
        snprintf(text, sizeof(text), "%04d", s_game.score);
        (void)esp_gsp_set_text(ui, GSP_BIND_GAME_SCORE, text);
        s_render.score = s_game.score;
    }
    if (!s_render.valid || s_render.best_score != s_game.best_score) {
        snprintf(text, sizeof(text), "%04d", s_game.best_score);
        (void)esp_gsp_set_text(ui, GSP_BIND_GAME_BEST, text);
        s_render.best_score = s_game.best_score;
    }
    if (!s_render.valid || s_render.level != s_game.level) {
        snprintf(text, sizeof(text), "%02d", s_game.level);
        (void)esp_gsp_set_text(ui, GSP_BIND_GAME_LEVEL, text);
        s_render.level = s_game.level;
    }
    static const uint16_t life_bind[3] = {
        GSP_BIND_LIFE_0_VISIBLE,
        GSP_BIND_LIFE_1_VISIBLE,
        GSP_BIND_LIFE_2_VISIBLE,
    };
    if (!s_render.valid || s_render.lives != s_game.lives) {
        for (int index = 0; index < 3; ++index) {
            (void)esp_gsp_set_visible(
                ui, life_bind[index], s_game.lives > index);
        }
        s_render.lives = s_game.lives;
    }
    render_telemetry_rails(ui);
    const int pause_state = s_game.state == ST_PAUSED ? 1 : 0;
    if (!s_render.valid || s_render.pause_state != pause_state) {
        (void)esp_gsp_set_text(
            ui, GSP_BIND_GAME_PAUSE, pause_state ? "▶" : "Ⅱ");
        s_render.pause_state = pause_state;
    }
    if (!s_render.valid ||
            s_render.audio_enabled != (int)s_game.audio_enabled) {
        (void)esp_gsp_set_text(
            ui, GSP_BIND_GAME_AUDIO,
            s_game.audio_enabled ? "♪ ON" : "♪ OFF");
        s_render.audio_enabled = s_game.audio_enabled;
    }
    const difficulty_t difficulty = difficulty_at(s_game.active_ms);
    const int level_progress =
        (int)lroundf(difficulty.level_progress * 100.0f);
    if (!s_render.valid || s_render.level_progress != level_progress) {
        (void)esp_gsp_set_value(
            ui, GSP_BIND_LEVEL_PROGRESS, level_progress);
        s_render.level_progress = level_progress;
    }

    const bool show_combo = s_game.state == ST_PLAY && s_game.combo >= 2;
    render_visible_if_changed(
        ui, GSP_BIND_COMBO_PANEL_VISIBLE, show_combo,
        &s_render.combo_visible);
    if (show_combo) {
        if (!s_render.valid || s_render.combo != s_game.combo) {
            snprintf(text, sizeof(text), "COMBO %d", s_game.combo);
            (void)esp_gsp_set_text(ui, GSP_BIND_COMBO_COUNT, text);
            s_render.combo = s_game.combo;
        }
        if (!s_render.valid || s_render.multiplier != s_game.multiplier) {
            snprintf(text, sizeof(text), "×%d", s_game.multiplier);
            (void)esp_gsp_set_text(
                ui, GSP_BIND_COMBO_MULTIPLIER, text);
            s_render.multiplier = s_game.multiplier;
        }
        const int combo_progress =
            s_game.combo_ms * 100 / COMBO_WINDOW_MS;
        if (!s_render.valid ||
                s_render.combo_progress != combo_progress) {
            (void)esp_gsp_set_value(
                ui, GSP_BIND_COMBO_PROGRESS, combo_progress);
            s_render.combo_progress = combo_progress;
        }
    }
    const bool level_notice =
        s_game.state == ST_PLAY && s_game.level_notice_ms > 0;
    render_visible_if_changed(
        ui, GSP_BIND_LEVEL_NOTICE_VISIBLE, level_notice,
        &s_render.level_notice_visible);
    if (level_notice) {
        if (!s_render.valid ||
                s_render.level_notice_level != s_game.level) {
            snprintf(text, sizeof(text), "LEVEL %02d", s_game.level);
            (void)esp_gsp_set_text(
                ui, GSP_BIND_LEVEL_NOTICE_TITLE, text);
            s_render.level_notice_level = s_game.level;
        }
        if (!s_render.valid ||
                s_render.level_unlock != s_game.unlock_label) {
            (void)esp_gsp_set_text(
                ui, GSP_BIND_LEVEL_NOTICE_UNLOCK,
                s_game.unlock_label != NULL ? s_game.unlock_label : "");
            s_render.level_unlock = s_game.unlock_label;
        }
    }
    render_visible_if_changed(
        ui, GSP_BIND_NEW_BEST_VISIBLE,
        s_game.state == ST_PLAY && s_game.new_best_notice_ms > 0,
        &s_render.new_best_visible);
    const bool flash =
        s_game.state == ST_PLAY && s_game.damage_flash_ms > 0;
    if (!s_render.valid || s_render.flash_visible != flash) {
        (void)esp_gsp_set_visible(
            ui, GSP_BIND_HIT_FLASH_VISIBLE, flash);
        (void)esp_gsp_set_visible(
            ui, GSP_BIND_HIT_FLASH_RIGHT_VISIBLE, flash);
        s_render.flash_visible = flash;
    }
}

static void render_overlay(esp_gsp_handle_t ui)
{
    const bool overlay = s_game.state != ST_PLAY;
    const bool results = s_game.state == ST_OVER;
    render_visible_if_changed(
        ui, GSP_BIND_GAME_OVERLAY_VISIBLE, overlay,
        &s_render.overlay_visible);
    render_visible_if_changed(
        ui, GSP_BIND_GAME_RESULTS_VISIBLE, results,
        &s_render.results_visible);
    if (!overlay) return;
    const bool content_changed = !s_render.valid ||
        s_render.overlay_state != s_game.state ||
        (results && (s_render.result_score != s_game.score ||
            s_render.result_best != s_game.best_score ||
            s_render.result_combo != s_game.max_combo));
    if (!content_changed) return;
    char hint[64];
    char subhint[64];
    const char *title;
    if (s_game.state == ST_IDLE) {
        title = "TAP TO START";
        snprintf(hint, sizeof(hint), "BEST %04d", s_game.best_score);
        snprintf(subhint, sizeof(subhint), "Drag to dodge · Tilt to aim");
    } else if (s_game.state == ST_PAUSED) {
        title = "PAUSED";
        snprintf(hint, sizeof(hint), "TAP TO RESUME");
        snprintf(subhint, sizeof(subhint), "LV %02d · SCORE %04d",
                 s_game.level, s_game.score);
    } else {
        title = s_game.new_best ? "NEW BEST" : "GAME OVER";
        hint[0] = '\0';
        subhint[0] = '\0';
        snprintf(hint, sizeof(hint), "%04d", s_game.score);
        (void)esp_gsp_set_text(ui, GSP_BIND_RESULT_SCORE, hint);
        snprintf(hint, sizeof(hint), "%04d", s_game.best_score);
        (void)esp_gsp_set_text(ui, GSP_BIND_RESULT_BEST, hint);
        snprintf(hint, sizeof(hint), "%d", s_game.max_combo);
        (void)esp_gsp_set_text(ui, GSP_BIND_RESULT_MAX_COMBO, hint);
        hint[0] = '\0';
        s_render.result_score = s_game.score;
        s_render.result_best = s_game.best_score;
        s_render.result_combo = s_game.max_combo;
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_GAME_MESSAGE, title);
    (void)esp_gsp_set_text(ui, GSP_BIND_GAME_HINT, hint);
    (void)esp_gsp_set_text(ui, GSP_BIND_GAME_SUBHINT, subhint);
    s_render.overlay_state = s_game.state;
}

static void render(esp_gsp_handle_t ui)
{
    render_gameplay(ui);
    render_hud(ui);
    render_overlay(ui);
    s_render.valid = true;
}

static void on_pointer(esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    if (!event->data.pointer.pressed ||
            event->data.pointer.y < PLAY_TOP ||
            event->data.pointer.y >= 460) {
        return;
    }
    if (s_game.state == ST_IDLE || s_game.state == ST_OVER) {
        reset_play();
        move_player_target(
            event->data.pointer.x, event->data.pointer.y);
        battle_audio_start();
    } else if (s_game.state == ST_PAUSED) {
        s_game.state = ST_PLAY;
        s_game.target_x = s_game.player_x;
        s_game.target_y = s_game.player_y;
        battle_audio_resume();
    } else {
        move_player_target(
            event->data.pointer.x, event->data.pointer.y);
    }
    render(ui);
}

static void on_call(esp_gsp_handle_t ui, uint16_t action_id)
{
    if (action_id == GSP_ACT_ID_GAME_PAUSE) {
        if (s_game.state == ST_PLAY) {
            s_game.state = ST_PAUSED;
            s_game.target_x = s_game.player_x;
            s_game.target_y = s_game.player_y;
            battle_audio_pause();
        } else if (s_game.state == ST_PAUSED) {
            s_game.state = ST_PLAY;
            battle_audio_resume();
        }
        render(ui);
    } else if (action_id == GSP_ACT_ID_GAME_AUDIO) {
        s_game.audio_enabled = !s_game.audio_enabled;
        if (s_game.audio_enabled) {
            battle_audio_start();
        } else {
            battle_audio_stop();
        }
        save_preferences();
        render_hud(ui);
    }
}

static void air_battle_started(esp_gsp_handle_t ui)
{
    (void)ui;
    load_preferences();
    reset_idle();
    battle_audio_stop();
}

static void air_battle_stopping(esp_gsp_handle_t ui)
{
    (void)ui;
    save_preferences();
    battle_audio_stop();
    if (s_audio != NULL) {
        music_audio_delete(s_audio);
        s_audio = NULL;
    }
}

static void air_battle_event(
    esp_gsp_handle_t ui, const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    if (event == NULL) return;
    switch (event->type) {
    case MOSAIC_EVENT_START:
    case MOSAIC_EVENT_SCENE_CHANGED:
        invalidate_render_cache();
        render(ui);
        break;
    case MOSAIC_EVENT_TIMER:
        sim_step(16);
        render(ui);
        break;
    case MOSAIC_EVENT_POINTER:
        on_pointer(ui, event);
        break;
    case MOSAIC_EVENT_UI_CALL:
        on_call(ui, event->data.call.action_id);
        break;
    case MOSAIC_EVENT_STOP:
    case MOSAIC_EVENT_MODEL_CHANGED:
    default:
        break;
    }
}

const mosaic_app_descriptor_t mosaic_air_battle_app = {
    .id = AIR_BATTLE_APP_ID,
    .launch_action = GSP_ACT_ID_APP_AIR_BATTLE,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .routes = s_air_battle_routes,
    .route_count = sizeof(s_air_battle_routes) /
        sizeof(s_air_battle_routes[0]),
    .routes_without_history = true,
    .name = "air_battle",
    .title = "Sky Shooter",
    .directory = &gsp_obj_directory_air_battle,
    .disable_swipe = true,
    .root_header_in_stack = true,
    .on_started = air_battle_started,
    .on_stopping = air_battle_stopping,
    .on_event = air_battle_event,
};

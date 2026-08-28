/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Breakout: a minimal real-time game showing the Mosaic game-loop pattern on a
 * compiled retained scene. MOSAIC_EVENT_TIMER drives a fixed simulation step;
 * the App owns all state and physics and writes only changed retained
 * properties (paddle/ball position, brick visibility, HUD text) back to GSP.
 *
 * For a production App the simulation (controller) and the GSP writes
 * (presenter) would live in separate files; they are kept together here so the
 * whole loop reads top-to-bottom as one example.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"

#include "breakout_actions.h"
#include "breakout_binds.h"
#include "breakout_objects.h"

#define BREAKOUT_APP_ID       43U

/* Geometry — must match scene/gen_scene.py. */
#define ROWS        4
#define COLS        5
#define BRICK_W     84
#define BRICK_H     24
#define BRICK_GAP   6
#define BRICK_LEFT  18
#define BRICK_TOP   104
#define PADDLE_W    96
#define PADDLE_H    14
#define PADDLE_Y    408
#define BALL_D      16
#define FIELD_TOP   64
#define SCREEN      480

#define PADDLE_MIN_X  BRICK_LEFT
#define PADDLE_MAX_X  (SCREEN - BRICK_LEFT - PADDLE_W)
#define BALL_MIN_X    4
#define BALL_MAX_X    (SCREEN - BALL_D - 4)
#define BALL_MIN_Y    FIELD_TOP
#define BALL_MAX_Y    (SCREEN - BALL_D)

#define BALL_VX0      3.0f
#define BALL_VY0      (-3.4f)
#define START_LIVES   3
#define BRICK_SCORE   10

typedef enum { ST_READY, ST_PLAYING, ST_WON, ST_LOST } game_state_t;

typedef struct {
    game_state_t state;
    float ball_x, ball_y, ball_vx, ball_vy;
    float paddle_x;
    int score, lives, bricks_left;
    bool brick[ROWS][COLS];
    /* Last values pushed to GSP, to avoid redundant setters. */
    int last_ball_x, last_ball_y, last_paddle_x, last_score, last_lives;
    game_state_t last_state;
} breakout_t;

static breakout_t s_game;

static const uint16_t s_brick_bind[ROWS][COLS] = {
    { GSP_BIND_BRICK_0_0_VISIBLE, GSP_BIND_BRICK_0_1_VISIBLE,
      GSP_BIND_BRICK_0_2_VISIBLE, GSP_BIND_BRICK_0_3_VISIBLE,
      GSP_BIND_BRICK_0_4_VISIBLE },
    { GSP_BIND_BRICK_1_0_VISIBLE, GSP_BIND_BRICK_1_1_VISIBLE,
      GSP_BIND_BRICK_1_2_VISIBLE, GSP_BIND_BRICK_1_3_VISIBLE,
      GSP_BIND_BRICK_1_4_VISIBLE },
    { GSP_BIND_BRICK_2_0_VISIBLE, GSP_BIND_BRICK_2_1_VISIBLE,
      GSP_BIND_BRICK_2_2_VISIBLE, GSP_BIND_BRICK_2_3_VISIBLE,
      GSP_BIND_BRICK_2_4_VISIBLE },
    { GSP_BIND_BRICK_3_0_VISIBLE, GSP_BIND_BRICK_3_1_VISIBLE,
      GSP_BIND_BRICK_3_2_VISIBLE, GSP_BIND_BRICK_3_3_VISIBLE,
      GSP_BIND_BRICK_3_4_VISIBLE },
};

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- simulation ---------------------------------------------------------- */

static void reset_ball_on_paddle(void)
{
    s_game.ball_x = s_game.paddle_x + (PADDLE_W - BALL_D) / 2.0f;
    s_game.ball_y = PADDLE_Y - BALL_D;
    s_game.ball_vx = BALL_VX0;
    s_game.ball_vy = BALL_VY0;
}

static void reset_game(void)
{
    s_game.state = ST_READY;
    s_game.score = 0;
    s_game.lives = START_LIVES;
    s_game.bricks_left = ROWS * COLS;
    s_game.paddle_x = (SCREEN - PADDLE_W) / 2.0f;
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            s_game.brick[r][c] = true;
        }
    }
    reset_ball_on_paddle();
}

static bool brick_rect(int r, int c, int *x, int *y)
{
    *x = BRICK_LEFT + c * (BRICK_W + BRICK_GAP);
    *y = BRICK_TOP + r * (BRICK_H + BRICK_GAP);
    return true;
}

static void bounce_off_bricks(esp_gsp_handle_t ui)
{
    const float cx = s_game.ball_x + BALL_D / 2.0f;
    const float cy = s_game.ball_y + BALL_D / 2.0f;
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            if (!s_game.brick[r][c]) {
                continue;
            }
            int bx, by;
            brick_rect(r, c, &bx, &by);
            if (cx >= bx && cx <= bx + BRICK_W &&
                    cy >= by && cy <= by + BRICK_H) {
                s_game.brick[r][c] = false;
                s_game.bricks_left--;
                s_game.score += BRICK_SCORE;
                s_game.ball_vy = -s_game.ball_vy;
                (void)esp_gsp_set_visible(ui, s_brick_bind[r][c], false);
                return; /* one brick per step keeps the bounce stable */
            }
        }
    }
}

static void sim_step(esp_gsp_handle_t ui)
{
    if (s_game.state != ST_PLAYING) {
        return;
    }
    s_game.ball_x += s_game.ball_vx;
    s_game.ball_y += s_game.ball_vy;

    if (s_game.ball_x <= BALL_MIN_X) {
        s_game.ball_x = BALL_MIN_X;
        s_game.ball_vx = -s_game.ball_vx;
    } else if (s_game.ball_x >= BALL_MAX_X) {
        s_game.ball_x = BALL_MAX_X;
        s_game.ball_vx = -s_game.ball_vx;
    }
    if (s_game.ball_y <= BALL_MIN_Y) {
        s_game.ball_y = BALL_MIN_Y;
        s_game.ball_vy = -s_game.ball_vy;
    }

    /* Paddle collision: ball falling and overlapping the paddle top. */
    if (s_game.ball_vy > 0.0f &&
            s_game.ball_y + BALL_D >= PADDLE_Y &&
            s_game.ball_y + BALL_D <= PADDLE_Y + PADDLE_H + 8 &&
            s_game.ball_x + BALL_D >= s_game.paddle_x &&
            s_game.ball_x <= s_game.paddle_x + PADDLE_W) {
        s_game.ball_y = PADDLE_Y - BALL_D;
        s_game.ball_vy = -s_game.ball_vy;
        const float hit = (s_game.ball_x + BALL_D / 2.0f) -
                          (s_game.paddle_x + PADDLE_W / 2.0f);
        s_game.ball_vx = clampf(hit / (PADDLE_W / 2.0f) * 4.0f, -4.5f, 4.5f);
    }

    bounce_off_bricks(ui);

    if (s_game.bricks_left <= 0) {
        s_game.state = ST_WON;
        return;
    }
    if (s_game.ball_y >= BALL_MAX_Y) {
        s_game.lives--;
        if (s_game.lives <= 0) {
            s_game.state = ST_LOST;
        } else {
            s_game.state = ST_READY;
            reset_ball_on_paddle();
        }
    }
}

/* ---- presenter ----------------------------------------------------------- */

static void render(esp_gsp_handle_t ui, bool force)
{
    const int px = (int)(s_game.paddle_x + 0.5f);
    if (force || px != s_game.last_paddle_x) {
        (void)esp_gsp_component_set_position(ui, GSP_OBJ_KEY_PADDLE, px,
                                             PADDLE_Y);
        s_game.last_paddle_x = px;
    }
    const int bx = (int)(s_game.ball_x + 0.5f);
    const int by = (int)(s_game.ball_y + 0.5f);
    if (force || bx != s_game.last_ball_x || by != s_game.last_ball_y) {
        (void)esp_gsp_component_set_position(ui, GSP_OBJ_KEY_BALL, bx, by);
        s_game.last_ball_x = bx;
        s_game.last_ball_y = by;
    }
    if (force || s_game.score != s_game.last_score) {
        char buf[24];
        snprintf(buf, sizeof(buf), "SCORE %d", s_game.score);
        (void)esp_gsp_set_text(ui, GSP_BIND_GAME_SCORE, buf);
        s_game.last_score = s_game.score;
    }
    if (force || s_game.lives != s_game.last_lives) {
        char buf[24];
        snprintf(buf, sizeof(buf), "LIVES %d", s_game.lives);
        (void)esp_gsp_set_text(ui, GSP_BIND_GAME_LIVES, buf);
        s_game.last_lives = s_game.lives;
    }
    if (force || s_game.state != s_game.last_state) {
        const bool overlay = s_game.state != ST_PLAYING;
        (void)esp_gsp_set_visible(ui, GSP_BIND_GAME_OVERLAY_VISIBLE, overlay);
        if (overlay) {
            const char *msg = s_game.state == ST_WON ? "YOU WIN"
                : s_game.state == ST_LOST ? "GAME OVER" : "TAP TO START";
            const char *hint = s_game.state == ST_READY
                ? "Drag to move the paddle" : "Tap to play again";
            (void)esp_gsp_set_text(ui, GSP_BIND_GAME_MESSAGE, msg);
            (void)esp_gsp_set_text(ui, GSP_BIND_GAME_HINT, hint);
        }
        s_game.last_state = s_game.state;
    }
}

static void render_all(esp_gsp_handle_t ui)
{
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            (void)esp_gsp_set_visible(ui, s_brick_bind[r][c],
                                      s_game.brick[r][c]);
        }
    }
    render(ui, true);
}

/* ---- input / lifecycle --------------------------------------------------- */

static void on_pointer(esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    if (event->data.pointer.pressed) {
        /* The bottom system band belongs exclusively to App exit. Never use
         * a swipe beginning there as paddle input or a start-game tap. */
        if (event->data.pointer.y >= 460) {
            return;
        }
        /* Ignore taps in the HUD band; the back chip lives there and is a scene
         * callback (routed to back_action), not paddle input. */
        if (event->data.pointer.y < FIELD_TOP) {
            return;
        }
        const float target = (float)event->data.pointer.x - PADDLE_W / 2.0f;
        s_game.paddle_x = clampf(target, PADDLE_MIN_X, PADDLE_MAX_X);
        if (s_game.state == ST_READY) {
            reset_ball_on_paddle();
            s_game.state = ST_PLAYING;
        } else if (s_game.state == ST_WON || s_game.state == ST_LOST) {
            reset_game();
            render_all(ui);
            return;
        }
        render(ui, false);
    }
}

static void breakout_started(esp_gsp_handle_t ui)
{
    (void)ui;
    reset_game();
}

static void breakout_event(esp_gsp_handle_t ui, const struct mosaic_event *raw)
{
    const mosaic_event_t *event = raw;
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case MOSAIC_EVENT_START:
    case MOSAIC_EVENT_SCENE_CHANGED:
        render_all(ui);
        break;
    case MOSAIC_EVENT_TIMER:
        sim_step(ui);
        render(ui, false);
        break;
    case MOSAIC_EVENT_POINTER:
        on_pointer(ui, event);
        break;
    case MOSAIC_EVENT_STOP:
    case MOSAIC_EVENT_UI_CALL:
    case MOSAIC_EVENT_MODEL_CHANGED:
    default:
        break;
    }
}

const mosaic_app_descriptor_t mosaic_breakout_app = {
    .id = BREAKOUT_APP_ID,
    .launch_action = GSP_ACT_ID_APP_BREAKOUT,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .name = "breakout",
    .title = "Breakout",
    .directory = &gsp_obj_directory_breakout,
    .disable_swipe = true,
    /* Shared Shell owns the same first-level header and exit affordance as
     * Works; the scene starts its game HUD below that header. */
    .root_header_in_stack = false,
    .on_started = breakout_started,
    .on_event = breakout_event,
};

local ROWS = 4
local COLS = 5
local BRICK_W = 84
local BRICK_H = 24
local BRICK_GAP = 6
local BRICK_LEFT = 18
local BRICK_TOP = 104
local PADDLE_W = 96
local PADDLE_H = 14
local PADDLE_Y = 408
local BALL_D = 16
local FIELD_TOP = 64
local SCREEN = 480

local PADDLE_MIN_X = BRICK_LEFT
local PADDLE_MAX_X = SCREEN - BRICK_LEFT - PADDLE_W
local BALL_MIN_X = 4
local BALL_MAX_X = SCREEN - BALL_D - 4
local BALL_MIN_Y = FIELD_TOP
local BALL_MAX_Y = SCREEN - BALL_D

local function clamp(value, minimum, maximum)
    if value < minimum then return minimum end
    if value > maximum then return maximum end
    return value
end

local function reset_ball(state)
    state.ball_x = state.paddle_x + (PADDLE_W - BALL_D) / 2
    state.ball_y = PADDLE_Y - BALL_D
    state.ball_vx = 3.0
    state.ball_vy = -3.4
end

local function reset_game(state)
    state.mode = "ready"
    state.score = 0
    state.lives = 3
    state.bricks_left = ROWS * COLS
    state.paddle_x = (SCREEN - PADDLE_W) / 2
    state.bricks = {}
    for row = 1, ROWS do
        state.bricks[row] = {}
        for column = 1, COLS do
            state.bricks[row][column] = true
        end
    end
    reset_ball(state)
    return state
end

local function new_game()
    return reset_game({})
end

local function bounce_off_bricks(state)
    local center_x = state.ball_x + BALL_D / 2
    local center_y = state.ball_y + BALL_D / 2
    for row = 1, ROWS do
        for column = 1, COLS do
            if state.bricks[row][column] then
                local brick_x = BRICK_LEFT + (column - 1) *
                    (BRICK_W + BRICK_GAP)
                local brick_y = BRICK_TOP + (row - 1) *
                    (BRICK_H + BRICK_GAP)
                if center_x >= brick_x and center_x <= brick_x + BRICK_W and
                        center_y >= brick_y and
                        center_y <= brick_y + BRICK_H then
                    state.bricks[row][column] = false
                    state.bricks_left = state.bricks_left - 1
                    state.score = state.score + 10
                    state.ball_vy = -state.ball_vy
                    return
                end
            end
        end
    end
end

local function simulate(state)
    if state.mode ~= "playing" then return state end

    state.ball_x = state.ball_x + state.ball_vx
    state.ball_y = state.ball_y + state.ball_vy

    if state.ball_x <= BALL_MIN_X then
        state.ball_x = BALL_MIN_X
        state.ball_vx = -state.ball_vx
    elseif state.ball_x >= BALL_MAX_X then
        state.ball_x = BALL_MAX_X
        state.ball_vx = -state.ball_vx
    end
    if state.ball_y <= BALL_MIN_Y then
        state.ball_y = BALL_MIN_Y
        state.ball_vy = -state.ball_vy
    end

    if state.ball_vy > 0 and
            state.ball_y + BALL_D >= PADDLE_Y and
            state.ball_y + BALL_D <= PADDLE_Y + PADDLE_H + 8 and
            state.ball_x + BALL_D >= state.paddle_x and
            state.ball_x <= state.paddle_x + PADDLE_W then
        state.ball_y = PADDLE_Y - BALL_D
        state.ball_vy = -state.ball_vy
        local hit = (state.ball_x + BALL_D / 2) -
            (state.paddle_x + PADDLE_W / 2)
        state.ball_vx = clamp(hit / (PADDLE_W / 2) * 4.0, -4.5, 4.5)
    end

    bounce_off_bricks(state)
    if state.bricks_left <= 0 then
        state.mode = "won"
    elseif state.ball_y >= BALL_MAX_Y then
        state.lives = state.lives - 1
        if state.lives <= 0 then
            state.mode = "lost"
        else
            state.mode = "ready"
            reset_ball(state)
        end
    end
    return state
end

local function handle_pointer(state, event)
    if event.y >= 460 or event.y < FIELD_TOP then return state end
    state.paddle_x = clamp(event.x - PADDLE_W / 2,
        PADDLE_MIN_X, PADDLE_MAX_X)
    if state.mode == "ready" then
        reset_ball(state)
        state.mode = "playing"
    elseif state.mode == "won" or state.mode == "lost" then
        reset_game(state)
    end
    return state
end

local bindings = {
    { component = "paddle", property = "x",
      select = function(state) return math.floor(state.paddle_x + 0.5) end },
    { component = "ball", property = "x",
      select = function(state) return math.floor(state.ball_x + 0.5) end },
    { component = "ball", property = "y",
      select = function(state) return math.floor(state.ball_y + 0.5) end },
    { component = "game_score", property = "text",
      select = function(state) return "SCORE " .. state.score end },
    { component = "game_lives", property = "text",
      select = function(state) return "LIVES " .. state.lives end },
    { component = "game_overlay", property = "visible",
      select = function(state) return state.mode ~= "playing" end },
    { component = "game_message", property = "text",
      select = function(state)
          if state.mode == "won" then return "YOU WIN" end
          if state.mode == "lost" then return "GAME OVER" end
          return "TAP TO START"
      end },
    { component = "game_hint", property = "text",
      select = function(state)
          if state.mode == "ready" then return "Drag to move the paddle" end
          return "Tap to play again"
      end },
}

for row = 1, ROWS do
    for column = 1, COLS do
        local selected_row = row
        local selected_column = column
        bindings[#bindings + 1] = {
            component = string.format("brick_%d_%d", row - 1, column - 1),
            property = "visible",
            select = function(state)
                return state.bricks[selected_row][selected_column]
            end,
        }
    end
end

return {
    initial_state = new_game(),
    reducer = function(state, event)
        if event.type == "TIMER" then
            return simulate(state)
        end
        if event.type == "POINTER_DOWN" then
            return handle_pointer(state, event)
        end
        return state
    end,
    bindings = bindings,
}

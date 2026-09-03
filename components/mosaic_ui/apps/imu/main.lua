local function clamp(value, minimum, maximum)
    if value < minimum then return minimum end
    if value > maximum then return maximum end
    return value
end

local function axis_position(degrees, invert, minimum, maximum)
    local value = invert and -degrees or degrees
    value = clamp(value, -30.0, 30.0)
    return math.floor(minimum + (value + 30.0) *
        (maximum - minimum) / 60.0 + 0.5)
end

local function next_state(state)
    local sample = mosaic.capability.read("sensor.imu")
    if sample == nil then return state end
    local pitch = sample.pitch_deg
    local roll = sample.roll_deg
    local yaw = sample.yaw_deg
    local tilt = math.sqrt(pitch * pitch + roll * roll)
    return {
        bubble_x = axis_position(roll, false, 130, 270),
        bubble_y = axis_position(pitch, true, 90, 240),
        angle = string.format("%.0f°", tilt),
        pitch = string.format("%.0f", pitch),
        roll = string.format("%.0f", roll),
        yaw = string.format("%.0f", yaw),
    }
end

return {
    initial_state = {
        bubble_x = 202, bubble_y = 168, angle = "0°",
        pitch = "0", roll = "0", yaw = "0",
    },
    reducer = function(state, event)
        if event.type == "START" or event.type == "TIMER" then
            return next_state(state)
        end
        return state
    end,
    bindings = {
        { component = "imu_bubble", property = "x",
          select = function(s) return s.bubble_x end },
        { component = "imu_bubble", property = "y",
          select = function(s) return s.bubble_y end },
        { component = "imu_angle", property = "text",
          select = function(s) return s.angle end },
        { component = "imu_pitch", property = "text",
          select = function(s) return s.pitch end },
        { component = "imu_roll", property = "text",
          select = function(s) return s.roll end },
        { component = "imu_yaw", property = "text",
          select = function(s) return s.yaw end },
    },
}

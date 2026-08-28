-- --------------------------------------------------------------
-- LVGL photo album for browsing images from the DATA root.
-- --------------------------------------------------------------

-- 1. Requires
local arg_schema = require("arg_schema")
local image = require("image")
local lvgl = require("lvgl")
local storage = require("storage")
local system = require("system")

-- 2. Constants
local TAG = "[album_app]"
local DEFAULT_DIR = "photos"
local DEFAULT_RUN_TIME_MS = 180000
local IMAGE_EXTS = {
    [".jpg"] = true,
    [".jpeg"] = true,
    [".bin"] = true,
}
local MAX_FULL_PIXELS = 600000
local MAX_THUMB_PIXELS = 4096
local GRID_GAP = 8
local MAX_SCAN_DEPTH = 12
local EVENT_POLL_MS = 20

-- 3. Args
local ARG_SCHEMA = {
    run_time_ms = arg_schema.int({ default = DEFAULT_RUN_TIME_MS, min = 1000 }),
}

local ctx = arg_schema.parse(args, ARG_SCHEMA)
ctx.dir = type(args) == "table" and type(args.dir) == "string" and args.dir or DEFAULT_DIR

-- 4. State
local lvgl_started = false
local width = 0
local height = 0
local root_dir = nil
local screen = nil
local image_files = {}
local current_index = 1
local view_mode = "grid"
local grid_generation = 0
local thumbnail_jobs = {}

-- 5. Helpers
local function reject_path_part(name, value)
    if type(value) ~= "string" then
        error(name .. " must be a string")
    end
    if value == "" then
        error(name .. " must not be empty")
    end
    if string.find(value, "%.%.", 1, false) or string.find(value, "/", 1, true) or string.find(value, "\\", 1, true) then
        error(name .. " must be a single path segment")
    end
end

local function basename(path)
    return string.match(path, "([^/]+)$") or path
end

local function lower_suffix(name)
    local lower = string.lower(name or "")
    return string.match(lower, "(%.[^.]+)$") or ""
end

local function is_jpeg_name(name)
    local ext = lower_suffix(name)
    return ext == ".jpg" or ext == ".jpeg"
end

local function is_image_file(entry)
    return type(entry) == "table" and entry.type == "file" and IMAGE_EXTS[lower_suffix(entry.name)] == true
end

local function join_rel_path(parent, name)
    if parent == "" then
        return name
    end
    return storage.join_path(parent, name)
end

local function data_abs_to_lvgl(path)
    if root_dir and string.sub(path, 1, #root_dir) == root_dir then
        local rel = string.sub(path, #root_dir + 1)
        while string.sub(rel, 1, 1) == "/" do
            rel = string.sub(rel, 2)
        end
        return "D:/" .. rel
    end
    return path
end

local function cleanup()
    if lvgl_started then
        local ok, err = pcall(lvgl.deinit)
        if not ok then
            print(TAG .. " WARN: lvgl.deinit failed: " .. tostring(err))
        end
        lvgl_started = false
    end
end

local function scan_image_dir(dir_path, rel_dir, files, depth)
    if depth > MAX_SCAN_DEPTH then
        print(TAG .. " WARN: skip deep album directory: " .. dir_path)
        return
    end

    local ok, entries_or_err = pcall(storage.listdir, dir_path)
    if not ok then
        print(TAG .. " WARN: list album directory failed: " .. dir_path .. ": " .. tostring(entries_or_err))
        return
    end

    for _, entry in ipairs(entries_or_err) do
        if type(entry) == "table" and type(entry.name) == "string" then
            local entry_path = storage.join_path(dir_path, entry.name)
            local rel_path = join_rel_path(rel_dir, entry.name)

            if entry.type == "dir" then
                -- Recurse into child albums so dated camera folders are shown in the same grid.
                scan_image_dir(entry_path, rel_path, files, depth + 1)
            elseif is_image_file(entry) then
                files[#files + 1] = {
                    name = rel_path,
                    path = entry_path,
                    size = entry.size or 0,
                    mtime = entry.mtime or 0,
                }
            end
        end
    end
end

local function scan_images()
    reject_path_part("dir", ctx.dir)

    root_dir = storage.get_root_dir()
    local album_dir = storage.join_path(root_dir, ctx.dir)
    local st, stat_err = storage.stat(album_dir)
    if not st then
        print(TAG .. " WARN: album directory not found: " .. tostring(stat_err))
        return {}
    end
    if st.type ~= "dir" then
        error("album path is not a directory: " .. album_dir)
    end

    local files = {}
    scan_image_dir(album_dir, "", files, 0)

    table.sort(files, function(a, b)
        if a.mtime == b.mtime then
            return a.name < b.name
        end
        return a.mtime > b.mtime
    end)
    return files
end

local function fit_size(src_w, src_h, max_w, max_h, max_pixels)
    if src_w <= 0 or src_h <= 0 or max_w <= 0 or max_h <= 0 then
        return 1, 1
    end

    local scale = math.min(max_w / src_w, max_h / src_h, 1.0)
    local dst_w = math.max(1, math.floor(src_w * scale))
    local dst_h = math.max(1, math.floor(src_h * scale))

    while dst_w * dst_h > max_pixels do
        dst_w = math.max(1, math.floor(dst_w * 0.9))
        dst_h = math.max(1, math.floor(dst_h * 0.9))
    end
    return dst_w, dst_h
end

local function draw_rgb565_canvas(parent, pixels, dst_w, dst_h)
    local canvas = lvgl.canvas(parent, {
        w = dst_w,
        h = dst_h,
        color_format = "rgb565",
        align = "center",
    })
    -- image.resize returns packed RGB565 little-endian pixels; write the whole buffer in C.
    canvas:set_rgb565_data(pixels, "le")
    return canvas
end

local function draw_jpeg_canvas(parent, file, max_w, max_h, max_pixels)
    local frame <close> = image.load_file(file.path)
    local info = frame:info()
    local dst_w, dst_h = fit_size(info.width, info.height, max_w, max_h, max_pixels)
    local resized <close> = image.resize(frame, { width = dst_w, height = dst_h, filter = "bilinear" })
    local canvas = draw_rgb565_canvas(parent, resized:data(), dst_w, dst_h)
    return string.format("%dx%d -> %dx%d", info.width, info.height, dst_w, dst_h), canvas
end

local function draw_lvgl_file_image(parent, file)
    local obj = lvgl.image(parent, {
        src = data_abs_to_lvgl(file.path),
        align = "center",
    })
    obj:set_style({ radius = 0 })
    return "LVGL image", obj
end

local show_grid
local show_fullscreen

local function show_error(parent, message)
    lvgl.label(parent, {
        text = message,
        align = "center",
        text_color = "#ffb4a8",
    })
end

local function draw_picture(parent, file, max_w, max_h, max_pixels)
    if is_jpeg_name(file.name) then
        return draw_jpeg_canvas(parent, file, max_w, max_h, max_pixels)
    end
    return draw_lvgl_file_image(parent, file)
end

local function draw_empty_grid()
    lvgl.label(screen, {
        text = "No images",
        align = "center",
        y = -12,
        text_color = "#f3f6fa",
    })
    lvgl.label(screen, {
        text = "Put JPEG photos in " .. ctx.dir .. "/",
        align = "center",
        y = 18,
        text_color = "#aeb8c4",
    })
end

local function delete_current_photo()
    if current_index < 1 or current_index > #image_files then
        print(TAG .. " ERROR: delete requested with invalid index: " .. tostring(current_index))
        show_grid()
        return
    end

    local file = image_files[current_index]
    local ok, err = pcall(storage.remove, file.path)
    if not ok then
        print(TAG .. " ERROR: delete failed for " .. tostring(file.path) .. ": " .. tostring(err))
        return
    end

    print(TAG .. " deleted photo: " .. tostring(file.path))
    table.remove(image_files, current_index)
    if #image_files == 0 then
        current_index = 1
        show_grid()
        return
    end
    if current_index > #image_files then
        current_index = #image_files
    end
    show_fullscreen(current_index)
end

local function show_delete_confirm(file)
    local msg = lvgl.msgbox(screen, {
        title = "Delete photo?",
        text = basename(file.name),
    })
    msg:set_size(math.min(width - 48, 360), 150)
    msg:align("center")

    local cancel = msg:add_footer_button("Cancel")
    local delete = msg:add_footer_button("Delete")
    delete:set_style({
        bg_color = "#a52a2a",
        bg_opa = 255,
        text_color = "#ffffff",
    })

    cancel:on("clicked", function()
        msg:close()
    end)
    delete:on("clicked", function()
        msg:close()
        delete_current_photo()
    end)
end

local function clear_thumbnail_jobs()
    thumbnail_jobs = {}
end

local function queue_thumbnail_job(tile, file, index, tile_w, tile_h, generation)
    thumbnail_jobs[#thumbnail_jobs + 1] = {
        tile = tile,
        file = file,
        index = index,
        tile_w = tile_w,
        tile_h = tile_h,
        generation = generation,
    }
end

local function pump_thumbnail_jobs(max_count)
    local processed = 0

    while view_mode == "grid" and #thumbnail_jobs > 0 and processed < max_count do
        local job = table.remove(thumbnail_jobs, 1)
        processed = processed + 1

        if job.generation == grid_generation and job.tile and job.tile:is_valid() then
            local ok, err = pcall(function()
                -- Remove the placeholder only when the target tile is still part of the active grid.
                job.tile:clean()
                local _, pic_obj = draw_picture(job.tile, job.file, job.tile_w - 8, job.tile_h - 8, MAX_THUMB_PIXELS)
                if pic_obj then
                    pic_obj:on("clicked", function()
                        show_fullscreen(job.index)
                    end)
                end
            end)
            if not ok then
                print(TAG .. " WARN: thumbnail failed for " .. tostring(job.file.path) .. ": " .. tostring(err))
                if job.generation == grid_generation and view_mode == "grid" and job.tile:is_valid() then
                    job.tile:clean()
                    show_error(job.tile, "Load failed")
                end
            end
        end
    end
end

show_grid = function()
    view_mode = "grid"
    grid_generation = grid_generation + 1
    clear_thumbnail_jobs()
    screen:clean()
    screen:set_style({ bg_color = "#0b0f14" })

    lvgl.label(screen, {
        text = "Album",
        x = 10,
        y = 8,
        text_color = "#f3f6fa",
    })
    lvgl.label(screen, {
        text = tostring(#image_files) .. " photos",
        align = "top_right",
        x = -10,
        y = 10,
        text_color = "#aeb8c4",
    })

    if #image_files == 0 then
        draw_empty_grid()
        return
    end

    local grid = lvgl.container(screen, {
        x = 8,
        y = 42,
        w = width - 16,
        h = height - 50,
        bg_opa = 0,
        border_width = 0,
        pad = 0,
        pad_row = GRID_GAP,
        pad_column = GRID_GAP,
    })
    grid:set_flex({ flow = "row_wrap", main = "start", cross = "start", track = "start" })
    grid:set_scroll({ dir = "ver", scrollbar = "auto" })

    local columns = math.max(2, math.floor((width - 16 + GRID_GAP) / 96))
    local tile_w = math.max(62, ((width - 16) - GRID_GAP * (columns - 1)) // columns)
    local tile_h = math.max(58, math.floor(tile_w * 0.78))

    for i, file in ipairs(image_files) do
        local tile = lvgl.button(grid, {
            w = tile_w,
            h = tile_h,
            radius = 4,
            bg_color = "#111820",
            bg_opa = 255,
            border_color = "#273442",
            border_width = 1,
            text_color = "#ffffff",
        })
        tile:on("clicked", function()
            show_fullscreen(i)
        end)

        lvgl.label(tile, {
            text = "...",
            align = "center",
            text_color = "#aeb8c4",
        })
        queue_thumbnail_job(tile, file, i, tile_w, tile_h, grid_generation)
    end
end

show_fullscreen = function(index)
    if #image_files == 0 then
        show_grid()
        return
    end

    view_mode = "fullscreen"
    grid_generation = grid_generation + 1
    clear_thumbnail_jobs()
    current_index = ((index - 1) % #image_files) + 1
    local file = image_files[current_index]
    screen:clean()
    screen:set_style({ bg_color = "#030609" })

    local back = lvgl.button(screen, {
        x = 0,
        y = 0,
        w = width,
        h = height,
        bg_color = "#030609",
        bg_opa = 255,
        border_width = 0,
        text_color = "#ffffff",
    })
    back:on("clicked", function()
        show_grid()
    end)

    local picture = lvgl.container(screen, {
        x = 8,
        y = 8,
        w = width - 16,
        h = height - 16,
        bg_opa = 0,
        border_width = 0,
        pad = 0,
    })
    picture:on("clicked", function()
        show_grid()
    end)

    local ok, detail_or_err, pic_obj = pcall(function()
        return draw_picture(picture, file, width - 20, height - 42, MAX_FULL_PIXELS)
    end)
    if not ok then
        print(TAG .. " ERROR: image load failed for " .. tostring(file.path) .. ": " .. tostring(detail_or_err))
        show_error(picture, "Image load failed")
        detail_or_err = "Unsupported or damaged image"
    elseif pic_obj then
        pic_obj:on("clicked", function()
            show_grid()
        end)
    end

    local title_bg = lvgl.container(screen, {
        x = 0,
        y = 0,
        w = width,
        h = 34,
        bg_color = "#030609",
        bg_opa = 180,
        border_width = 0,
        pad = 6,
    })
    title_bg:on("clicked", function()
        show_grid()
    end)
    local title_name = lvgl.label(title_bg, {
        text = basename(file.name),
        align = "left_mid",
        x = 4,
        text_color = "#f3f6fa",
    })
    title_name:on("clicked", function()
        show_grid()
    end)
    local title_counter = lvgl.label(title_bg, {
        text = tostring(current_index) .. " / " .. tostring(#image_files),
        align = "right_mid",
        x = -4,
        text_color = "#aeb8c4",
    })
    title_counter:on("clicked", function()
        show_grid()
    end)

    local status_bg = lvgl.container(screen, {
        x = 0,
        y = height - 24,
        w = width,
        h = 24,
        bg_color = "#030609",
        bg_opa = 160,
        border_width = 0,
        pad = 4,
    })
    status_bg:on("clicked", function()
        show_grid()
    end)
    local status_text = lvgl.label(status_bg, {
        text = tostring(detail_or_err),
        align = "center",
        text_color = "#aeb8c4",
    })
    status_text:on("clicked", function()
        show_grid()
    end)

    local prev = lvgl.button(screen, {
        text = "<",
        align = "left_mid",
        x = 12,
        y = 0,
        w = 64,
        h = 64,
        radius = 32,
        bg_color = "#ffffff",
        bg_opa = 115,
        text_color = "#1b2733",
    })
    local next = lvgl.button(screen, {
        text = ">",
        align = "right_mid",
        x = -12,
        y = 0,
        w = 64,
        h = 64,
        radius = 32,
        bg_color = "#ffffff",
        bg_opa = 115,
        text_color = "#1b2733",
    })
    local delete_btn = lvgl.button(screen, {
        text = "Delete",
        align = "top_right",
        x = -8,
        y = 42,
        w = 72,
        h = 34,
        radius = 6,
        bg_color = "#8f2d2d",
        bg_opa = 235,
        text_color = "#ffffff",
    })

    prev:on("clicked", function()
        show_fullscreen(current_index - 1)
    end)
    next:on("clicked", function()
        show_fullscreen(current_index + 1)
    end)
    delete_btn:on("clicked", function()
        show_delete_confirm(file)
    end)
end

-- 6. Run
local function run()
    lvgl.init({
        buffer_lines = 40,
        tick_ms = 5,
        task_period_ms = 10,
    })
    lvgl_started = true

    screen = lvgl.create_screen()
    width, height = screen:get_size()
    if width <= 0 or height <= 0 then
        error(string.format("invalid display size: %dx%d", width, height))
    end
    -- display_service auto-wires the system touch input in EXCLUSIVE_LVGL mode.

    image_files = scan_images()
    print(string.format("%s found %d image(s) under %s", TAG, #image_files, ctx.dir))

    screen:load()
    show_grid()

    local deadline_ms = system.millis() + ctx.run_time_ms
    while system.millis() < deadline_ms do
        lvgl.process_events(EVENT_POLL_MS)
        pump_thumbnail_jobs(1)
    end
    print(string.format("%s done images=%d", TAG, #image_files))
end

-- 7. Epilogue
local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    print(TAG .. " ERROR: " .. tostring(err))
    error(err)
end

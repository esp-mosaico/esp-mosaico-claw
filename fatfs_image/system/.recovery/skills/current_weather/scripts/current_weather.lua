local capability = require("capability")
local json = require("json")

local DEFAULT_TIMEOUT_MS = 15000
local IP_LOCATION_URL = "http://ip-api.com/json/?fields=status,message,query,lat,lon,city,regionName,countryCode,timezone&lang=en"
local IP_LOCATION_MAX_BODY_BYTES = 2048
local WEATHER_MAX_BODY_BYTES = 12288

local a = type(args) == "table" and args or {}

local WEATHER_CODES = {
    [0] = "Clear sky",
    [1] = "Mainly clear",
    [2] = "Partly cloudy",
    [3] = "Overcast",
    [45] = "Fog",
    [48] = "Depositing rime fog",
    [51] = "Light drizzle",
    [53] = "Moderate drizzle",
    [55] = "Dense drizzle",
    [56] = "Light freezing drizzle",
    [57] = "Dense freezing drizzle",
    [61] = "Slight rain",
    [63] = "Moderate rain",
    [65] = "Heavy rain",
    [66] = "Light freezing rain",
    [67] = "Heavy freezing rain",
    [71] = "Slight snow fall",
    [73] = "Moderate snow fall",
    [75] = "Heavy snow fall",
    [77] = "Snow grains",
    [80] = "Slight rain showers",
    [81] = "Moderate rain showers",
    [82] = "Violent rain showers",
    [85] = "Slight snow showers",
    [86] = "Heavy snow showers",
    [95] = "Thunderstorm",
    [96] = "Thunderstorm with slight hail",
    [99] = "Thunderstorm with heavy hail",
}

local function trim(value)
    return tostring(value or ""):gsub("^%s+", ""):gsub("%s+$", "")
end

local function validated_number(key, value, min_value, max_value)
    if type(value) ~= "number" or value ~= value or value == math.huge or value == -math.huge then
        error(key .. " must be a finite number")
    end
    if value < min_value or value > max_value then
        error(key .. " must be between " .. tostring(min_value) .. " and " .. tostring(max_value))
    end
    return value
end

local function selected_location()
    if type(a.location) == "string" and trim(a.location) ~= "" then
        return trim(a.location)
    end
    return nil
end

local function selected_timezone()
    if type(a.timezone) == "string" and trim(a.timezone) ~= "" then
        local value = trim(a.timezone)
        if not value:match("^[%w_/%+%-]+$") then
            error("timezone contains unsupported characters")
        end
        return value
    end
    return nil
end

local function request_timeout()
    local value = a.timeout_ms
    if type(value) ~= "number" then
        return DEFAULT_TIMEOUT_MS
    end
    if value ~= value or value == math.huge or value == -math.huge then
        error("timeout_ms must be a finite number")
    end
    value = math.floor(value)
    if value < 1 then
        return 1
    end
    if value > 120000 then
        return 120000
    end
    return value
end

local function string_arg(key, default)
    local value = a[key]
    if type(value) == "string" and value ~= "" then
        return value
    end
    return default
end

local function build_opts()
    local opts = {
        source_cap = string_arg("source_cap", "current_weather"),
    }

    local session_id = string_arg("session_id", nil)
    if session_id then
        opts.session_id = session_id
    end

    return opts
end

local function split_http_response(out)
    local status_text, body = tostring(out or ""):match("^(HTTP [^\n]*)\n(.*)$")
    if not status_text then
        return nil, nil
    end

    local status = tonumber(status_text:match("^HTTP%s+(%d+)"))
    return status, body or ""
end

local function decode_json_body(body)
    local ok, data = pcall(json.decode, body or "")
    if not ok or type(data) ~= "table" then
        return nil
    end
    return data
end

local function open_meteo_error(status, body)
    local data = decode_json_body(body)
    if data and type(data.reason) == "string" then
        return string.format("Open-Meteo API failed: status=%d reason=%s", status or -1, data.reason)
    end
    return string.format("Open-Meteo API failed: status=%s", tostring(status))
end

local function http_get_json(url, service_name, max_body_bytes)
    local ok, out, err = capability.call("http_request", {
        url = url,
        method = "GET",
        headers = {
            Accept = "application/json",
            ["User-Agent"] = "esp-clawgent-lua-capability",
        },
        timeout_ms = request_timeout(),
        max_body_bytes = max_body_bytes,
    }, build_opts())

    if not ok then
        error(string.format("%s request failed: %s", service_name, tostring(err or out)))
    end

    local status, body = split_http_response(out)
    if not status then
        error(service_name .. " returned an unexpected response")
    end
    if status ~= 200 then
        if service_name == "Open-Meteo API" then
            error(open_meteo_error(status, body))
        end
        error(string.format("%s failed: status=%d", service_name, status))
    end

    local data = decode_json_body(body)
    if not data then
        error(service_name .. " returned invalid JSON")
    end
    return data
end

local function optional_string(value)
    if type(value) == "string" and trim(value) ~= "" then
        return trim(value)
    end
    return nil
end

local function location_label(city, region, country_code, latitude, longitude)
    return selected_location()
        or city
        or region
        or country_code
        or string.format("%.4f, %.4f", latitude, longitude)
end

local function resolve_location()
    local has_latitude = a.latitude ~= nil
    local has_longitude = a.longitude ~= nil
    if has_latitude ~= has_longitude then
        error("latitude and longitude must be provided together")
    end

    if has_latitude then
        local latitude = validated_number("latitude", a.latitude, -90, 90)
        local longitude = validated_number("longitude", a.longitude, -180, 180)
        return {
            latitude = latitude,
            longitude = longitude,
            location = location_label(nil, nil, nil, latitude, longitude),
            timezone = selected_timezone() or "auto",
            location_source = "explicit",
        }
    end

    local data = http_get_json(IP_LOCATION_URL, "IP geolocation API", IP_LOCATION_MAX_BODY_BYTES)
    if data.status ~= "success" then
        error("IP geolocation API failed: " .. tostring(data.message or "unknown error"))
    end

    local latitude = validated_number("IP geolocation latitude", data.lat, -90, 90)
    local longitude = validated_number("IP geolocation longitude", data.lon, -180, 180)
    local city = optional_string(data.city)
    local region = optional_string(data.regionName)
    local country_code = optional_string(data.countryCode)
    local detected_timezone = optional_string(data.timezone)
    if detected_timezone and not detected_timezone:match("^[%w_/%+%-]+$") then
        detected_timezone = nil
    end

    return {
        latitude = latitude,
        longitude = longitude,
        location = location_label(city, region, country_code, latitude, longitude),
        city = city,
        region = region,
        country_code = country_code,
        timezone = selected_timezone() or detected_timezone or "auto",
        location_source = "ip",
    }
end

local function first_array_value(value)
    if type(value) == "table" then
        return value[1]
    end
    return nil
end

local function build_url(latitude, longitude, timezone)
    return string.format(
        "https://api.open-meteo.com/v1/forecast?latitude=%.6f&longitude=%.6f&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max&timezone=%s&forecast_days=1",
        latitude,
        longitude,
        timezone
    )
end

local function run()
    local resolved = resolve_location()
    local data = http_get_json(
        build_url(resolved.latitude, resolved.longitude, resolved.timezone),
        "Open-Meteo API",
        WEATHER_MAX_BODY_BYTES
    )
    if type(data.current) ~= "table" then
        error("Open-Meteo API response missing current weather")
    end

    local current = data.current
    local daily = type(data.daily) == "table" and data.daily or {}
    local code = current.weather_code
    local result = {
        ok = true,
        location = resolved.location,
        location_source = resolved.location_source,
        city = resolved.city,
        region = resolved.region,
        country_code = resolved.country_code,
        latitude = resolved.latitude,
        longitude = resolved.longitude,
        timezone = data.timezone or resolved.timezone,
        time = current.time,
        temperature_c = current.temperature_2m,
        apparent_temperature_c = current.apparent_temperature,
        relative_humidity_pct = current.relative_humidity_2m,
        wind_speed_kmh = current.wind_speed_10m,
        weather_code = code,
        condition = WEATHER_CODES[code] or ("Weather code " .. tostring(code)),
        rain_probability_pct = first_array_value(daily.precipitation_probability_max),
        daily_high_c = first_array_value(daily.temperature_2m_max),
        daily_low_c = first_array_value(daily.temperature_2m_min),
    }

    if type(result.temperature_c) ~= "number" then
        error("Open-Meteo API response missing temperature_2m")
    end

    print(string.format(
        "[current_weather] %s temp=%sC apparent=%sC condition=%s humidity=%s%% wind=%skm/h rain_prob=%s%% time=%s",
        result.location,
        tostring(result.temperature_c),
        tostring(result.apparent_temperature_c),
        result.condition,
        tostring(result.relative_humidity_pct),
        tostring(result.wind_speed_kmh),
        tostring(result.rain_probability_pct),
        tostring(result.time)
    ))
    print(json.encode(result))
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
    print("[current_weather] ERROR: " .. tostring(err))
    error(err)
end

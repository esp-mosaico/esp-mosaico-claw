---
{
  "name": "current_weather",
  "description": "Fetch current weather from Open-Meteo, automatically locating the device by public IP when coordinates are not provided.",
  "author": "ESP-Claw contributor",
  "metadata":
    {
      "category": ["utility"],
      "tags": ["weather", "info"],
      "cap_groups": ["cap_lua", "cap_http_request"],
      "manage_mode": "web"
    }
}
---

# Current Weather

Use this skill when the user asks for current weather, temperature, wind,
humidity, rain probability, or today's weather forecast.

Run the bundled Lua script once with `lua_run_script`. The script uses the Lua
`capability` module from `lua_module_call_capability` to call the registered
`http_request` capability. Without explicit coordinates it first calls
`http://ip-api.com` to resolve the device's public-IP location, then calls
Open-Meteo's forecast API at `https://api.open-meteo.com`.

The configured `search_http_allowlist` must allow both `ip-api.com` and
`api.open-meteo.com`, or use `*`.

## Script Args Schema

```json
{
  "type": "object",
  "properties": {
    "latitude": {
      "type": "number",
      "description": "Optional latitude. Must be provided together with longitude; otherwise public-IP location is used."
    },
    "longitude": {
      "type": "number",
      "description": "Optional longitude. Must be provided together with latitude; otherwise public-IP location is used."
    },
    "location": {
      "type": "string",
      "description": "Optional display label. Without one, the detected city/region/country or coordinates are used."
    },
    "timezone": {
      "type": "string",
      "description": "Optional Open-Meteo timezone parameter. Defaults to the IP-detected timezone or auto for explicit coordinates."
    },
    "timeout_ms": {
      "type": "integer",
      "description": "Optional HTTP timeout in milliseconds. Defaults to 15000."
    },
    "session_id": {
      "type": "string",
      "description": "Optional session id passed to capability.call opts."
    },
    "source_cap": {
      "type": "string",
      "description": "Optional source capability name. Defaults to current_weather."
    }
  }
}
```

## Tool Call Inputs

Default action:

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/current_weather.lua",
  "args": {}
}
```

Fetch weather for a specific coordinate:

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/current_weather.lua",
  "args": {
    "location": "Shanghai",
    "latitude": 31.2304,
    "longitude": 121.4737
  }
}
```

## Behavior

- With both `latitude` and `longitude`, the script skips IP geolocation and uses
  the explicit coordinates.
- With neither coordinate, the script resolves approximate location from the
  device's public IP before requesting weather.
- Providing only one coordinate is an error.
- IP geolocation failures are reported directly; the script does not silently
  fall back to a fixed city.

The script prints a human-readable line and a compact JSON result:

```text
[current_weather] Shanghai temp=29.4C apparent=34.1C condition=Partly cloudy humidity=78% wind=8.5km/h rain_prob=35% time=2026-05-13T14:00
{"ok":true,"location":"Shanghai","location_source":"ip","city":"Shanghai","region":"Shanghai","country_code":"CN","latitude":31.2304,"longitude":121.4737,"timezone":"Asia/Shanghai","temperature_c":29.4,"apparent_temperature_c":34.1,"relative_humidity_pct":78,"wind_speed_kmh":8.5,"weather_code":2,"condition":"Partly cloudy","rain_probability_pct":35,"daily_high_c":31.2,"daily_low_c":26.7,"time":"2026-05-13T14:00"}
```

Report the weather values to the user. If the script errors, report the error
message directly.

set(MOSAIC_APP_NAME weather)
set(MOSAIC_APP_MODULE_SOURCE weather_app.c)
set(MOSAIC_APP_MODULE_SYMBOL mosaic_weather_app)
set(MOSAIC_APP_BUNDLE generated/weather.gspb)
set(MOSAIC_APP_SCENE_DIR scene)
set(MOSAIC_APP_SCENE_JSON scene/weather_480.json)
set(MOSAIC_APP_GENERATOR scene/gen_scene.py)
set(MOSAIC_APP_GENERATED_HEADERS
    weather_binds.h
    weather_actions.h
    weather_objects.h
    weather_templates.h)
set(MOSAIC_APP_LOGIC NATIVE)
set(MOSAIC_APP_TICK_MS 1000)

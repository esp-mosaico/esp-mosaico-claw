set(MOSAIC_APP_NAME settings)
set(MOSAIC_APP_MODULE_SOURCE settings_app.c)
set(MOSAIC_APP_MODULE_SYMBOL mosaic_settings_app)
set(MOSAIC_APP_BUNDLE generated/settings.gspb)
set(MOSAIC_APP_SCENE_DIR scene)
set(MOSAIC_APP_SCENE_JSON scene/settings_480.json)
set(MOSAIC_APP_GENERATOR scene/gen_scene.py)
set(MOSAIC_APP_SCENE_SOURCES
    ../setup_center/scene/gen_scene.py
    ../../docs/mosaico-prototype.html)
set(MOSAIC_APP_GENERATED_HEADERS
    settings_binds.h
    settings_actions.h
    settings_objects.h
    settings_templates.h)
set(MOSAIC_APP_LOGIC NATIVE)
set(MOSAIC_APP_TICK_MS 1000)

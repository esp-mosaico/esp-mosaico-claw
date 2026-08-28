set(MOSAIC_APP_NAME welcome)
set(MOSAIC_APP_MODULE_SOURCE welcome_app.c)
set(MOSAIC_APP_MODULE_SYMBOL mosaic_welcome_app)
set(MOSAIC_APP_BUNDLE generated/welcome.gspb)
set(MOSAIC_APP_SCENE_DIR scene)
set(MOSAIC_APP_GENERATOR scene/gen_scene.py)
set(MOSAIC_APP_GENERATED_HEADERS
    welcome_intro_actions.h welcome_intro_binds.h welcome_intro_objects.h
    welcome_keys_actions.h welcome_keys_binds.h welcome_keys_objects.h
    welcome_g1_actions.h welcome_g1_binds.h welcome_g1_objects.h
    welcome_g2_actions.h welcome_g2_binds.h welcome_g2_objects.h
    welcome_g4_actions.h welcome_g4_binds.h welcome_g4_objects.h)
set(MOSAIC_APP_TICK_MS 250)

set(MOSAIC_APP_NAME album)
set(MOSAIC_APP_MODULE_SOURCE album_app.c)
set(MOSAIC_APP_MODULE_SYMBOL mosaic_album_app)
set(MOSAIC_APP_BUNDLE generated/album.gspb)
set(MOSAIC_APP_SCENE_DIR scene)
set(MOSAIC_APP_SCENE_JSON scene/album_480.json)
set(MOSAIC_APP_GENERATOR scene/gen_scene.py)
set(MOSAIC_APP_GENERATED_HEADERS
    album_binds.h
    album_actions.h
    album_objects.h
    album_templates.h)

set(MOSAIC_APP_NAME music)
set(MOSAIC_APP_MODULE_SOURCE music_app.c)
set(MOSAIC_APP_MODULE_SYMBOL mosaic_music_app)
set(MOSAIC_APP_BUNDLE generated/music.gspb)
set(MOSAIC_APP_SCENE_DIR scene)
set(MOSAIC_APP_SCENE_JSON scene/music_480.json)
set(MOSAIC_APP_GENERATOR scene/gen_scene.py)
set(MOSAIC_APP_SCENE_SOURCES
    ../../common/assets/music/vinyl_base.png
    ../../common/assets/music/vinyl_label.png
    ../../common/assets/music/tonearm.png
    ../../common/assets/music/music_loop.png
    ../../common/assets/music/music_shuffle.png
    ../../common/assets/music/music_list.png)
set(MOSAIC_APP_EXTRA_SOURCES music_presenter.c)
set(MOSAIC_APP_LOGIC NATIVE)
set(MOSAIC_APP_TICK_MS 100)

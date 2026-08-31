set(MOSAIC_APP_NAME air_battle)
set(MOSAIC_APP_MODULE_SOURCE air_battle_app.c)
set(MOSAIC_APP_MODULE_SYMBOL mosaic_air_battle_app)
set(MOSAIC_APP_BUNDLE generated/air_battle.gspb)
set(MOSAIC_APP_SCENE_DIR scene)
set(MOSAIC_APP_SCENE_JSON scene/air_battle_480.json)
set(MOSAIC_APP_GENERATOR scene/gen_scene.py)
set(MOSAIC_APP_SCENE_SOURCES
    assets/player.png
    assets/enemy_a.png
    assets/enemy_b.png
    assets/enemy_c.png
    assets/enemy_d.png
    assets/exhaust_0.png
    assets/exhaust_1.png
    assets/boom_0.png
    assets/boom_1.png
    assets/boom_2.png)
set(MOSAIC_APP_EXTRA_INCLUDE_DIRS ../music)
set(MOSAIC_APP_LOGIC NATIVE)
set(MOSAIC_APP_TICK_MS 16)

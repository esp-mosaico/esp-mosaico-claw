set(MOSAIC_APP_NAME bluetooth)
set(MOSAIC_APP_MODULE_SOURCE bluetooth_app.c)
set(MOSAIC_APP_MODULE_SYMBOL mosaic_bluetooth_app)
set(MOSAIC_APP_BUNDLE generated/bluetooth.gspb)
set(MOSAIC_APP_SCENE_DIR scene)
set(MOSAIC_APP_SCENE_JSON scene/bluetooth_480.json)
set(MOSAIC_APP_GENERATOR scene/gen_scene.py)
set(MOSAIC_APP_SCENE_SOURCES
    ../../common/assets/bluetooth/vinyl_base.png
    ../../common/assets/bluetooth/tonearm.png
    ../../common/assets/bluetooth/bt_platter.png
    ../../common/assets/bluetooth/bt_halftone.png
    ../../common/assets/bluetooth/bt_ring.png)
set(MOSAIC_APP_LOGIC NATIVE)
set(MOSAIC_APP_TICK_MS 1000)
if(ESP_PLATFORM)
    set(MOSAIC_APP_EXTRA_SOURCES
        bluetooth_audio_runtime.c
        bluetooth_a2dp_lifecycle.c)
endif()

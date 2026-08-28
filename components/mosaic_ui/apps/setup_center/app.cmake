set(MOSAIC_APP_NAME setup_center)
set(MOSAIC_APP_MODULE_SOURCE setup_center_app.c)
set(MOSAIC_APP_EXTRA_SOURCES setup_center_wechat_queue.c)
set(MOSAIC_APP_MODULE_SYMBOL mosaic_setup_center_app)
set(MOSAIC_APP_BUNDLE generated/setup_center.gspb)
set(MOSAIC_APP_SCENE_DIR scene)
set(MOSAIC_APP_SCENE_JSON scene/setup_center_480.json)
set(MOSAIC_APP_GENERATOR scene/gen_scene.py)
set(MOSAIC_APP_SCENE_SOURCES
    scene/setup_html_back.png
    scene/setup_html_chevron.png
    scene/setup_html_refresh.png
    scene/setup_html_loading.png
    scene/setup_html_yes.png
    scene/setup_html_no.png
    scene/setup_qr_placeholder.png
    scene/setup_llm_qr_placeholder.png)
set(MOSAIC_APP_LOGIC NATIVE)
set(MOSAIC_APP_TICK_MS 100)

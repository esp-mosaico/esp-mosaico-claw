idf_component_register(
    SRCS
        "air_voice_service.c"
        "air_voice_dsp.c"
        "air_voice_config.c"
        "air_voice_store.c"
        "air_voice_log.c"
        "air_voice_json.c"
        "air_voice_player.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        espressif__cjson
    PRIV_REQUIRES
        audio_hub
        claw_core
        esp_timer
        esp_hw_support
        espressif__esp_board_manager
        espressif__esp_codec_dev
        freertos
        heap
        log
)

# The DSP inner loops are float heavy; keep them fast even in -Og builds.
set_source_files_properties(air_voice_dsp.c PROPERTIES COMPILE_OPTIONS "-O2")

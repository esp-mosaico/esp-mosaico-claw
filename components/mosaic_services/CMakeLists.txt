# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

# Bottom layer: device model plus the capability providers built on it.
# Nothing here may depend on mosaic_ui, mosaic_shell or any App.
set(MOSAIC_SERVICES_SRCS
    "src/mosaic_settings_model.c"
    "src/mosaic_device_capabilities.c"
    "src/mosaic_media_bluetooth.c"
    "src/mosaic_media_player.c"
    "src/mosaic_net_weather.c"
    "src/music_controller.c"
    "src/music_audio.c")

# The A2DP sink runtime is platform code and lives with the provider that
# owns it, not with the App that renders it.
if(ESP_PLATFORM AND CONFIG_MOSAIC_UI_BLUETOOTH_AUDIO)
    list(APPEND MOSAIC_SERVICES_SRCS
        "src/bluetooth_audio_runtime.c"
        "src/bluetooth_a2dp_lifecycle.c")
endif()

idf_component_register(
    SRCS ${MOSAIC_SERVICES_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        mosaic_capabilities
        esp_timer
        freertos
    PRIV_REQUIRES
        audio_hub
        audio_playback
        weather_service
        bt
        espressif__esp_audio_codec
        espressif__esp_asrc
        espressif__esp_bt_audio
)

if(CONFIG_MOSAIC_UI_BLUETOOTH_AUDIO)
    idf_build_set_property(LINK_OPTIONS
        "-Wl,--wrap=esp_a2d_sink_init;-Wl,--wrap=esp_a2d_register_callback;-Wl,--wrap=esp_a2d_sink_deinit;-Wl,-u,__wrap_esp_a2d_sink_init"
        APPEND)
endif()

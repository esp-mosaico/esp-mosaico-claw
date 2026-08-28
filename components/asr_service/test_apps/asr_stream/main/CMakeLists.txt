idf_component_register(
    SRCS
        "test_main.c"
        "../../../src/asr_provider_qwen.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../../../src"
    REQUIRES
        cjson
        esp_event
        esp_netif
        esp_websocket_client
        nvs_flash
        protocol_examples_common
        unity)

set(asr_test_pcm_file "${CMAKE_CURRENT_LIST_DIR}/test_audio_16k_mono_s16le.pcm")
if(NOT EXISTS "${asr_test_pcm_file}")
    message(FATAL_ERROR "Missing ASR test PCM file: ${asr_test_pcm_file}")
endif()

set(asr_test_pcm_embed "${CMAKE_CURRENT_BINARY_DIR}/asr_test_audio.pcm")
configure_file("${asr_test_pcm_file}" "${asr_test_pcm_embed}" COPYONLY)
target_add_binary_data(${COMPONENT_LIB} "${asr_test_pcm_embed}" BINARY)

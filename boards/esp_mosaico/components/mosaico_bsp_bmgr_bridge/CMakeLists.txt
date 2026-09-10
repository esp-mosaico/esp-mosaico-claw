idf_component_register(
    SRCS "mosaico_bsp_bmgr_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-mosaico-bsp
    PRIV_REQUIRES esp_board_manager
)

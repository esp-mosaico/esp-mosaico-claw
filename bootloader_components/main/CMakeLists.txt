idf_component_register(
    SRCS "bootloader_start.c"
    REQUIRES bootloader bootloader_support
)

# Preserve ESP-IDF's default second-stage bootloader linker layout while
# replacing only its application partition selection entry point.
idf_build_get_property(bootloader_linker_script BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${bootloader_linker_script}")

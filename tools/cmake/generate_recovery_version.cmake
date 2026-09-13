if(NOT DEFINED OUTPUT_FILE OR OUTPUT_FILE STREQUAL "")
    message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

# Keep the marker compact while making every packaged system image unique.
string(TIMESTAMP RECOVERY_BUILD_TIME "%Y%m%d%H%M%S" UTC)
string(RANDOM LENGTH 17 ALPHABET 0123456789abcdef RECOVERY_RANDOM_SUFFIX)
file(WRITE "${OUTPUT_FILE}" "${RECOVERY_BUILD_TIME}-${RECOVERY_RANDOM_SUFFIX}")

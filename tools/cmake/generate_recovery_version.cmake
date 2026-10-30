if(NOT DEFINED OUTPUT_FILE OR OUTPUT_FILE STREQUAL "")
    message(FATAL_ERROR "OUTPUT_FILE is required")
endif()

if(NOT DEFINED SEED_ROOT OR NOT IS_DIRECTORY "${SEED_ROOT}")
    message(FATAL_ERROR "SEED_ROOT must contain the packaged recovery defaults")
endif()

# UI-only rebuilds must not look like a new set of defaults. Keep the existing
# 32-byte marker format, derived from sorted relative names and file contents.
file(GLOB_RECURSE RECOVERY_FILES LIST_DIRECTORIES false
    RELATIVE "${SEED_ROOT}" "${SEED_ROOT}/*")
list(SORT RECOVERY_FILES)
set(RECOVERY_CONTENTS "")
foreach(RECOVERY_FILE IN LISTS RECOVERY_FILES)
    file(SHA256 "${SEED_ROOT}/${RECOVERY_FILE}" RECOVERY_FILE_SHA)
    string(APPEND RECOVERY_CONTENTS "${RECOVERY_FILE}:${RECOVERY_FILE_SHA}\n")
endforeach()
string(SHA256 RECOVERY_CONTENT_SHA "${RECOVERY_CONTENTS}")
string(SUBSTRING "${RECOVERY_CONTENT_SHA}" 0 32 RECOVERY_MARKER)
file(WRITE "${OUTPUT_FILE}" "${RECOVERY_MARKER}")

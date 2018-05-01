find_path(COTP_INCLUDE_DIR cotp.h)

find_library(COTP_LIBRARIES cotp)

mark_as_advanced(COTP_LIBRARIES COTP_INCLUDE_DIR)

if(COTP_INCLUDE_DIR AND EXISTS "${COTP_INCLUDE_DIR}/cotp.h")
    file(STRINGS "${COTP_INCLUDE_DIR}/cotp.h" COTP_H REGEX "^#define COTP_VERSION \"[^\"]*\"$")
    string(REGEX REPLACE "^.*COTP_VERSION \"([0-9]+).*$" "\\1" COTP_VERSION_MAJOR "${COTP_H}")
    string(REGEX REPLACE "^.*COTP_VERSION \"[0-9]+\\.([0-9]+).*$" "\\1" COTP_VERSION_MINOR  "${COTP_H}")
    string(REGEX REPLACE "^.*COTP_VERSION \"[0-9]+\\.[0-9]+\\.([0-9]+).*$" "\\1" COTP_VERSION_PATCH "${COTP_H}")
    set(COTP_VERSION_STRING "${COTP_VERSION_MAJOR}.${COTP_VERSION_MINOR}.${COTP_VERSION_PATCH}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Cotp REQUIRED_VARS COTP_LIBRARIES COTP_INCLUDE_DIR VERSION_VAR ${COTP_VERSION_STRING})

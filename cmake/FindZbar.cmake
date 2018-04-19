find_path(ZBAR_INCLUDE_DIR zbar.h)

find_library(ZBAR_LIBRARIES zbar)

mark_as_advanced(ZBAR_LIBRARIES ZBAR_INCLUDE_DIR)

if(ZBAR_INCLUDE_DIR AND EXISTS "${ZBAR_INCLUDE_DIR}/zbar.h")
    file(STRINGS "${ZBAR_INCLUDE_DIR}/zbar.h" ZBAR_H REGEX "^#define ZBAR_VERSION \"[^\"]*\"$")
    string(REGEX REPLACE "^.*ZBAR_VERSION \"([0-9]+).*$" "\\1" ZBAR_VERSION_MAJOR "${ZBAR_H}")
    string(REGEX REPLACE "^.*ZBAR_VERSION \"[0-9]+\\.([0-9]+).*$" "\\1" ZBAR_VERSION_MINOR  "${ZBAR_H}")
    string(REGEX REPLACE "^.*ZBAR_VERSION \"[0-9]+\\.[0-9]+\\.([0-9]+).*$" "\\1" ZBAR_VERSION_PATCH "${ZBAR_H}")
    set(ZBAR_VERSION_STRING "${ZBAR_VERSION_MAJOR}.${ZBAR_VERSION_MINOR}.${ZBAR_VERSION_PATCH}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Zbar DEFAULT_MSG ZBAR_LIBRARIES ZBAR_INCLUDE_DIR)

cmake_minimum_required(VERSION 3.16)
project(OTPClient VERSION "3.5.3" LANGUAGES "C")
include(GNUInstallDirs)

configure_file("src/common/version.h.in" "version.h")

set (GETTEXT_PACKAGE ${CMAKE_PROJECT_NAME})

include_directories(${PROJECT_BINARY_DIR})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(IS_FLATPAK "Use flatpak app's config folder to store the database" OFF)
option(BUILD_CLI "Build the CLI" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -Wextra -O2 -Wformat=2 -Wmissing-format-attribute -fstack-protector-strong -Wundef -Wmissing-format-attribute")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fdiagnostics-color=always -Wstrict-prototypes -Wunreachable-code")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wchar-subscripts -Wwrite-strings -Wpointer-arith -Wbad-function-cast -Wcast-align")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=format-security -Werror=implicit-function-declaration -Wno-sign-compare")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2")
if(CMAKE_COMPILER_IS_GNUCC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pie -fPIE")
endif()
if(IS_FLATPAK)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DIS_FLATPAK")
endif()

add_definitions(-DGETTEXT_PACKAGE=\"${GETTEXT_PACKAGE}\")


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-add-needed -Wl,--as-needed -Wl,--no-undefined")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,relro,-z,now")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--no-add-needed -Wl,--as-needed")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,relro,-z,now")
endif()

find_package(PkgConfig REQUIRED)
find_package(Protobuf 3.6.0 REQUIRED)
find_package(Gcrypt 1.10.1 REQUIRED)
pkg_check_modules(COTP REQUIRED cotp>=3.0.0)
pkg_check_modules(PNG REQUIRED libpng>=1.6.30)
pkg_check_modules(JANSSON REQUIRED jansson>=2.12)
pkg_check_modules(ZBAR REQUIRED zbar>=0.20)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0>=3.24.0)
pkg_check_modules(GLIB2 REQUIRED glib-2.0>=2.68.0)
pkg_check_modules(GIO REQUIRED gio-2.0>=2.68.0)
pkg_check_modules(UUID REQUIRED uuid>=2.34.0)
pkg_check_modules(PROTOC REQUIRED libprotobuf-c>=1.3.0)
pkg_check_modules(LIBSECRET REQUIRED libsecret-1>=0.20.0)
pkg_check_modules(LIBQRENCODE REQUIRED libqrencode>=4.0.2)

set(COMMON_INCDIRS
        ${GTK3_INCLUDE_DIRS}
        ${GCRYPT_INCLUDE_DIRS}
        ${COTP_INCLUDE_DIRS}
        ${JANSSON_INCLUDE_DIRS}
        ${UUID_INCLUDE_DIRS}
        ${PROTOC_INCLUDE_DIRS}
        ${LIBSECRET_INCLUDE_DIRS}
        ${LIBQRENCODE_INCLUDE_DIRS}
)

set(COMMON_LIBS
        ${GTK3_LIBRARIES}
        ${GCRYPT_LIBRARIES}
        ${COTP_LIBRARIES}
        ${JANSSON_LIBRARIES}
        ${UUID_LIBRARIES}
        ${PROTOC_LIBRARIES}
        ${LIBSECRET_LIBRARIES}
        ${LIBQRENCODE_LIBRARIES}
)

## GUI START
add_subdirectory(src/gui)
target_link_libraries(${PROJECT_NAME}
        ${PNG_LIBRARIES}
        ${ZBAR_LIBRARIES}
        ${COMMON_LIBS}
)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "otpclient")

install(TARGETS ${PROJECT_NAME} DESTINATION bin)

install(FILES data/com.github.paolostivanin.OTPClient.desktop DESTINATION share/applications)

install(FILES
        src/gui/ui/otpclient.ui
        src/gui/ui/add_popover.ui
        src/gui/ui/settings_popover.ui
        src/gui/ui/shortcuts.ui
        DESTINATION share/otpclient)

install(FILES man/otpclient.1.gz DESTINATION share/man/man1)

install(FILES
        data/icons/com.github.paolostivanin.OTPClient.svg
        data/icons/com.github.paolostivanin.OTPClient-symbolic.svg
        DESTINATION share/icons/hicolor/scalable/apps)
## GUI END
## CLI START
if(BUILD_CLI)
    add_subdirectory(src/cli)
    target_link_libraries(${PROJECT_NAME}-cli
            ${COMMON_LIBS}
    )
    set_target_properties(${PROJECT_NAME}-cli PROPERTIES OUTPUT_NAME "otpclient-cli")

    install(TARGETS ${PROJECT_NAME}-cli DESTINATION bin)

    install(FILES man/otpclient-cli.1.gz DESTINATION share/man/man1)
endif()
## CLI END

install(FILES data/com.github.paolostivanin.OTPClient.appdata.xml DESTINATION share/metainfo)
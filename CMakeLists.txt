cmake_minimum_required(VERSION 3.16)
project(OTPClient VERSION "4.3.0" LANGUAGES "C")
include(GNUInstallDirs)

configure_file("src/common/version.h.in" "version.h")

set (GETTEXT_PACKAGE ${CMAKE_PROJECT_NAME})

include_directories(${PROJECT_BINARY_DIR})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(IS_FLATPAK "Use flatpak app's config folder to store the database" OFF)
option(BUILD_GUI "Build the GTK UI" ON)
option(BUILD_CLI "Build the CLI" ON)
option(ENABLE_MINIMIZE_TO_TRAY "Enable minimize to tray feature" OFF)

set(CMAKE_C_STANDARD 11)
set(COMMON_C_FLAGS
        "-Wall -Wextra -O3 -Wformat=2 -Wmissing-format-attribute -fstack-protector-strong"
        "-Wundef -Wmissing-format-attribute -fdiagnostics-color=always"
        "-Wstrict-prototypes -Wunreachable-code -Wchar-subscripts"
        "-Wwrite-strings -Wpointer-arith -Wbad-function-cast -Wcast-align"
        "-Werror=format-security -Werror=implicit-function-declaration -Wno-sign-compare"
        "-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3"
)

if(CMAKE_COMPILER_IS_GNUCC)
    list(APPEND COMMON_C_FLAGS "-pie -fPIE")
endif()

if(IS_FLATPAK)
    list(APPEND COMMON_C_FLAGS "-DIS_FLATPAK")
endif()

string(REPLACE ";" " " COMMON_C_FLAGS "${COMMON_C_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_C_FLAGS}")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(COMMON_LINKER_FLAGS
            "-Wl,--no-add-needed -Wl,--as-needed -Wl,--no-undefined"
            "-Wl,-z,relro,-z,now"
    )

    string(REPLACE ";" " " COMMON_LINKER_FLAGS "${COMMON_LINKER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${COMMON_LINKER_FLAGS}")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${COMMON_LINKER_FLAGS}")
endif()

add_definitions(
    -DGETTEXT_PACKAGE=\"${GETTEXT_PACKAGE}\"
    -DDESTINATION="${CMAKE_INSTALL_PREFIX}"
)
if (ENABLE_MINIMIZE_TO_TRAY)
  add_definitions(-DENABLE_MINIMIZE_TO_TRAY=1)
endif()

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
if (ENABLE_MINIMIZE_TO_TRAY)
  pkg_check_modules(APPINDICATOR REQUIRED ayatana-appindicator3-0.1)
endif()

set(COMMON_INCDIRS
        ${GCRYPT_INCLUDE_DIRS}
        ${COTP_INCLUDE_DIRS}
        ${JANSSON_INCLUDE_DIRS}
        ${UUID_INCLUDE_DIRS}
        ${PROTOC_INCLUDE_DIRS}
        ${LIBSECRET_INCLUDE_DIRS}
        ${LIBQRENCODE_INCLUDE_DIRS}
)

set(COMMON_LIBS
        ${GCRYPT_LIBRARIES}
        ${COTP_LIBRARIES}
        ${JANSSON_LIBRARIES}
        ${UUID_LIBRARIES}
        ${PROTOC_LIBRARIES}
        ${LIBSECRET_LIBRARIES}
        ${LIBQRENCODE_LIBRARIES}
)

if(BUILD_GUI)
    add_subdirectory(src/gui)
    if(ENABLE_MINIMIZE_TO_TRAY)
        target_include_directories(${PROJECT_NAME} PRIVATE ${APPINDICATOR_INCLUDE_DIRS})
    endif()
    target_link_libraries(${PROJECT_NAME}
            ${GTK3_LIBRARIES}
            ${PNG_LIBRARIES}
            ${ZBAR_LIBRARIES}
            ${COMMON_LIBS}
    )
    if(ENABLE_MINIMIZE_TO_TRAY)
        target_link_libraries(${PROJECT_NAME} ${APPINDICATOR_LIBRARIES})
    endif()
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "otpclient")

    install(TARGETS ${PROJECT_NAME} DESTINATION bin)
    install(FILES data/com.github.paolostivanin.OTPClient.desktop DESTINATION share/applications)
    install(FILES man/otpclient.1 DESTINATION share/man/man1)

    set(GUI_UI_FILES
            src/gui/ui/otpclient.ui
            src/gui/ui/add_popover.ui
            src/gui/ui/settings_popover.ui
            src/gui/ui/security_settings.ui
            src/gui/ui/shortcuts.ui
    )
    install(FILES ${GUI_UI_FILES} DESTINATION share/otpclient)

    set(GUI_ICON_FILES
            data/icons/com.github.paolostivanin.OTPClient.svg
            data/icons/com.github.paolostivanin.OTPClient-symbolic.svg
    )
    install(FILES ${GUI_ICON_FILES} DESTINATION share/icons/hicolor/scalable/apps)
endif ()

if(BUILD_CLI)
    add_subdirectory(src/cli)
    target_link_libraries(${PROJECT_NAME}-cli
            ${COMMON_LIBS}
    )
    set_target_properties(${PROJECT_NAME}-cli PROPERTIES OUTPUT_NAME "otpclient-cli")

    install(TARGETS ${PROJECT_NAME}-cli DESTINATION bin)
    install(FILES man/otpclient-cli.1 DESTINATION share/man/man1)
endif()

install(FILES data/com.github.paolostivanin.OTPClient.appdata.xml DESTINATION share/metainfo)

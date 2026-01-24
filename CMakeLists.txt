cmake_minimum_required(VERSION 3.16)
project(OTPClient VERSION "4.3.1" LANGUAGES "C")
include(GNUInstallDirs)

configure_file("src/common/version.h.in" "version.h")

set(GETTEXT_PACKAGE ${PROJECT_NAME})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(IS_FLATPAK "Use flatpak app's config folder to store the database" OFF)
option(BUILD_GUI "Build the GTK UI" ON)
option(BUILD_CLI "Build the CLI" ON)
option(ENABLE_MINIMIZE_TO_TRAY "Enable minimize to tray feature" OFF)

set(COMMON_C_OPTIONS
        -Wall -Wextra -O3 -Wformat=2 -Wmissing-format-attribute -fstack-protector-strong
        -Wundef -Wmissing-format-attribute -fdiagnostics-color=always
        -Wstrict-prototypes -Wunreachable-code -Wchar-subscripts
        -Wwrite-strings -Wpointer-arith -Wbad-function-cast -Wcast-align
        -Werror=format-security -Werror=implicit-function-declaration -Wno-sign-compare
        -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3
)

set(COMMON_COMPILE_DEFINITIONS
        GETTEXT_PACKAGE=\"${GETTEXT_PACKAGE}\"
        DESTINATION=\"${CMAKE_INSTALL_PREFIX}\"
)

set(COMMON_LINK_OPTIONS)

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND COMMON_C_OPTIONS -fPIE)
    list(APPEND COMMON_LINK_OPTIONS -pie)
endif()

if(IS_FLATPAK)
    list(APPEND COMMON_COMPILE_DEFINITIONS IS_FLATPAK)
endif()

if(ENABLE_MINIMIZE_TO_TRAY)
    list(APPEND COMMON_COMPILE_DEFINITIONS ENABLE_MINIMIZE_TO_TRAY=1)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND COMMON_LINK_OPTIONS
            "-Wl,--no-add-needed"
            "-Wl,--as-needed"
            "-Wl,--no-undefined"
            "-Wl,-z,relro,-z,now"
    )
endif()

find_package(PkgConfig REQUIRED)
find_package(Protobuf 3.6.0 REQUIRED)
find_package(Gcrypt 1.10.1 REQUIRED)
pkg_check_modules(COTP REQUIRED IMPORTED_TARGET cotp>=3.0.0)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng>=1.6.30)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson>=2.12)
pkg_check_modules(ZBAR REQUIRED IMPORTED_TARGET zbar>=0.20)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.24.0)
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0>=2.68.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.68.0)
pkg_check_modules(UUID REQUIRED IMPORTED_TARGET uuid>=2.34.0)
pkg_check_modules(PROTOC REQUIRED IMPORTED_TARGET libprotobuf-c>=1.3.0)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1>=0.20.0)
pkg_check_modules(LIBQRENCODE REQUIRED IMPORTED_TARGET libqrencode>=4.0.2)
if(ENABLE_MINIMIZE_TO_TRAY)
    pkg_check_modules(APPINDICATOR REQUIRED IMPORTED_TARGET ayatana-appindicator3-0.1)
endif()

set(COMMON_INCDIRS
        ${GCRYPT_INCLUDE_DIRS}
)

set(COMMON_LIBS
        ${GCRYPT_LIBRARIES}
        PkgConfig::COTP
        PkgConfig::JANSSON
        PkgConfig::UUID
        PkgConfig::PROTOC
        PkgConfig::LIBSECRET
        PkgConfig::LIBQRENCODE
        PkgConfig::GLIB2
        PkgConfig::GIO
)

function(otpclient_apply_target_settings target_name)
    target_compile_options(${target_name} PRIVATE ${COMMON_C_OPTIONS})
    target_compile_definitions(${target_name} PRIVATE ${COMMON_COMPILE_DEFINITIONS})
    target_include_directories(${target_name} PRIVATE ${PROJECT_BINARY_DIR} ${COMMON_INCDIRS})
    if(COMMON_LINK_OPTIONS)
        target_link_options(${target_name} PRIVATE ${COMMON_LINK_OPTIONS})
    endif()
endfunction()

if(BUILD_GUI)
    add_subdirectory(src/gui)
    otpclient_apply_target_settings(${PROJECT_NAME})
    if(ENABLE_MINIMIZE_TO_TRAY)
        target_link_libraries(${PROJECT_NAME} PkgConfig::APPINDICATOR)
    endif()
    target_link_libraries(${PROJECT_NAME}
            PkgConfig::GTK3
            PkgConfig::PNG
            PkgConfig::ZBAR
            ${COMMON_LIBS}
    )
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
    otpclient_apply_target_settings(${PROJECT_NAME}-cli)
    target_link_libraries(${PROJECT_NAME}-cli
            ${COMMON_LIBS}
    )
    set_target_properties(${PROJECT_NAME}-cli PROPERTIES OUTPUT_NAME "otpclient-cli")

    install(TARGETS ${PROJECT_NAME}-cli DESTINATION bin)
    install(FILES man/otpclient-cli.1 DESTINATION share/man/man1)
endif()

install(FILES data/com.github.paolostivanin.OTPClient.appdata.xml DESTINATION share/metainfo)

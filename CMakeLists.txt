cmake_minimum_required(VERSION 3.25)
project(OTPClient VERSION "4.99.91" LANGUAGES "C")
include(GNUInstallDirs)

configure_file("src/common/version.h.in" "version.h")

set(GETTEXT_PACKAGE ${PROJECT_NAME})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(IS_FLATPAK "Use flatpak app's config folder to store the database" OFF)
option(ENABLE_MINIMIZE_TO_TRAY "Enable minimize to tray feature" OFF)
option(BUILD_GUI "Build the GUI" ON)
option(BUILD_CLI "Build the CLI" ON)
option(BUILD_SEARCH_PROVIDER "Build the D-Bus search provider" ON)

set(COMMON_C_OPTIONS
        -Wall -Wextra -O3 -Wformat=2 -Wmissing-format-attribute -fstack-protector-strong
        -Wundef -fdiagnostics-color=always
        -Wstrict-prototypes -Wunreachable-code -Wchar-subscripts
        -Wwrite-strings -Wpointer-arith -Wbad-function-cast -Wcast-align
        -Werror=format-security -Werror=implicit-function-declaration -Wno-sign-compare
        -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3
)

set(COMMON_COMPILE_DEFINITIONS
        GETTEXT_PACKAGE=\"${GETTEXT_PACKAGE}\"
)

set(COMMON_LINK_OPTIONS)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
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
            "-Wl,--no-copy-dt-needed-entries"
            "-Wl,--as-needed"
            "-Wl,--no-undefined"
            "-Wl,-z,relro,-z,now"
    )
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GCRYPT REQUIRED IMPORTED_TARGET libgcrypt>=1.11.0)
pkg_check_modules(COTP REQUIRED IMPORTED_TARGET cotp>=4.0.0)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson>=2.14)
pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0>=2.82.0)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.82.0)
pkg_check_modules(UUID REQUIRED IMPORTED_TARGET uuid>=2.34.0)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1>=0.20.0)

if(BUILD_GUI)
    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.18.0)
    pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1>=1.5.0)
    pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng>=1.6.30)
    pkg_check_modules(ZBAR REQUIRED IMPORTED_TARGET zbar>=0.20)
    pkg_check_modules(PROTOC REQUIRED IMPORTED_TARGET libprotobuf-c>=1.3.0)
    pkg_check_modules(LIBQRENCODE REQUIRED IMPORTED_TARGET libqrencode>=4.1.0)
endif()

set(COMMON_LIBS
        PkgConfig::GCRYPT
        PkgConfig::COTP
        PkgConfig::JANSSON
        PkgConfig::LIBSECRET
        PkgConfig::GLIB2
        PkgConfig::GIO
        PkgConfig::UUID
)

function(otpclient_apply_target_settings target_name)
    target_compile_options(${target_name} PRIVATE ${COMMON_C_OPTIONS})
    target_compile_definitions(${target_name} PRIVATE ${COMMON_COMPILE_DEFINITIONS})
    target_include_directories(${target_name} PRIVATE ${PROJECT_BINARY_DIR})
    if(COMMON_LINK_OPTIONS)
        target_link_options(${target_name} PRIVATE ${COMMON_LINK_OPTIONS})
    endif()
endfunction()

if(BUILD_GUI)
    add_subdirectory(src/gui)
    otpclient_apply_target_settings(${PROJECT_NAME})
    target_link_libraries(${PROJECT_NAME}
            PkgConfig::GTK4
            PkgConfig::ADWAITA
            PkgConfig::PNG
            PkgConfig::ZBAR
            PkgConfig::PROTOC
            PkgConfig::LIBQRENCODE
            ${COMMON_LIBS}
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "otpclient")

    install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES data/com.github.paolostivanin.OTPClient.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES man/otpclient.1.gz DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

    set(GUI_UI_FILES
            src/gui/ui/window.ui
    )
    install(FILES ${GUI_UI_FILES} DESTINATION ${CMAKE_INSTALL_DATADIR}/otpclient)

    set(GUI_ICON_FILES
            data/icons/com.github.paolostivanin.OTPClient.svg
            data/icons/com.github.paolostivanin.OTPClient-symbolic.svg
    )
    install(FILES ${GUI_ICON_FILES} DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif ()

if(BUILD_CLI)
    add_subdirectory(src/cli)
    otpclient_apply_target_settings(${PROJECT_NAME}-cli)
    target_link_libraries(${PROJECT_NAME}-cli
            ${COMMON_LIBS}
    )
    set_target_properties(${PROJECT_NAME}-cli PROPERTIES OUTPUT_NAME "otpclient-cli")

    install(TARGETS ${PROJECT_NAME}-cli DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES man/otpclient-cli.1.gz DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endif()

if(BUILD_SEARCH_PROVIDER)
    add_subdirectory(src/search-provider)
    otpclient_apply_target_settings(${PROJECT_NAME}-search-provider)
    target_link_libraries(${PROJECT_NAME}-search-provider
            PkgConfig::GIO
            PkgConfig::GLIB2
            ${COMMON_LIBS}
    )

    install(TARGETS ${PROJECT_NAME}-search-provider DESTINATION ${CMAKE_INSTALL_BINDIR})

    # GNOME Search Provider metadata
    install(FILES data/gnome-shell/search-providers/com.github.paolostivanin.OTPClient.SearchProvider.ini DESTINATION ${CMAKE_INSTALL_DATADIR}/gnome-shell/search-providers)

    # D-Bus Service activation files
    install(FILES data/dbus-1/services/com.github.paolostivanin.OTPClient.SearchProvider.service DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
    install(FILES data/dbus-1/services/com.github.paolostivanin.OTPClient.KRunner.service DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)

    # KDE Plasma 6 KRunner Metadata
    install(FILES data/krunner/com.github.paolostivanin.OTPClient.KRunner.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/krunner/dbusplugins)
endif()

install(FILES data/com.github.paolostivanin.OTPClient.appdata.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/com.github.paolostivanin.OTPClient.gschema.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)

if(NOT IS_FLATPAK)
    install(CODE "execute_process(COMMAND glib-compile-schemas \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)")
    install(CODE "execute_process(COMMAND gtk4-update-icon-cache -t -f \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/icons/hicolor)")
endif()

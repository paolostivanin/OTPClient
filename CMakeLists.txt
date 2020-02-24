cmake_minimum_required(VERSION 3.5)
project(OTPClient VERSION "2.1.0" LANGUAGES "C")

configure_file("src/common/version.h.in" "version.h")
include_directories(${PROJECT_BINARY_DIR})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(USE_FLATPAK_APP_FOLDER "Use flatpak app's config folder to store the database" OFF)
option(BUILD_GUI "Build the GUI" ON)
option(BUILD_CLI "Build the CLI" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -Wextra -O3 -Wformat=2 -Wmissing-format-attribute -fstack-protector-strong -Wundef -Wmissing-format-attribute")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fdiagnostics-color=always -Wstrict-prototypes -Wunreachable-code")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wchar-subscripts -Wwrite-strings -Wpointer-arith -Wbad-function-cast -Wcast-align")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=format-security -Werror=implicit-function-declaration -Wno-sign-compare")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3")
if(CMAKE_COMPILER_IS_GNUCC)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pie -fPIE")
endif()
if(USE_FLATPAK_APP_FOLDER)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DUSE_FLATPAK_APP_FOLDER")
endif()
if(BUILD_GUI)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DBUILD_GUI")
endif()
if(BUILD_CLI)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DBUILD_CLI")
endif()


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-add-needed -Wl,--as-needed -Wl,--no-undefined")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,relro,-z,now")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--no-add-needed -Wl,--as-needed")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,relro,-z,now")
endif()

find_package(PkgConfig REQUIRED)
find_package(Gcrypt 1.6.0 REQUIRED)
pkg_check_modules(COTP REQUIRED cotp>=1.2.1)
pkg_check_modules(PNG REQUIRED libpng>=1.2.0)
pkg_check_modules(LIBZIP REQUIRED libzip>=1.0.0)
pkg_check_modules(JANSSON REQUIRED jansson>=2.6.0)
pkg_check_modules(ZBAR REQUIRED zbar>=0.10)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0>=3.18.0)
pkg_check_modules(GLIB2 REQUIRED glib-2.0>=2.48.0)
pkg_check_modules(GIO REQUIRED gio-2.0>=2.48.0)

if(${ZBAR_VERSION} VERSION_EQUAL 0.10)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DZBAR_OLD_LIB")
endif ()

set(GUI_HEADER_FILES
        src/common/common.h
        src/add-common.h
        src/gui-common.h
        src/data.h
        src/db-misc.h
        src/file-size.h
        src/get-builder.h
        src/gquarks.h
        src/imports.h
        src/liststore-misc.h
        src/manual-add-cb.h
        src/message-dialogs.h
        src/otpclient.h
        src/parse-uri.h
        src/password-cb.h
        src/qrcode-parser.h
        src/treeview.h
        src/exports.h
        src/lock-app.h)

set(GUI_SOURCE_FILES
        src/common/common.c
        src/add-common.c
        src/andotp.c
        src/app.c
        src/authplus.c
        src/gui-common.c
        src/db-misc.c
        src/edit-data.c
        src/file-size.c
        src/get-builder.c
        src/gquarks.c
        src/imports.c
        src/liststore-misc.c
        src/main.c
        src/manual-add-cb.c
        src/message-dialogs.c
        src/parse-data.c
        src/parse-uri.c
        src/password-cb.c
        src/qrcode-parser.c
        src/screenshot-add-cb.c
        src/add-from-qr.c
        src/settings.c
        src/shortcuts.c
        src/treeview.c
        src/webcam-add-cb.c
        src/exports.c
        src/lock-app.c)

set(CLI_HEADER_FILES
        src/cli/help.h
        src/cli/get-data.h
        src/common/common.h
        src/db-misc.h
        src/gquarks.h
        src/file-size.h)

set(CLI_SOURCE_FILES
        src/cli/main.c
        src/cli/help.c
        src/cli/get-data.c
        src/common/common.c
        src/db-misc.c
        src/gquarks.c
        src/file-size.c)
if(BUILD_GUI AND BUILD_CLI)
        list(APPEND CLI_SOURCE_FILES
                src/treeview.c
                src/liststore-misc.c
                src/gui-common.c
                src/message-dialogs.c)

        list(APPEND CLI_HEADER_FILES
                src/treeview.h
                src/liststore-misc.h
                src/gui-common.h
                src/message-dialogs.h)
endif()

if(BUILD_GUI)
        include_directories(${GTK3_INCLUDE_DIRS}
                ${GCRYPT_INCLUDE_DIRS}
                ${COTP_INCLUDE_DIRS}
                ${LIBZIP_INCLUDE_DIRS}
                ${PNG_INCLUDE_DIRS}
                ${JANSSON_INCLUDE_DIRS}
                ${ZBAR_INCLUDE_DIRS})

        add_executable(${PROJECT_NAME} ${GUI_SOURCE_FILES} ${GUI_HEADER_FILES})
        target_link_libraries(${PROJECT_NAME}
                ${GTK3_LIBRARIES}
                ${GCRYPT_LIBRARIES}
                ${COTP_LIBRARIES}
                ${LIBZIP_LIBRARIES}
                ${PNG_LIBRARIES}
                ${JANSSON_LIBRARIES}
                ${ZBAR_LIBRARIES})

        set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "otpclient")
        install(TARGETS ${PROJECT_NAME} DESTINATION bin)
endif()

if(BUILD_CLI)
        include_directories(${GTK3_INCLUDE_DIRS}
                ${GCRYPT_INCLUDE_DIRS}
                ${COTP_INCLUDE_DIRS}
                ${JANSSON_INCLUDE_DIRS})
        if(BUILD_GUI)
                include_directories(
                        ${LIBZIP_INCLUDE_DIRS}
                        ${PNG_INCLUDE_DIRS}
                        ${ZBAR_INCLUDE_DIRS})
        endif()

        add_executable(${PROJECT_NAME}-cli ${CLI_SOURCE_FILES} ${CLI_HEADER_FILES})
        target_link_libraries(${PROJECT_NAME}-cli
                ${GLIB2_LIBRARIES}
                ${GIO_LIBRARIES}
                ${GCRYPT_LIBRARIES}
                ${COTP_LIBRARIES}
                ${JANSSON_LIBRARIES})
        if(BUILD_GUI)
                target_link_libraries(${PROJECT_NAME}-cli
                        ${GTK3_LIBRARIES}
                        ${LIBZIP_LIBRARIES}
                        ${PNG_LIBRARIES}
                        ${ZBAR_LIBRARIES})
        endif()

        set_target_properties(${PROJECT_NAME}-cli PROPERTIES OUTPUT_NAME "otpclient-cli")
        install(TARGETS ${PROJECT_NAME}-cli DESTINATION bin)
endif()

install(FILES data/com.github.paolostivanin.OTPClient.desktop DESTINATION share/applications)
install(FILES data/com.github.paolostivanin.OTPClient.appdata.xml DESTINATION share/metainfo)

install(FILES src/ui/otpclient.ui DESTINATION share/otpclient)
install(FILES src/ui/shortcuts.ui DESTINATION share/otpclient)

install(FILES data/icons/icon_x16.png DESTINATION share/icons/hicolor/16x16/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x24.png DESTINATION share/icons/hicolor/24x24/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x32.png DESTINATION share/icons/hicolor/32x32/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x48.png DESTINATION share/icons/hicolor/48x48/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x64.png DESTINATION share/icons/hicolor/64x64/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x128.png DESTINATION share/icons/hicolor/128x128/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x256.png DESTINATION share/icons/hicolor/256x256/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x512.png DESTINATION share/icons/hicolor/512x512/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/otpclient.svg DESTINATION share/icons/hicolor/scalable/apps RENAME com.github.paolostivanin.OTPClient.svg)

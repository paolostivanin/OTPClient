cmake_minimum_required(VERSION 3.5)
project(OTPClient)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(USE_FLATPAK_APP_FOLDER "Use flatpak app's config folder to store the database" OFF)

set(OTPCLIENT_VERSION_MAJOR "1")
set(OTPCLIENT_VERSION_MINOR "1")
set(OTPCLIENT_VERSION_PATCH "0")
set(OTPCLIENT_VERSION "${OTPCLIENT_VERSION_MAJOR}.${OTPCLIENT_VERSION_MINOR}.${OTPCLIENT_VERSION_PATCH}")

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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-add-needed -Wl,--as-needed -Wl,--no-undefined")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,relro,-z,now")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--no-add-needed -Wl,--as-needed")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,relro,-z,now")
endif()

find_package(PkgConfig REQUIRED)
find_package(Gcrypt 1.6.0 REQUIRED)
find_package(Cotp 1.0.10 REQUIRED)
find_package(Libzip 1.1.0 REQUIRED)
find_package(Libpng 1.6.0 REQUIRED)
find_package(Jansson 2.8.0 REQUIRED)
find_package(Zbar 0.10.0 REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

include_directories(${GTK3_INCLUDE_DIRS}
        ${GCRYPT_INCLUDE_DIR}
        ${COTP_INCLUDE_DIR}
        ${LIBZIP_INCLUDE_DIR}
        ${LIBPNG_INCLUDE_DIR}
        ${JANSSON_INCLUDE_DIR}
        ${ZBAR_INCLUDE_DIR})

link_directories(${GTK3_LIBRARY_DIRS}
        ${GCRYPT_LIBRARY_DIR}
        ${COTP_LIBRARY_DIR}
        ${LIBZIP_LIBRARY_DIR}
        ${LIBPNG_LIBRARY_DIR}
        ${JANSSON_LIBRARY_DIR}
        ${ZBAR_LIBRARY_DIR})

add_definitions(${GTK3_CFLAGS_OTHER}
        ${GCRYPT_CFLAGS_OTHER}
        ${COTP_CFLAGS_OTHER}
        ${LIBZIP_CFLAGS_OTHER}
        ${LIBPNG_CFLAGS_OTHER}
        ${JANSSON_CFLAGS_OTHER}
        ${ZBAR_CFLAGS_OTHER})

set(HEADER_FILES
        src/common.h
        src/file-size.h
        src/imports.h
        src/db-misc.h
        src/liststore-misc.h
        src/otpclient.h
        src/timer.h
        src/treeview.h
        src/manual-add-cb.h
        src/gquarks.h
        src/message-dialogs.h
        src/password-cb.h
        src/get-builder.h
        src/parse-uri.h
        src/add-common.h)

set(SOURCE_FILES
        src/treeview.c
        src/main.c
        src/app.c
        src/message-dialogs.c
        src/db-misc.c
        src/file-size.c
        src/manual-add-cb.c
        src/common.c
        src/timer.c
        src/liststore-misc.c
        src/authplus.c
        src/andotp.c
        src/parse-data.c
        src/gquarks.c
        src/imports.c
        src/password-cb.c
        src/webcam-add-cb.c
        src/get-builder.c
        src/parse-uri.c
        src/screenshot-add-cb.c
        src/add-common.c)

add_executable(${PROJECT_NAME} ${SOURCE_FILES} ${HEADER_FILES})
target_link_libraries(${PROJECT_NAME}
        ${GTK3_LIBRARIES}
        ${GCRYPT_LIBRARIES}
        ${COTP_LIBRARIES}
        ${LIBZIP_LIBRARIES}
        ${LIBPNG_LIBRARIES}
        ${JANSSON_LIBRARIES}
        ${ZBAR_LIBRARIES})

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "otpclient")

install(TARGETS ${PROJECT_NAME} DESTINATION bin)
install(FILES data/com.github.paolostivanin.OTPClient.desktop DESTINATION share/applications)
install(FILES data/com.github.paolostivanin.OTPClient.appdata.xml DESTINATION share/metainfo)

install(FILES src/ui/popover.ui DESTINATION share/otpclient)
install(FILES src/ui/webcam-diag.ui DESTINATION share/otpclient)
install(FILES data/icons/icon_x16.png DESTINATION share/icons/hicolor/16x16/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x24.png DESTINATION share/icons/hicolor/24x24/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x32.png DESTINATION share/icons/hicolor/32x32/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x48.png DESTINATION share/icons/hicolor/48x48/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x64.png DESTINATION share/icons/hicolor/64x64/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x128.png DESTINATION share/icons/hicolor/128x128/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x256.png DESTINATION share/icons/hicolor/256x256/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/icon_x512.png DESTINATION share/icons/hicolor/512x512/apps RENAME com.github.paolostivanin.OTPClient.png)
install(FILES data/icons/otpclient.svg DESTINATION share/icons/hicolor/scalable/apps RENAME com.github.paolostivanin.OTPClient.svg)

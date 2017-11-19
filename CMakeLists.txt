cmake_minimum_required(VERSION 3.5)
project(OTPClient)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(OTPCLIENT_VERSION_MAJOR "1")
set(OTPCLIENT_VERSION_MINOR "0")
set(OTPCLIENT_VERSION_PATCH "0")
set(OTPCLIENT_VERSION "${OTPCLIENT_VERSION_MAJOR}.${OTPCLIENT_VERSION_MINOR}.${OTPCLIENT_VERSION_PATCH}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -Wextra -O3 -Wformat=2 -Wmissing-format-attribute -fstack-protector-strong -Wundef")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pie -fPIE -fdiagnostics-color=always -Wstrict-prototypes -Wunreachable-code")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wwrite-strings -Wpointer-arith -Wbad-function-cast -Wcast-align")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers -Wno-return-type -Wno-cast-qual -Wno-sign-compare")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-add-needed -Wl,--as-needed -Wl,--no-undefined")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,relro,-z,now")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--no-add-needed -Wl,--as-needed")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,relro,-z,now")
endif()

find_package(PkgConfig REQUIRED)
find_package(Gcrypt 1.6.0 REQUIRED)
find_package(Baseencode 1.0.4 REQUIRED)
find_package(Cotp 1.0.10 REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)
pkg_check_modules(LIBZIP REQUIRED libzip)

include_directories(${GTK3_INCLUDE_DIRS}
        ${JSON_GLIB_INCLUDE_DIRS}
        ${GCRYPT_INCLUDE_DIR}
        ${BASEENCODE_INCLUDE_DIR}
        ${COTP_INCLUDE_DIR}
        ${LIBZIP_INCLUDE_DIRS})

link_directories(${GTK3_LIBRARY_DIRS}
        ${JSON_GLIB_LIBRARY_DIRS}
        ${GCRYPT_LIBRARY_DIRS}
        ${BASEENCODE_LIBRARY_DIRS}
        ${COTP_LIBRARY_DIRS}
        ${LIBZIP_LIBRARY_DIRS})

add_definitions(${GTK3_CFLAGS_OTHER}
        ${JSON_GLIB_CFLAGS_OTHER}
        ${GCRYPT_CFLAGS_OTHER}
        ${BASEENCODE_CFLAGS_OTHER}
        ${COTP_CFLAGS_OTHER}
        ${LIBZIP_CFLAGS_OTHER})

set(HEADER_FILES
        src/common.h
        src/file-size.h
        src/imports.h
        src/db-misc.h
        src/liststore-misc.h
        src/otpclient.h
        src/timer.h
        src/treeview.h
        src/add-data-dialog.h
        src/gquarks.h
        src/message-dialogs.h)

set(SOURCE_FILES
        src/treeview.c
        src/main.c
        src/app.c
        src/message-dialogs.c
        src/db-misc.c
        src/file-size.c
        src/add-data-dialog.c
        src/common.c
        src/timer.c
        src/liststore-misc.c
        src/authplus.c
        src/andotp.c
        src/parse-data.c
        src/gquarks.c
        src/imports.c)

add_executable(${PROJECT_NAME} ${SOURCE_FILES} ${HEADER_FILES})
target_link_libraries(${PROJECT_NAME}
        ${GTK3_LIBRARIES}
        ${JSON_GLIB_LIBRARIES}
        ${GCRYPT_LIBRARIES}
        ${BASEENCODE_LIBRARIES}
        ${COTP_LIBRARIES}
        ${LIBZIP_LIBRARIES})
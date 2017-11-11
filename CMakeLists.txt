cmake_minimum_required(VERSION 3.5)
project(OTPClient)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-Wall -Wextra -O3 -Wformat=2 -fstack-protector-all -fstack-protector-strong")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pie -fpie -fdiagnostics-color=always -Wstrict-prototypes -Wunreachable-code  -Wwrite-strings")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wpointer-arith -Wbad-function-cast -Wcast-align -Wno-missing-field-initializers")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-return-type -Wno-cast-qual -Wno-sign-compare -Wl,--sort-common,--as-needed,-z,relro")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_FILE_OFFSET_BITS=64 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3")


find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)

include_directories(${GTK3_INCLUDE_DIRS} ${JSON_GLIB_INCLUDE_DIRS})
link_directories(${GTK3_LIBRARY_DIRS} ${JSON_GLIB_LIBRARY_DIRS})

add_definitions(${GTK3_CFLAGS_OTHER} ${JSON_GLIB_CFLAGS_OTHER})

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
        src/gquarks.h src/message-dialogs.h)

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
        src/gquarks.c)

add_executable(${PROJECT_NAME} ${SOURCE_FILES} ${HEADER_FILES})
target_link_libraries(${PROJECT_NAME} ${GTK3_LIBRARIES} ${JSON_GLIB_LIBRARIES} -lgcrypt -lcotp -lbaseencode -lzip)
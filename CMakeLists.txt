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

include_directories(${GTK3_INCLUDE_DIRS})
link_directories(${GTK3_LIBRARY_DIRS})

add_definitions(${GTK3_CFLAGS_OTHER})

set(SOURCE_FILES src/treeview.c src/main.c src/app.c src/message-dialog.c src/kf-misc.c src/get-file-size.c src/add-data-dialog.c src/common.c)

add_executable(${PROJECT_NAME} ${SOURCE_FILES})
target_link_libraries(${PROJECT_NAME} ${GTK3_LIBRARIES} -lgcrypt -lcotp)
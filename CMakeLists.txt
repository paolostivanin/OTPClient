cmake_minimum_required(VERSION 3.20)
project(OTPClient VERSION "3.99.0" LANGUAGES "C")
include(GNUInstallDirs)

configure_file("src/common/version.h.in" "version.h")

set(GETTEXT_PACKAGE ${CMAKE_PROJECT_NAME})

include_directories(${PROJECT_BINARY_DIR})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(USE_FLATPAK_APP_FOLDER "Use flatpak app's config folder to store the database" OFF)
option(BUILD_GUI "Build the GUI" ON)
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
if(USE_FLATPAK_APP_FOLDER)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DUSE_FLATPAK_APP_FOLDER")
endif()
if(BUILD_GUI)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DBUILD_GUI")
endif()
if(BUILD_CLI)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DBUILD_CLI")
endif()
add_definitions(-DGETTEXT_PACKAGE=\"${GETTEXT_PACKAGE}\")


if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-add-needed -Wl,--as-needed -Wl,--no-undefined")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,relro,-z,now")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--no-add-needed -Wl,--as-needed")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,relro,-z,now")
endif()

find_package(PkgConfig REQUIRED)
find_package(Protobuf 3.20.0 REQUIRED)
find_package(Gcrypt 1.8.0 REQUIRED)
pkg_check_modules(COTP REQUIRED cotp>=1.2.1)
pkg_check_modules(BASEENCODE REQUIRED baseencode>=1.0.12)
pkg_check_modules(PNG REQUIRED libpng>=1.6.0)
pkg_check_modules(JANSSON REQUIRED jansson>=2.14.0)
pkg_check_modules(ZBAR REQUIRED zbar>=0.20)
pkg_check_modules(GTK4 REQUIRED gtk4>=4.10.0)
pkg_check_modules(GLIB2 REQUIRED glib-2.0>=2.76.0)
pkg_check_modules(UUID REQUIRED uuid>=2.34.0)
pkg_check_modules(PROTOC REQUIRED libprotobuf-c>=1.4.0)
pkg_check_modules(LIBSECRET REQUIRED libsecret-1>=0.20.0)
pkg_check_modules(LIBQRENCODE REQUIRED libqrencode>=4.1.0)

set(GUI_HEADER_FILES
)

set(GUI_SOURCE_FILES
)

set(CLI_HEADER_FILES
)

set(CLI_SOURCE_FILES
)

if(BUILD_GUI AND BUILD_CLI)
elseif(BUILD_GUI AND NOT BUILD_CLI)
elseif(BUILD_CLI AND NOT BUILD_GUI)
endif()

install(FILES data/com.github.paolostivanin.OTPClient.desktop DESTINATION share/applications)
install(FILES data/com.github.paolostivanin.OTPClient.appdata.xml DESTINATION share/metainfo)

install(FILES src/ui/otpclient.ui DESTINATION share/otpclient)
install(FILES src/ui/shortcuts.ui DESTINATION share/otpclient)

install(FILES man/otpclient.1.gz DESTINATION share/man/man1)
install(FILES man/otpclient-cli.1.gz DESTINATION share/man/man1)

install(FILES data/icons/com.github.paolostivanin.OTPClient.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES data/icons/com.github.paolostivanin.OTPClient-symbolic.svg DESTINATION share/icons/hicolor/scalable/apps)

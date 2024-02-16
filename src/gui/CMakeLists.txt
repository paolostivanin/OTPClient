# we need to add glib-compile-resources --generate-source --target=resources.c gui.gresource.xml
set(GLIB_FLAGS --target=resources.c)
set(GLIB_FLAGS ${GLIB_FLAGS} --generate-source)
set(GLIB_FLAGS ${GLIB_FLAGS} --sourcedir=${PROJECT_SOURCE_DIR}/src/gui)

add_custom_command(
        OUTPUT resources.c
        COMMAND glib-compile-resources ${PROJECT_SOURCE_DIR}/src/gui/gui.gresource.xml ${GLIB_FLAGS}
        DEPENDS gui.gresource.xml
        ui/window.ui
)

set(GUI_HEADER_FILES
        ../common/secret-schema.h
        otpclient-application.h
        otpclient-window.h
        otpclient-types.h
)

set(GUI_SOURCE_FILES
        ${CMAKE_CURRENT_BINARY_DIR}/resources.c
        ../common/secret-schema.c
        main.c
        otpclient-application.c
        otpclient-window.c)

include_directories(
        ${GTK4_INCLUDE_DIRS}
        ${ADWAITA_INCLUDE_DIRS}
        ${PNG_INCLUDE_DIRS}
        ${ZBAR_INCLUDE_DIRS}
        ${COMMON_INCDIRS}
)

add_executable(${PROJECT_NAME} ${GUI_SOURCE_FILES} ${GUI_HEADER_FILES})
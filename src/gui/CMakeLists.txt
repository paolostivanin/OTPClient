set(GRESOURCE_C ${CMAKE_CURRENT_BINARY_DIR}/resources.c)

add_custom_command(
        OUTPUT ${GRESOURCE_C}
        COMMAND glib-compile-resources
        ${PROJECT_SOURCE_DIR}/src/gui/gui.gresource.xml
        --target=${GRESOURCE_C}
        --generate-source
        --sourcedir=${PROJECT_SOURCE_DIR}/src/gui
        DEPENDS
        ${PROJECT_SOURCE_DIR}/src/gui/gui.gresource.xml
        ${PROJECT_SOURCE_DIR}/src/gui/ui/window.ui
)

file(GLOB GUI_HEADER_FILES
        *.h
        ../common/*.h
)

file(GLOB GUI_SOURCE_FILES
        *.c
        ../common/*.c
)

list(APPEND GUI_SOURCE_FILES ${GRESOURCE_C})

include_directories(
        ${GTK4_INCLUDE_DIRS}
        ${ADWAITA_INCLUDE_DIRS}
        ${PNG_INCLUDE_DIRS}
        ${ZBAR_INCLUDE_DIRS}
        ${COMMON_INCDIRS}
)

add_executable(${PROJECT_NAME} ${GUI_SOURCE_FILES} ${GUI_HEADER_FILES})
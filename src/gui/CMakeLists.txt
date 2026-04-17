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
        ${PROJECT_SOURCE_DIR}/src/gui/ui/shortcuts-window.ui
        ${PROJECT_SOURCE_DIR}/src/gui/ui/otp-button-row.css
)

file(GLOB GUI_HEADER_FILES
        *.h
        dialogs/*.h
        ../common/*.h
)

file(GLOB GUI_SOURCE_FILES
        *.c
        dialogs/*.c
        ../common/*.c
)

list(APPEND GUI_SOURCE_FILES ${GRESOURCE_C})

add_executable(${PROJECT_NAME} ${GUI_SOURCE_FILES} ${GUI_HEADER_FILES})
target_include_directories(${PROJECT_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
        ${PROJECT_SOURCE_DIR}/src/gui
)

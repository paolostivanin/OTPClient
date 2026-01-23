file(GLOB GUI_HEADER_FILES
        *.h
        ../common/*.h
)

file(GLOB GUI_SOURCE_FILES
        *.c
        ../common/*.c
)

add_executable(${PROJECT_NAME} ${GUI_SOURCE_FILES} ${GUI_HEADER_FILES})

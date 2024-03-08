file(GLOB GUI_HEADER_FILES
        *.h
        ../common/*.h
)

file(GLOB GUI_SOURCE_FILES
        *.c
        ../common/*.c
)

include_directories(
        ${PNG_INCLUDE_DIRS}
        ${ZBAR_INCLUDE_DIRS}
        ${COMMON_INCDIRS}
)

add_executable(${PROJECT_NAME} ${GUI_SOURCE_FILES} ${GUI_HEADER_FILES})
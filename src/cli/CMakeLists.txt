file(GLOB CLI_HEADER_FILES
        *.h
        ../common/*.h
)

file(GLOB CLI_SOURCE_FILES
        *.c
        ../common/*.c
)

add_executable(${PROJECT_NAME}-cli ${CLI_SOURCE_FILES} ${CLI_HEADER_FILES})

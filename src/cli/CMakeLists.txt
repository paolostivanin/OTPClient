set(CLI_HEADER_FILES
        ../common/secret-schema.h
        get-data.h
        help.h
)

set(CLI_SOURCE_FILES
        ../common/secret-schema.c
        get-data.c
        help.c
        main.c
)

include_directories(
        ${COMMON_INCDIRS}
)

add_executable(${PROJECT_NAME}-cli ${CLI_SOURCE_FILES} ${CLI_HEADER_FILES})
set(SEARCH_PROVIDER_SOURCE_FILES
        search-provider.c
        ../common/common.c
        ../common/db-common.c
        ../common/file-size.c
        ../common/gquarks.c
        ../common/secret-schema.c
)

set(SEARCH_PROVIDER_HEADER_FILES
        ../common/common.h
        ../common/db-common.h
        ../common/file-size.h
        ../common/gquarks.h
        ../common/secret-schema.h
)

add_executable(${PROJECT_NAME}-search-provider ${SEARCH_PROVIDER_SOURCE_FILES} ${SEARCH_PROVIDER_HEADER_FILES})
set_target_properties(${PROJECT_NAME}-search-provider PROPERTIES OUTPUT_NAME "otpclient-search-provider")

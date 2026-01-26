file(GLOB SEARCH_PROVIDER_HEADER_FILES
        *.h
        ../common/*.h
)

file(GLOB SEARCH_PROVIDER_SOURCE_FILES
        *.c
        ../common/*.c
)

add_executable(${PROJECT_NAME}-search-provider ${SEARCH_PROVIDER_SOURCE_FILES} ${SEARCH_PROVIDER_HEADER_FILES})
set_target_properties(${PROJECT_NAME}-search-provider PROPERTIES OUTPUT_NAME "otpclient-search-provider")

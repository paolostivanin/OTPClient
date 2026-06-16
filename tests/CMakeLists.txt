set(TEST_COMMON_SOURCES
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
        ${PROJECT_SOURCE_DIR}/src/common/parse-uri.c
)

add_executable(test_parse_uri
        test_parse_uri.c
        ${TEST_COMMON_SOURCES}
)
otpclient_apply_target_settings(test_parse_uri)
target_include_directories(test_parse_uri PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_parse_uri ${COMMON_LIBS})
add_test(NAME parse_uri COMMAND test_parse_uri)

add_executable(test_otp_validation
	test_otp_validation.c
	${PROJECT_SOURCE_DIR}/src/common/common.c
	${PROJECT_SOURCE_DIR}/src/common/file-size.c
	${PROJECT_SOURCE_DIR}/src/common/gquarks.c
	${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
)
otpclient_apply_target_settings(test_otp_validation)
target_include_directories(test_otp_validation PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_otp_validation ${COMMON_LIBS})
add_test(NAME otp_validation COMMAND test_otp_validation)

add_executable(test_db_transaction
        test_db_transaction.c
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/db-common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
)
otpclient_apply_target_settings(test_db_transaction)
target_compile_definitions(test_db_transaction PRIVATE OTPCLIENT_TESTING)
target_include_directories(test_db_transaction PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_db_transaction ${COMMON_LIBS})
add_test(NAME db_transaction COMMAND test_db_transaction)

add_executable(test_malformed_db
        test_malformed_db.c
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/db-common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
)
otpclient_apply_target_settings(test_malformed_db)
target_include_directories(test_malformed_db PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_malformed_db ${COMMON_LIBS})
add_test(NAME malformed_db COMMAND test_malformed_db)

add_executable(test_malformed_aegis
        test_malformed_aegis.c
        ${PROJECT_SOURCE_DIR}/src/common/aegis.c
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
        ${PROJECT_SOURCE_DIR}/src/common/parse-uri.c
)
otpclient_apply_target_settings(test_malformed_aegis)
target_include_directories(test_malformed_aegis PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_malformed_aegis ${COMMON_LIBS})
add_test(NAME malformed_aegis COMMAND test_malformed_aegis)

if(BUILD_GUI)
    add_executable(test_qrcode_parser
            test_qrcode_parser.c
            ${PROJECT_SOURCE_DIR}/src/gui/qrcode-parser.c
            ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
    )
    otpclient_apply_target_settings(test_qrcode_parser)
    target_include_directories(test_qrcode_parser PRIVATE
            ${PROJECT_SOURCE_DIR}/src/common
            ${PROJECT_SOURCE_DIR}/src/gui
    )
	target_link_libraries(test_qrcode_parser
	        PkgConfig::GTK4
	        PkgConfig::GDKPIXBUF
	        PkgConfig::ZBAR
	        PkgConfig::LIBQRENCODE
	        ${COMMON_LIBS}
	)
    add_test(NAME qrcode_parser COMMAND test_qrcode_parser)
endif()

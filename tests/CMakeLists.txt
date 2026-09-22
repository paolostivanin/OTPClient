set(TEST_COMMON_SOURCES
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
        ${PROJECT_SOURCE_DIR}/src/common/parse-uri.c
)

# A test that touches GSettings must find the schema from this checkout rather
# than whatever the developer happens to have installed, and must never write to
# the real one. Compile the schema into the build tree; the memory backend, set
# by the tests themselves, keeps the writes where they belong.
find_program(GLIB_COMPILE_SCHEMAS glib-compile-schemas)
if(GLIB_COMPILE_SCHEMAS)
    set(TEST_SCHEMA_DIR ${CMAKE_CURRENT_BINARY_DIR}/schemas)
    add_custom_command(
            OUTPUT ${TEST_SCHEMA_DIR}/gschemas.compiled
            COMMAND ${CMAKE_COMMAND} -E make_directory ${TEST_SCHEMA_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${PROJECT_SOURCE_DIR}/data/com.github.paolostivanin.OTPClient.gschema.xml
                    ${TEST_SCHEMA_DIR}/
            COMMAND ${GLIB_COMPILE_SCHEMAS} ${TEST_SCHEMA_DIR}
            DEPENDS ${PROJECT_SOURCE_DIR}/data/com.github.paolostivanin.OTPClient.gschema.xml
            COMMENT "Compiling the GSettings schema for the tests"
    )
    add_custom_target(test-schemas ALL DEPENDS ${TEST_SCHEMA_DIR}/gschemas.compiled)
endif()

# Point one test at the compiled schema. Without glib-compile-schemas the tests
# that need it skip themselves, so this is allowed to do nothing.
function(otpclient_use_test_schema test_name)
    if(GLIB_COMPILE_SCHEMAS)
        set_tests_properties(${test_name} PROPERTIES
                ENVIRONMENT "GSETTINGS_SCHEMA_DIR=${TEST_SCHEMA_DIR}")
    endif()
endfunction()

# Same deal for the build-time edge: there is no test-schemas target to depend on
# when the compiler is missing.
function(otpclient_depend_on_test_schema target_name)
    if(GLIB_COMPILE_SCHEMAS)
        add_dependencies(${target_name} test-schemas)
    endif()
endfunction()

# The integration tests below drive the real binaries under a private session bus
# and, for the GUI, a virtual display. None of that is needed to build or run
# OTPClient itself, so a missing tool has to skip the test rather than fail the
# configure step: a packager should not need Xvfb to produce a release build.
find_package(Python3 COMPONENTS Interpreter)
find_program(TEST_DBUS_DAEMON dbus-daemon)
find_program(TEST_XVFB Xvfb)

set(ISOLATED_SESSION_READY TRUE)
if(NOT Python3_Interpreter_FOUND OR NOT TEST_DBUS_DAEMON OR NOT GLIB_COMPILE_SCHEMAS)
    set(ISOLATED_SESSION_READY FALSE)
    message(STATUS "Not registering the isolated-session tests (review_flows, gui_flows, "
            "search_provider_settings): they need Python 3, dbus-daemon, and glib-compile-schemas")
endif()

# Exercise user flows against the same implementation as the shipped binaries.
file(GLOB REVIEW_COMMON_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/common/*.c)
add_executable(test_review_flows test_review_flows.c ${REVIEW_COMMON_SOURCES})
otpclient_apply_target_settings(test_review_flows)
target_compile_definitions(test_review_flows PRIVATE OTPCLIENT_TESTING
        REVIEW_TEST_SOURCE="${PROJECT_SOURCE_DIR}/tests")
target_include_directories(test_review_flows PRIVATE ${PROJECT_SOURCE_DIR}/src/common)
target_link_libraries(test_review_flows ${COMMON_LIBS})
otpclient_depend_on_test_schema(test_review_flows)
if(BUILD_CLI)
    target_compile_definitions(test_review_flows PRIVATE REVIEW_CLI_PATH="$<TARGET_FILE:OTPClient-cli>")
    add_dependencies(test_review_flows OTPClient-cli)
endif()
if(ISOLATED_SESSION_READY)
    add_test(NAME review_flows COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/isolated-session.py --headless $<TARGET_FILE:test_review_flows>)
    otpclient_use_test_schema(review_flows)
endif()

if(BUILD_SEARCH_PROVIDER)
    # Header-only: no provider sources, no bus, no schema. The point is to scan
    # the format strings, which needs nothing but GLib.
    add_executable(test_dbus_signatures test_dbus_signatures.c)
    otpclient_apply_target_settings(test_dbus_signatures)
    target_include_directories(test_dbus_signatures PRIVATE ${PROJECT_SOURCE_DIR}/src/search-provider)
    target_link_libraries(test_dbus_signatures ${COMMON_LIBS})
    add_test(NAME dbus_signatures COMMAND test_dbus_signatures)

    add_executable(test_search_provider_settings test_search_provider_settings.c
            ${REVIEW_COMMON_SOURCES})
    otpclient_apply_target_settings(test_search_provider_settings)
    target_include_directories(test_search_provider_settings PRIVATE ${PROJECT_SOURCE_DIR}/src/common)
    target_link_libraries(test_search_provider_settings ${COMMON_LIBS})
    otpclient_depend_on_test_schema(test_search_provider_settings)
    if(ISOLATED_SESSION_READY)
        add_test(NAME search_provider_settings COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tests/isolated-session.py --headless $<TARGET_FILE:test_search_provider_settings>)
        otpclient_use_test_schema(search_provider_settings)
    endif()
endif()

if(BUILD_GUI)
    get_target_property(REVIEW_GUI_SOURCES OTPClient SOURCES)
    list(FILTER REVIEW_GUI_SOURCES EXCLUDE REGEX "/main\\.c$")
    add_executable(test_gui_flows test_gui_flows.c ${REVIEW_GUI_SOURCES})
    add_dependencies(test_gui_flows OTPClient)
    otpclient_apply_target_settings(test_gui_flows)
    target_compile_definitions(test_gui_flows PRIVATE OTPCLIENT_TESTING)
    if(BUILD_CLI)
        target_compile_definitions(test_gui_flows PRIVATE REVIEW_CLI_PATH="$<TARGET_FILE:OTPClient-cli>")
        add_dependencies(test_gui_flows OTPClient-cli)
    endif()
    target_include_directories(test_gui_flows PRIVATE ${PROJECT_SOURCE_DIR}/src/common ${PROJECT_SOURCE_DIR}/src/gui)
    target_link_libraries(test_gui_flows PkgConfig::GTK4 PkgConfig::ADWAITA PkgConfig::GDKPIXBUF
            PkgConfig::ZBAR PkgConfig::PROTOC PkgConfig::LIBQRENCODE ${COMMON_LIBS})
    otpclient_depend_on_test_schema(test_gui_flows)
    if(ISOLATED_SESSION_READY AND TEST_XVFB)
        add_test(NAME gui_flows COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tests/isolated-session.py --gui $<TARGET_FILE:test_gui_flows>)
        set_tests_properties(gui_flows PROPERTIES TIMEOUT 100)
        otpclient_use_test_schema(gui_flows)
        if(SANITIZE)
            set_property(TEST gui_flows APPEND PROPERTY ENVIRONMENT
                    "LSAN_OPTIONS=suppressions=${PROJECT_SOURCE_DIR}/tests/gui-lsan.supp")
        endif()
    elseif(ISOLATED_SESSION_READY)
        message(STATUS "Not registering the gui_flows test: Xvfb was not found")
    endif()
endif()

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
# Enables the failure-injection hooks (otp_test_set_fail_invalid_append) the
# quarantine-append regression test uses.
target_compile_definitions(test_otp_validation PRIVATE OTPCLIENT_TESTING)
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

add_executable(test_otp_generation
        test_otp_generation.c
)
otpclient_apply_target_settings(test_otp_generation)
target_link_libraries(test_otp_generation ${COMMON_LIBS})
add_test(NAME otp_generation COMMAND test_otp_generation)

add_executable(test_db_roundtrip
        test_db_roundtrip.c
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/db-common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
)
otpclient_apply_target_settings(test_db_roundtrip)
target_include_directories(test_db_roundtrip PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_db_roundtrip ${COMMON_LIBS})
add_test(NAME db_roundtrip COMMAND test_db_roundtrip)

add_executable(test_cli_hotp
        test_cli_hotp.c
        ${PROJECT_SOURCE_DIR}/src/cli/get-data.c
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/db-common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
)
otpclient_apply_target_settings(test_cli_hotp)
target_compile_definitions(test_cli_hotp PRIVATE OTPCLIENT_TESTING)
target_include_directories(test_cli_hotp PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
        ${PROJECT_SOURCE_DIR}/src/cli
)
target_link_libraries(test_cli_hotp ${COMMON_LIBS})
add_test(NAME cli_hotp COMMAND test_cli_hotp)

add_executable(test_parse_uri_extra
        test_parse_uri_extra.c
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
        ${PROJECT_SOURCE_DIR}/src/common/parse-uri.c
)
otpclient_apply_target_settings(test_parse_uri_extra)
target_include_directories(test_parse_uri_extra PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_parse_uri_extra ${COMMON_LIBS})
add_test(NAME parse_uri_extra COMMAND test_parse_uri_extra)

add_executable(test_malformed_importers
        test_malformed_importers.c
        ${PROJECT_SOURCE_DIR}/src/common/authpro.c
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/freeotp.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
        ${PROJECT_SOURCE_DIR}/src/common/parse-uri.c
        ${PROJECT_SOURCE_DIR}/src/common/twofas.c
)
otpclient_apply_target_settings(test_malformed_importers)
target_include_directories(test_malformed_importers PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_malformed_importers ${COMMON_LIBS})
add_test(NAME malformed_importers COMMAND test_malformed_importers)

add_executable(test_common_utils
        test_common_utils.c
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
)
otpclient_apply_target_settings(test_common_utils)
target_include_directories(test_common_utils PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_common_utils ${COMMON_LIBS})
add_test(NAME common_utils COMMAND test_common_utils)

add_executable(test_field_bounds_roundtrip
        test_field_bounds_roundtrip.c
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/db-common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
)
otpclient_apply_target_settings(test_field_bounds_roundtrip)
target_include_directories(test_field_bounds_roundtrip PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_field_bounds_roundtrip ${COMMON_LIBS})
add_test(NAME field_bounds_roundtrip COMMAND test_field_bounds_roundtrip)

add_executable(test_memlock_sizing
        test_memlock_sizing.c
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
)
otpclient_apply_target_settings(test_memlock_sizing)
target_include_directories(test_memlock_sizing PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_memlock_sizing ${COMMON_LIBS})
add_test(NAME memlock_sizing COMMAND test_memlock_sizing)

add_executable(test_settings_import
        test_settings_import.c
        ${PROJECT_SOURCE_DIR}/src/common/common.c
        ${PROJECT_SOURCE_DIR}/src/common/file-size.c
        ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
        ${PROJECT_SOURCE_DIR}/src/common/gsettings-common.c
        ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
        ${PROJECT_SOURCE_DIR}/src/common/settings-import-export.c
)
otpclient_apply_target_settings(test_settings_import)
target_include_directories(test_settings_import PRIVATE
        ${PROJECT_SOURCE_DIR}/src/common
)
target_link_libraries(test_settings_import
        PkgConfig::JANSSON
        ${COMMON_LIBS}
)
add_test(NAME settings_import COMMAND test_settings_import)
otpclient_use_test_schema(settings_import)

add_executable(test_tray_menu_model
        test_tray_menu_model.c
        ${PROJECT_SOURCE_DIR}/src/gui/tray-menu-model.c
)
otpclient_apply_target_settings(test_tray_menu_model)
target_include_directories(test_tray_menu_model PRIVATE
        ${PROJECT_SOURCE_DIR}/src/gui
)
target_link_libraries(test_tray_menu_model ${COMMON_LIBS})
add_test(NAME tray_menu_model COMMAND test_tray_menu_model)

if(BUILD_GUI)
    add_executable(test_otp_entry
            test_otp_entry.c
            ${PROJECT_SOURCE_DIR}/src/gui/otp-entry.c
            ${PROJECT_SOURCE_DIR}/src/common/common.c
            ${PROJECT_SOURCE_DIR}/src/common/file-size.c
            ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
            ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
    )
    otpclient_apply_target_settings(test_otp_entry)
    target_compile_definitions(test_otp_entry PRIVATE OTPCLIENT_TESTING)
    target_include_directories(test_otp_entry PRIVATE
            ${PROJECT_SOURCE_DIR}/src/common
            ${PROJECT_SOURCE_DIR}/src/gui
    )
    target_link_libraries(test_otp_entry
            PkgConfig::GTK4
            ${COMMON_LIBS}
    )
    add_test(NAME otp_entry COMMAND test_otp_entry)

    add_executable(test_db_relocation
            test_db_relocation.c
            ${PROJECT_SOURCE_DIR}/src/gui/database-sidebar.c
            ${PROJECT_SOURCE_DIR}/src/gui/gui-misc.c
            ${PROJECT_SOURCE_DIR}/src/common/common.c
            ${PROJECT_SOURCE_DIR}/src/common/file-size.c
            ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
            ${PROJECT_SOURCE_DIR}/src/common/gsettings-common.c
            ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
    )
    otpclient_apply_target_settings(test_db_relocation)
    target_include_directories(test_db_relocation PRIVATE
            ${PROJECT_SOURCE_DIR}/src/common
            ${PROJECT_SOURCE_DIR}/src/gui
    )
    target_link_libraries(test_db_relocation
            PkgConfig::GTK4
            ${COMMON_LIBS}
    )
    add_test(NAME db_relocation COMMAND test_db_relocation)
    otpclient_use_test_schema(db_relocation)
    otpclient_depend_on_test_schema(test_db_relocation)

    # No GTK and no D-Bus: the decision table is deliberately free of both.
    add_executable(test_autostart_policy
            test_autostart_policy.c
            ${PROJECT_SOURCE_DIR}/src/gui/autostart-policy.c
    )
    otpclient_apply_target_settings(test_autostart_policy)
    target_include_directories(test_autostart_policy PRIVATE
            ${PROJECT_SOURCE_DIR}/src/common
            ${PROJECT_SOURCE_DIR}/src/gui
    )
    target_link_libraries(test_autostart_policy ${COMMON_LIBS})
    add_test(NAME autostart_policy COMMAND test_autostart_policy)

    add_executable(test_google_migration
            test_google_migration.c
            ${PROJECT_SOURCE_DIR}/src/gui/google-migration.c
            ${PROJECT_SOURCE_DIR}/src/gui/google-migration.pb-c.c
            ${PROJECT_SOURCE_DIR}/src/common/common.c
            ${PROJECT_SOURCE_DIR}/src/common/file-size.c
            ${PROJECT_SOURCE_DIR}/src/common/gquarks.c
            ${PROJECT_SOURCE_DIR}/src/common/otp-validation.c
    )
    otpclient_apply_target_settings(test_google_migration)
    target_include_directories(test_google_migration PRIVATE
            ${PROJECT_SOURCE_DIR}/src/common
            ${PROJECT_SOURCE_DIR}/src/gui
    )
    target_link_libraries(test_google_migration
            PkgConfig::PROTOC
            ${COMMON_LIBS}
    )
    add_test(NAME google_migration COMMAND test_google_migration)

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

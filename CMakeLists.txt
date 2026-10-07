cmake_minimum_required(VERSION 3.20)

project(
    ROCVehicleClient
    VERSION 0.2.1
    DESCRIPTION "C++17 vehicle communication library for ROC-SYSTEM"
    LANGUAGES C CXX
)

# Software pairing metadata, not a Device Protocol or resource schema version.
set(ROC_VEHICLE_FULL_VERSION "${PROJECT_VERSION}-rc2")
set(ROC_VEHICLE_SYSTEM_COMMIT "a9c601d6977d89000438854a24e3199573004a2f")
configure_file(src/build_version.hpp.in generated/build_version.hpp @ONLY)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

find_path(JSONCONS_INCLUDE_DIR jsoncons/json.hpp REQUIRED)
find_path(BOOST_INCLUDE_DIR boost/beast.hpp REQUIRED)
find_package(OpenSSL 3.5 REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

option(ROC_VEHICLE_BUILD_EXAMPLES "Build ROC vehicle client examples" ON)
option(ROC_VEHICLE_BUILD_TESTS "Build ROC vehicle client tests" ${BUILD_TESTING})
option(ROC_VEHICLE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(ROC_VEHICLE_BUILD_DRIVER "Build the Linux single-vehicle integration driver" OFF)

add_library(roc_vehicle_client
    src/artifact_receiver.cpp
    src/crypto.cpp
    src/config.cpp
    src/contract_validation.cpp
    src/validation/wire_rules.cpp
    src/beast_websocket_transport.cpp
    src/device_client.cpp
    src/endpoint.cpp
    src/file_sink.cpp
    src/logging.cpp
    src/package_sink.cpp
    src/package_receiver.cpp
    src/resource_failure.cpp
    src/protocol.cpp
    src/road_network.cpp
    src/road_network_v2.cpp
    src/road_receiver.cpp
    src/session_core.cpp
    src/state_store.cpp
    src/task_http_client.cpp
    src/task_protocol.cpp
    src/version.cpp
)
add_library(roc::vehicle_client ALIAS roc_vehicle_client)

target_compile_features(roc_vehicle_client PUBLIC cxx_std_17)
target_include_directories(roc_vehicle_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(roc_vehicle_client PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${BOOST_INCLUDE_DIR}
    ${JSONCONS_INCLUDE_DIR}
)
target_link_libraries(roc_vehicle_client PRIVATE
    OpenSSL::Crypto
    OpenSSL::SSL
    SQLite::SQLite3
    Threads::Threads
)

if(MSVC)
    target_compile_options(roc_vehicle_client PRIVATE /W4 /permissive-)
    if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
        target_compile_options(roc_vehicle_client PRIVATE /WX)
    endif()
else()
    target_compile_options(roc_vehicle_client PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
        target_compile_options(roc_vehicle_client PRIVATE -Werror)
    endif()
endif()

set_target_properties(roc_vehicle_client PROPERTIES
    CXX_EXTENSIONS OFF
    EXPORT_NAME vehicle_client
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)

if(ROC_VEHICLE_BUILD_EXAMPLES)
    add_executable(roc_vehicle_info examples/vehicle_info.cpp)
    target_link_libraries(roc_vehicle_info PRIVATE roc::vehicle_client)
endif()

if(ROC_VEHICLE_BUILD_DRIVER)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The driver currently requires Linux private paths and flock")
    endif()
    add_library(roc_vehicle_driver_support STATIC apps/driver_support.cpp apps/driver_context.cpp)
    target_include_directories(roc_vehicle_driver_support PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_include_directories(roc_vehicle_driver_support SYSTEM PRIVATE ${JSONCONS_INCLUDE_DIR})
    target_link_libraries(roc_vehicle_driver_support PUBLIC roc::vehicle_client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    add_executable(roc_vehicle_driver apps/vehicle_driver.cpp)
    target_include_directories(roc_vehicle_driver PRIVATE apps src)
    target_include_directories(roc_vehicle_driver SYSTEM PRIVATE ${JSONCONS_INCLUDE_DIR})
    target_link_libraries(roc_vehicle_driver PRIVATE roc_vehicle_driver_support)
    foreach(driver_target roc_vehicle_driver_support roc_vehicle_driver)
        target_compile_options(${driver_target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
            target_compile_options(${driver_target} PRIVATE -Werror)
        endif()
    endforeach()
endif()

if(ROC_VEHICLE_BUILD_TESTS)
    if(ROC_VEHICLE_BUILD_DRIVER)
        add_executable(roc_vehicle_driver_crash_probe apps/vehicle_driver.cpp tests/integration/driver_crash_probe.cpp)
        target_include_directories(roc_vehicle_driver_crash_probe PRIVATE apps src)
        target_include_directories(roc_vehicle_driver_crash_probe SYSTEM PRIVATE ${JSONCONS_INCLUDE_DIR})
        target_link_libraries(roc_vehicle_driver_crash_probe PRIVATE roc_vehicle_driver_support SQLite::SQLite3)
        target_compile_definitions(roc_vehicle_driver_crash_probe PRIVATE ROC_VEHICLE_TEST_INSTRUMENTATION=1)
        target_link_options(roc_vehicle_driver_crash_probe PRIVATE -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_exec)
        add_executable(roc_vehicle_probe_boundary_tests tests/unit/probe_boundary_tests.cpp tests/integration/driver_crash_probe.cpp)
        target_link_libraries(roc_vehicle_probe_boundary_tests PRIVATE roc::vehicle_client SQLite::SQLite3)
        target_link_options(roc_vehicle_probe_boundary_tests PRIVATE -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_exec)
        target_compile_options(roc_vehicle_probe_boundary_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
            target_compile_options(roc_vehicle_probe_boundary_tests PRIVATE -Werror)
        endif()
        add_test(NAME driver_probe_boundary COMMAND roc_vehicle_probe_boundary_tests)
        set_tests_properties(driver_probe_boundary PROPERTIES TIMEOUT 10)
        add_executable(roc_vehicle_driver_joint_tests tests/integration/driver_joint_tests.cpp)
        add_executable(roc_vehicle_runtime_inputs tests/integration/runtime_inputs.cpp)
        target_include_directories(roc_vehicle_runtime_inputs SYSTEM PRIVATE ${JSONCONS_INCLUDE_DIR})
        target_link_libraries(roc_vehicle_runtime_inputs PRIVATE roc::vehicle_client)
        target_include_directories(roc_vehicle_driver_joint_tests PRIVATE apps src ${CMAKE_CURRENT_BINARY_DIR}/generated)
        target_include_directories(roc_vehicle_driver_joint_tests SYSTEM PRIVATE ${JSONCONS_INCLUDE_DIR} ${BOOST_INCLUDE_DIR})
        target_link_libraries(roc_vehicle_driver_joint_tests PRIVATE roc_vehicle_driver_support OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(roc_vehicle_driver_joint_tests PRIVATE
            ROC_DRIVER_EXECUTABLE="$<TARGET_FILE:roc_vehicle_driver>"
            ROC_DRIVER_CRASH_PROBE="$<TARGET_FILE:roc_vehicle_driver_crash_probe>")
        add_dependencies(roc_vehicle_driver_joint_tests roc_vehicle_driver roc_vehicle_driver_crash_probe)
        foreach(target roc_vehicle_driver_joint_tests roc_vehicle_driver_crash_probe roc_vehicle_runtime_inputs)
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
            if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
                target_compile_options(${target} PRIVATE -Werror)
            endif()
        endforeach()
        add_test(NAME driver_prechecks COMMAND roc_vehicle_driver_joint_tests --prechecks)
        add_test(NAME driver_joint COMMAND roc_vehicle_driver_joint_tests --safe)
        add_test(NAME driver_process_recovery COMMAND roc_vehicle_driver_joint_tests --faults)
        add_test(NAME driver_context COMMAND roc_vehicle_driver_joint_tests --context)
        set_tests_properties(driver_prechecks driver_joint driver_process_recovery driver_context PROPERTIES TIMEOUT 180)
    endif()
    add_executable(roc_vehicle_road_v2_tests tests/unit/road_network_v2_tests.cpp)
    target_include_directories(roc_vehicle_road_v2_tests SYSTEM PRIVATE ${JSONCONS_INCLUDE_DIR})
    target_include_directories(roc_vehicle_road_v2_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(roc_vehicle_road_v2_tests PRIVATE roc::vehicle_client)
    add_test(NAME road_v2 COMMAND roc_vehicle_road_v2_tests)
    add_test(NAME road_v2_limits COMMAND roc_vehicle_road_v2_tests --limits)
    set_tests_properties(road_v2 road_v2_limits PROPERTIES TIMEOUT 180)
    add_executable(roc_vehicle_road_http_tests tests/integration/road_http_tests.cpp)
    target_include_directories(roc_vehicle_road_http_tests SYSTEM PRIVATE ${JSONCONS_INCLUDE_DIR} ${BOOST_INCLUDE_DIR})
    target_link_libraries(roc_vehicle_road_http_tests PRIVATE roc::vehicle_client)
    add_test(NAME road_http COMMAND roc_vehicle_road_http_tests)
    set_tests_properties(road_http PROPERTIES TIMEOUT 30)
    if(NOT MSVC)
        foreach(road_target roc_vehicle_road_v2_tests roc_vehicle_road_http_tests)
            target_compile_options(${road_target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
            if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
                target_compile_options(${road_target} PRIVATE -Werror)
            endif()
        endforeach()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(roc_vehicle_v08_network_fault_tests tests/integration/v08_network_fault_tests.cpp)
        target_include_directories(roc_vehicle_v08_network_fault_tests SYSTEM PRIVATE ${BOOST_INCLUDE_DIR} ${JSONCONS_INCLUDE_DIR})
        target_link_libraries(roc_vehicle_v08_network_fault_tests PRIVATE roc::vehicle_client OpenSSL::SSL OpenSSL::Crypto)
        target_compile_options(roc_vehicle_v08_network_fault_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
            target_compile_options(roc_vehicle_v08_network_fault_tests PRIVATE -Werror)
        endif()
        add_test(NAME v08_network_faults COMMAND roc_vehicle_v08_network_fault_tests)
        set_tests_properties(v08_network_faults PROPERTIES TIMEOUT 120)
        add_executable(roc_vehicle_v08_wire_recovery_tests tests/integration/v08_wire_recovery_tests.cpp)
        target_include_directories(roc_vehicle_v08_wire_recovery_tests SYSTEM PRIVATE ${BOOST_INCLUDE_DIR} ${JSONCONS_INCLUDE_DIR})
        target_link_libraries(roc_vehicle_v08_wire_recovery_tests PRIVATE roc::vehicle_client)
        target_compile_options(roc_vehicle_v08_wire_recovery_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
            target_compile_options(roc_vehicle_v08_wire_recovery_tests PRIVATE -Werror)
        endif()
        add_test(NAME v08_wire_recovery COMMAND roc_vehicle_v08_wire_recovery_tests)
        set_tests_properties(v08_wire_recovery PROPERTIES TIMEOUT 120)
        add_executable(roc_vehicle_storage_security_tests tests/unit/storage_security_tests.cpp)
        target_include_directories(roc_vehicle_storage_security_tests SYSTEM PRIVATE ${JSONCONS_INCLUDE_DIR})
        target_link_libraries(roc_vehicle_storage_security_tests PRIVATE roc::vehicle_client)
        target_compile_options(roc_vehicle_storage_security_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
            target_compile_options(roc_vehicle_storage_security_tests PRIVATE -Werror)
        endif()
        add_test(NAME storage_security COMMAND roc_vehicle_storage_security_tests)
        add_executable(roc_vehicle_package_tests tests/unit/package_tests.cpp)
        target_include_directories(roc_vehicle_package_tests PRIVATE ${JSONCONS_INCLUDE_DIR})
        target_link_libraries(roc_vehicle_package_tests PRIVATE roc::vehicle_client)
        add_test(NAME package COMMAND roc_vehicle_package_tests)
        add_executable(roc_vehicle_package_fault_tests tests/integration/package_fault_tests.cpp)
        target_link_libraries(roc_vehicle_package_fault_tests PRIVATE roc::vehicle_client)
        target_link_options(roc_vehicle_package_fault_tests PRIVATE -Wl,--wrap=fsync -Wl,--wrap=renameat2 -Wl,--wrap=write)
        add_test(NAME package_faults COMMAND roc_vehicle_package_fault_tests)
        add_executable(roc_vehicle_package_http_tests tests/integration/package_http_tests.cpp)
        target_include_directories(roc_vehicle_package_http_tests PRIVATE ${BOOST_INCLUDE_DIR})
        target_link_libraries(roc_vehicle_package_http_tests PRIVATE roc::vehicle_client)
        add_test(NAME package_http COMMAND roc_vehicle_package_http_tests)
        set_tests_properties(package package_faults package_http PROPERTIES TIMEOUT 30)
    endif()
    add_executable(roc_vehicle_sys001_contract_tests tests/unit/sys001_contract_tests.cpp)
    target_include_directories(roc_vehicle_sys001_contract_tests PRIVATE ${JSONCONS_INCLUDE_DIR})
    target_link_libraries(roc_vehicle_sys001_contract_tests PRIVATE roc::vehicle_client SQLite::SQLite3)
    add_test(NAME sys001_contract COMMAND roc_vehicle_sys001_contract_tests)
    add_executable(roc_vehicle_wire_validation_tests tests/unit/wire_validation_tests.cpp)
    target_include_directories(roc_vehicle_wire_validation_tests PRIVATE src)
    target_include_directories(roc_vehicle_wire_validation_tests SYSTEM PRIVATE ${JSONCONS_INCLUDE_DIR})
    target_link_libraries(roc_vehicle_wire_validation_tests PRIVATE roc::vehicle_client)
    if(NOT MSVC)
        target_compile_options(roc_vehicle_wire_validation_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
            target_compile_options(roc_vehicle_wire_validation_tests PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME wire_validation COMMAND roc_vehicle_wire_validation_tests)
    add_executable(roc_vehicle_foundation_tests tests/unit/foundation_tests.cpp)
    target_link_libraries(roc_vehicle_foundation_tests PRIVATE roc::vehicle_client)
    add_test(NAME foundation COMMAND roc_vehicle_foundation_tests)

    add_executable(roc_vehicle_protocol_tests tests/unit/protocol_tests.cpp)
    target_link_libraries(roc_vehicle_protocol_tests PRIVATE roc::vehicle_client)
    target_compile_definitions(roc_vehicle_protocol_tests PRIVATE
        ROC_VEHICLE_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    add_test(NAME protocol COMMAND roc_vehicle_protocol_tests)

    add_executable(roc_vehicle_state_store_tests tests/unit/state_store_tests.cpp)
    target_link_libraries(roc_vehicle_state_store_tests PRIVATE
        roc::vehicle_client
        SQLite::SQLite3
    )
    add_test(NAME state_store COMMAND roc_vehicle_state_store_tests)

    add_executable(roc_vehicle_task_protocol_tests tests/unit/task_protocol_tests.cpp)
    target_link_libraries(roc_vehicle_task_protocol_tests PRIVATE roc::vehicle_client)
    add_test(NAME task_protocol COMMAND roc_vehicle_task_protocol_tests)

    add_executable(roc_vehicle_road_network_tests tests/unit/road_network_tests.cpp)
    target_link_libraries(roc_vehicle_road_network_tests PRIVATE roc::vehicle_client)
    add_test(NAME road_network COMMAND roc_vehicle_road_network_tests)

    add_executable(roc_vehicle_file_sink_tests tests/unit/file_sink_tests.cpp)
    target_link_libraries(roc_vehicle_file_sink_tests PRIVATE roc::vehicle_client)
    add_test(NAME file_sink COMMAND roc_vehicle_file_sink_tests)

    add_executable(roc_vehicle_artifact_receiver_tests tests/unit/artifact_receiver_tests.cpp)
    target_link_libraries(roc_vehicle_artifact_receiver_tests PRIVATE roc::vehicle_client)
    add_test(NAME artifact_receiver COMMAND roc_vehicle_artifact_receiver_tests)

    add_executable(roc_vehicle_task_http_client_tests tests/unit/task_http_client_tests.cpp)
    target_include_directories(roc_vehicle_task_http_client_tests PRIVATE ${BOOST_INCLUDE_DIR})
    target_link_libraries(roc_vehicle_task_http_client_tests PRIVATE
        roc::vehicle_client
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    add_test(NAME task_http_client COMMAND roc_vehicle_task_http_client_tests)
    set_tests_properties(task_http_client PROPERTIES TIMEOUT 60)

    add_executable(roc_vehicle_session_core_tests tests/unit/session_core_tests.cpp)
    target_include_directories(roc_vehicle_session_core_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(roc_vehicle_session_core_tests PRIVATE roc::vehicle_client)
    add_test(NAME session_core COMMAND roc_vehicle_session_core_tests)

    add_executable(roc_vehicle_device_client_tests tests/unit/device_client_tests.cpp)
    target_include_directories(roc_vehicle_device_client_tests PRIVATE ${BOOST_INCLUDE_DIR})
    target_link_libraries(roc_vehicle_device_client_tests PRIVATE roc::vehicle_client)
    add_test(NAME device_client COMMAND roc_vehicle_device_client_tests)

    add_executable(roc_vehicle_device_client_stress_tests
        tests/integration/device_client_stress_tests.cpp
    )
    target_include_directories(roc_vehicle_device_client_stress_tests PRIVATE
        ${BOOST_INCLUDE_DIR}
    )
    target_link_libraries(roc_vehicle_device_client_stress_tests PRIVATE
        roc::vehicle_client
    )
    add_test(NAME device_client_stress COMMAND roc_vehicle_device_client_stress_tests)
    set_tests_properties(device_client_stress PROPERTIES TIMEOUT 20)

    add_executable(roc_vehicle_fault_recovery_tests
        tests/integration/fault_recovery_tests.cpp
    )
    target_link_libraries(roc_vehicle_fault_recovery_tests PRIVATE
        roc::vehicle_client
        SQLite::SQLite3
    )
    add_test(NAME fault_recovery COMMAND roc_vehicle_fault_recovery_tests)

    if(UNIX)
        add_executable(roc_vehicle_v08_delivery_recovery_tests tests/integration/v08_delivery_recovery_tests.cpp)
        target_include_directories(roc_vehicle_v08_delivery_recovery_tests SYSTEM PRIVATE ${JSONCONS_INCLUDE_DIR})
        target_link_libraries(roc_vehicle_v08_delivery_recovery_tests PRIVATE roc::vehicle_client SQLite::SQLite3)
        target_compile_options(roc_vehicle_v08_delivery_recovery_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
            target_compile_options(roc_vehicle_v08_delivery_recovery_tests PRIVATE -Werror)
        endif()
        add_test(NAME v08_delivery_recovery COMMAND roc_vehicle_v08_delivery_recovery_tests)
        set_tests_properties(v08_delivery_recovery PROPERTIES TIMEOUT 60)
        add_executable(roc_vehicle_v08_state_fault_tests tests/integration/v08_state_fault_tests.cpp)
        target_link_libraries(roc_vehicle_v08_state_fault_tests PRIVATE roc::vehicle_client SQLite::SQLite3)
        target_link_options(roc_vehicle_v08_state_fault_tests PRIVATE -Wl,--wrap=sqlite3_step -Wl,--wrap=sqlite3_exec)
        target_compile_options(roc_vehicle_v08_state_fault_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(ROC_VEHICLE_WARNINGS_AS_ERRORS)
            target_compile_options(roc_vehicle_v08_state_fault_tests PRIVATE -Werror)
        endif()
        add_test(NAME v08_state_faults COMMAND roc_vehicle_v08_state_fault_tests)
        set_tests_properties(v08_state_faults PROPERTIES TIMEOUT 60)
        add_executable(roc_vehicle_file_sink_fault_tests
            tests/integration/file_sink_fault_tests.cpp
        )
        target_link_libraries(roc_vehicle_file_sink_fault_tests PRIVATE
            roc::vehicle_client
        )
        target_include_directories(roc_vehicle_file_sink_fault_tests SYSTEM PRIVATE ${JSONCONS_INCLUDE_DIR})
        target_link_options(roc_vehicle_file_sink_fault_tests PRIVATE
            -Wl,--wrap=fsync
            -Wl,--wrap=rename
            -Wl,--wrap=write
        )
        add_test(NAME file_sink_faults COMMAND roc_vehicle_file_sink_fault_tests)

        add_executable(roc_vehicle_state_store_fault_tests
            tests/integration/state_store_fault_tests.cpp
        )
        target_link_libraries(roc_vehicle_state_store_fault_tests PRIVATE
            roc::vehicle_client
            SQLite::SQLite3
        )
        target_link_options(roc_vehicle_state_store_fault_tests PRIVATE
            -Wl,--wrap=sqlite3_step
        )
        add_test(NAME state_store_faults COMMAND roc_vehicle_state_store_fault_tests)
    endif()
endif()

install(TARGETS roc_vehicle_client
    EXPORT ROCVehicleClientTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/ROCVehicleClient)
install(EXPORT ROCVehicleClientTargets
    FILE ROCVehicleClientTargets.cmake
    NAMESPACE roc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ROCVehicleClient
)

configure_package_config_file(
    cmake/ROCVehicleClientConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ROCVehicleClientConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ROCVehicleClient
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ROCVehicleClientConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ROCVehicleClientConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ROCVehicleClientConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ROCVehicleClient
)

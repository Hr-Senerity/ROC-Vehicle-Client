cmake_minimum_required(VERSION 3.20)
project(ROCVehicleInstalledConsumer LANGUAGES CXX)
find_package(ROCVehicleClient 0.2.1 EXACT CONFIG REQUIRED)
if(NOT ROCVehicleClient_FULL_VERSION STREQUAL "0.2.1-rc2" OR
   NOT ROCVehicleClient_SYSTEM_COMMIT STREQUAL "a9c601d6977d89000438854a24e3199573004a2f")
    message(FATAL_ERROR "Installed version or frozen SYSTEM provenance mismatch")
endif()
add_executable(roc_vehicle_installed_consumer main.cpp)
target_link_libraries(roc_vehicle_installed_consumer PRIVATE roc::vehicle_client)
set_target_properties(roc_vehicle_installed_consumer PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_options(roc_vehicle_installed_consumer PRIVATE
    -Wall -Wextra -Wpedantic -Werror)

cmake_minimum_required(VERSION 3.21)

# Use VCPKG_ROOT when the caller did not provide a toolchain explicitly.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file")
endif()

set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg target triplet")

project(RisaAllInOneMenu VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_BUILD_TESTS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/CharmedBaryon/CommonLibSSE-NG.git
    GIT_TAG b93280e832f263dbef44e44cbe2936622a02f91a
)
FetchContent_MakeAvailable(CommonLibSSE)

find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(minhook CONFIG REQUIRED)

add_library(${PROJECT_NAME} SHARED src/main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    fmt::fmt
    spdlog::spdlog
    minhook::minhook
    dinput8
    dxguid
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "1RisaAllInOneMenu"
)

# Output the DLL directly into the mod's SKSE/Plugins folder so MO2 deploys it automatically
set(OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../SKSE/Plugins")
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE        "${OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG          "${OUTPUT_DIR}"
)

# Remove the pre-prefix filename so SKSE never loads two copies of the plugin.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f "${OUTPUT_DIR}/RisaAllInOneMenu.dll"
)

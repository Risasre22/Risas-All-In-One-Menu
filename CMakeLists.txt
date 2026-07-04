cmake_minimum_required(VERSION 3.21)

# Use VCPKG_ROOT when the caller did not provide a toolchain explicitly.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file")
endif()

set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg target triplet")

project(RisaAllInOneMenu VERSION 1.5.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep local source and build paths out of release binaries.
if(MSVC)
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" RISA_SOURCE_DIR_NATIVE)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" RISA_BINARY_DIR_NATIVE)
    string(TOUPPER "${RISA_SOURCE_DIR_NATIVE}" RISA_SOURCE_DIR_NATIVE_UPPER)
    string(TOUPPER "${RISA_BINARY_DIR_NATIVE}" RISA_BINARY_DIR_NATIVE_UPPER)

    add_compile_options(
        "/experimental:deterministic"
        "/pathmap:${RISA_BINARY_DIR_NATIVE}=."
        "/pathmap:${RISA_BINARY_DIR_NATIVE_UPPER}=."
        "/pathmap:${RISA_SOURCE_DIR_NATIVE}=."
        "/pathmap:${RISA_SOURCE_DIR_NATIVE_UPPER}=."
    )
endif()

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
find_package(nlohmann_json CONFIG REQUIRED)

add_library(${PROJECT_NAME} SHARED src/main.cpp src/reshade_addon.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    fmt::fmt
    spdlog::spdlog
    minhook::minhook
    nlohmann_json::nlohmann_json
    dinput8
    dxguid
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "1RisaAllInOneMenu"
)

# Optional local deployment. Clean builds remain under build/<config> unless this cache
# variable is explicitly provided by the developer.
set(RISA_DEPLOY_DIR "" CACHE PATH "Optional SKSE/Plugins directory for automatic DLL deployment.")
if(RISA_DEPLOY_DIR)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${RISA_DEPLOY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${RISA_DEPLOY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${RISA_DEPLOY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE        "${RISA_DEPLOY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${RISA_DEPLOY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG          "${RISA_DEPLOY_DIR}"
    )

    # Remove the old non-prefixed filename so SKSE cannot load two copies.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f "${RISA_DEPLOY_DIR}/RisaAllInOneMenu.dll"
    )
endif()

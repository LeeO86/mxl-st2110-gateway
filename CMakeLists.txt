cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(mxl-st2110-gateway
    VERSION 0.0.0
    DESCRIPTION "SMPTE ST 2110 <-> MXL gateway (Intel MTL/DPDK, NMOS IS-04/IS-05, BCP-007-03)"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(MXLGW_BUILD_TESTS "Build unit tests" ON)
option(MXLGW_BUILD_TOOLS "Build mxl-pattern-writer and mxl-verify (needs MXL)" ON)
option(MXLGW_BUILD_FUZZERS "Build libFuzzer targets (clang)" OFF)
option(MXLGW_WERROR "Treat warnings as errors" OFF)
option(MXLGW_COVERAGE "Instrument for coverage (gcov)" OFF)
option(MXLGW_USE_PREBUILT_WEBUI "Embed an existing web/dist/index.html instead of running npm" OFF)
set(MXLGW_WITH_MXL "AUTO" CACHE STRING "Build the MXL bridge (ON/OFF/AUTO)")
set(MXLGW_WITH_MTL "AUTO" CACHE STRING "Build the MTL backend (ON/OFF/AUTO)")
set(MXLGW_WITH_NMOS "AUTO" CACHE STRING "Build the nmos-cpp node and HTTP server (ON/OFF/AUTO)")
set(MXLGW_VERSION "" CACHE STRING "Version string (default: git describe, else 0.0.0-dev+<sha>)")

find_package(Threads REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(OpenSSL REQUIRED)

include(MxlgwVersion)
include(MxlgwDependencies)

set(MXLGW_WARNINGS -Wall -Wextra -Wnon-virtual-dtor)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND MXLGW_WARNINGS -Wshadow=local)
else()
    list(APPEND MXLGW_WARNINGS -Wshadow-uncaptured-local)
endif()
if(MXLGW_WERROR)
    list(APPEND MXLGW_WARNINGS -Werror)
endif()
if(MXLGW_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

configure_file(src/version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp @ONLY)

# ------------------------------------------------------------------
# Embedded assets: JSON schema and web UI
# ------------------------------------------------------------------
include(EmbedFile)
mxlgw_embed_file(${CMAKE_CURRENT_SOURCE_DIR}/schema/gateway-config.schema.json
    ${CMAKE_CURRENT_BINARY_DIR}/generated/config/schema_embedded.hpp configSchemaJson)
mxlgw_embed_file(${CMAKE_CURRENT_SOURCE_DIR}/config/examples/gateway.minimal.json
    ${CMAKE_CURRENT_BINARY_DIR}/generated/config/minimal_embedded.hpp minimalConfigJson)
include(WebUi)

# ------------------------------------------------------------------
# Core library: pure C++20, no MTL / nmos-cpp
# ------------------------------------------------------------------
add_library(mxlgw-core STATIC
    ${CMAKE_CURRENT_BINARY_DIR}/generated/config/schema_embedded.hpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/config/minimal_embedded.hpp
    src/util/uuid.cpp
    src/util/logging.cpp
    src/util/fs.cpp
    src/util/strings.cpp
    src/util/cpuset.cpp
    src/util/net.cpp
    src/util/threading.cpp
    src/util/backoff.cpp
    src/config/schema.cpp
    src/config/formats.cpp
    src/config/config.cpp
    src/config/env.cpp
    src/config/store.cpp
    src/nmos/ids.cpp
    src/timing/rtpclock.cpp
    src/timing/ptp.cpp
    src/codec/audioconv.cpp
    src/codec/anc8331.cpp
    src/codec/v210.cpp
    src/codec/testpattern.cpp
    src/mtl/sdp_map.cpp
    src/mtl/backend.cpp
    src/mtl/mock_backend.cpp
    src/mxlbridge/domainscan.cpp
    src/mxlbridge/domaindef.cpp
    src/mxlbridge/flowdef.cpp
    src/mxlbridge/bootstrap.cpp
    src/group/essence_state.cpp
    src/group/replacement.cpp
    src/ops/http.cpp
    src/ops/metrics.cpp
    src/ops/health.cpp
    src/ops/preflight.cpp
    src/ops/statusz.cpp
)
target_include_directories(mxlgw-core PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(mxlgw-core PUBLIC nlohmann_json::nlohmann_json OpenSSL::Crypto Threads::Threads)
target_compile_options(mxlgw-core PRIVATE ${MXLGW_WARNINGS})

# ------------------------------------------------------------------
# MXL bridge (writers/readers, domain bootstrap, sync groups)
# ------------------------------------------------------------------
if(MXLGW_HAVE_MXL)
    add_library(mxlgw-mxl STATIC
        src/mxlbridge/instance.cpp
        src/mxlbridge/writer.cpp
        src/mxlbridge/reader.cpp
        src/mxlbridge/flowsync.cpp
        src/mxlbridge/pattern.cpp
    )
    target_link_libraries(mxlgw-mxl PUBLIC mxlgw-core mxl::mxl)
    target_compile_options(mxlgw-mxl PRIVATE ${MXLGW_WARNINGS})
    target_compile_definitions(mxlgw-mxl PUBLIC MXLGW_HAVE_MXL=1)
endif()

# ------------------------------------------------------------------
# MTL backend (DPDK and kernel-socket PMDs)
# ------------------------------------------------------------------
if(MXLGW_HAVE_MTL)
    add_library(mxlgw-mtl STATIC
        src/mtl/instance.cpp
        src/mtl/video_rx.cpp
        src/mtl/video_tx.cpp
        src/mtl/audio_rx.cpp
        src/mtl/audio_tx.cpp
        src/mtl/anc_rx.cpp
        src/mtl/anc_tx.cpp
        src/mtl/mtl_backend.cpp
        src/mtl/ethdev.cpp
    )
    target_link_libraries(mxlgw-mtl PUBLIC mxlgw-core PkgConfig::MTL)
    target_compile_options(mxlgw-mtl PRIVATE ${MXLGW_WARNINGS})
    target_compile_definitions(mxlgw-mtl PUBLIC MXLGW_HAVE_MTL=1)
endif()

# ------------------------------------------------------------------
# Groups / essences / pipeline orchestration (needs the MXL bridge)
# ------------------------------------------------------------------
if(MXLGW_HAVE_MXL)
    add_library(mxlgw-pipeline STATIC
        src/group/ingest_essence.cpp
        src/group/egress_group.cpp
        src/group/domain_resolver.cpp
        src/group/group_manager.cpp
    )
    target_link_libraries(mxlgw-pipeline PUBLIC mxlgw-mxl)
    if(MXLGW_HAVE_MTL)
        target_link_libraries(mxlgw-pipeline PUBLIC mxlgw-mtl)
    endif()
    target_compile_options(mxlgw-pipeline PRIVATE ${MXLGW_WARNINGS})
endif()

# ------------------------------------------------------------------
# nmos-cpp node + cpprest HTTP adapter (C++17: websocketpp is not C++20 clean)
# ------------------------------------------------------------------
if(MXLGW_HAVE_NMOS)
    add_library(mxlgw-nmos STATIC
        ${WEBUI_GENERATED}
        src/nmos/node.cpp
        src/nmos/resources.cpp
        src/nmos/sdp.cpp
        src/nmos/connection_state.cpp
        src/ops/http_cpprest.cpp
    )
    set_target_properties(mxlgw-nmos PROPERTIES CXX_STANDARD 17)
    target_include_directories(mxlgw-nmos PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(mxlgw-nmos PUBLIC mxlgw-core nmos-cpp::nmos-cpp)
    target_compile_definitions(mxlgw-nmos PUBLIC MXLGW_HAVE_NMOS=1)
    # nmos-cpp headers are noisy; keep -Werror for our own code via separate flags.
    target_compile_options(mxlgw-nmos PRIVATE -Wall -Wno-deprecated-declarations)
endif()

# ------------------------------------------------------------------
# Executable
# ------------------------------------------------------------------
if(MXLGW_HAVE_MXL AND MXLGW_HAVE_NMOS)
    add_executable(mxl-st2110-gateway src/main.cpp)
    target_link_libraries(mxl-st2110-gateway PRIVATE mxlgw-pipeline mxlgw-nmos)
    target_compile_options(mxl-st2110-gateway PRIVATE ${MXLGW_WARNINGS})
    install(TARGETS mxl-st2110-gateway RUNTIME DESTINATION bin)
endif()

if(MXLGW_HAVE_MXL AND MXLGW_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(MXLGW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MXLGW_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()

message(STATUS "mxl-st2110-gateway ${MXLGW_VERSION_STRING}: MXL=${MXLGW_HAVE_MXL} MTL=${MXLGW_HAVE_MTL} NMOS=${MXLGW_HAVE_NMOS}")

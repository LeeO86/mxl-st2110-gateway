# Version source of truth: the git tag (SPECIFICATION.md §16.3).
# Tagged builds report X.Y.Z, untagged builds 0.0.0-dev+<sha>.
if(MXLGW_VERSION)
    set(MXLGW_VERSION_STRING "${MXLGW_VERSION}")
else()
    find_package(Git QUIET)
    set(_tag "")
    set(_sha "unknown")
    if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match --match "v[0-9]*"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE _tag OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE _sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(_tag MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+.*)$")
        set(MXLGW_VERSION_STRING "${CMAKE_MATCH_1}")
    else()
        set(MXLGW_VERSION_STRING "0.0.0-dev+${_sha}")
    endif()
endif()

# Pins live only in docker/Dockerfile and .github/workflows/ci.yaml (AGENTS.md); the image build passes
# them in. Local builds report "unknown" (the MXL version is also read at runtime via mxlGetVersion).
set(MXLGW_PIN_MTL "unknown" CACHE STRING "MTL pin reported in build info")
set(MXLGW_PIN_DPDK "unknown" CACHE STRING "DPDK pin reported in build info")
set(MXLGW_PIN_MXL "unknown" CACHE STRING "MXL pin reported in build info")
set(MXLGW_PIN_NMOS_CPP "unknown" CACHE STRING "nmos-cpp pin reported in build info")

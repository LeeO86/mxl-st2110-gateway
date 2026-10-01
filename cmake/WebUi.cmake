# Embedded admin web UI (§11): Vue SPA -> single-file dist/index.html -> header.
# The Docker build creates web/dist in a Node stage and passes MXLGW_USE_PREBUILT_WEBUI=ON;
# local builds run npm when available, otherwise a placeholder page is embedded.
set(WEBUI_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/ops/webui_embedded.hpp)

file(GLOB_RECURSE _webui_sources CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/*.vue
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/*.js
    ${CMAKE_CURRENT_SOURCE_DIR}/web/src/*.css)
list(APPEND _webui_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
    ${CMAKE_CURRENT_SOURCE_DIR}/web/package.json
    ${CMAKE_CURRENT_SOURCE_DIR}/web/vite.config.js)

if(MXLGW_USE_PREBUILT_WEBUI)
    set(WEBUI_HTML ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html)
    if(NOT EXISTS ${WEBUI_HTML})
        message(FATAL_ERROR "MXLGW_USE_PREBUILT_WEBUI=ON but ${WEBUI_HTML} is missing")
    endif()
else()
    find_program(NPM_EXECUTABLE npm)
    if(NPM_EXECUTABLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/web/package.json)
        set(WEBUI_HTML ${CMAKE_CURRENT_BINARY_DIR}/webui/index.html)
        add_custom_command(
            OUTPUT ${WEBUI_HTML}
            COMMAND ${NPM_EXECUTABLE} ci --no-fund --no-audit
            COMMAND ${NPM_EXECUTABLE} run build
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/webui
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html ${WEBUI_HTML}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web
            DEPENDS ${_webui_sources}
            COMMENT "Building Vue web UI (npm run build)"
            VERBATIM)
    else()
        set(WEBUI_HTML ${CMAKE_CURRENT_SOURCE_DIR}/web/placeholder.html)
        message(STATUS "npm not found: embedding the placeholder admin UI")
    endif()
endif()

mxlgw_embed_file(${WEBUI_HTML} ${WEBUI_GENERATED} webUiHtml)

add_executable(unit-tests
    unit/test_main.cpp
    unit/test_util.cpp
    unit/test_schema.cpp
    unit/test_config.cpp
    unit/test_env.cpp
    unit/test_store.cpp
    unit/test_ids.cpp
    unit/test_rtpclock.cpp
    unit/test_ptp.cpp
    unit/test_audioconv.cpp
    unit/test_anc8331.cpp
    unit/test_v210.cpp
    unit/test_sdp_map.cpp
    unit/test_domainscan.cpp
    unit/test_flowdef.cpp
    unit/test_ops.cpp
    unit/test_preflight.cpp
    unit/test_mock_backend.cpp
    unit/test_group.cpp
    unit/test_bootstrap.cpp
    unit/test_testpattern.cpp
)
target_include_directories(unit-tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(unit-tests PRIVATE mxlgw-core)
target_compile_definitions(unit-tests PRIVATE MXLGW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME unit-tests COMMAND unit-tests)

if(MXLGW_HAVE_MXL)
    add_executable(mxl-tests
        unit/test_main.cpp
        unit/test_mxl_bridge.cpp
        unit/test_pipeline.cpp
    )
    target_include_directories(mxl-tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(mxl-tests PRIVATE mxlgw-pipeline)
    target_compile_definitions(mxl-tests PRIVATE MXLGW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME mxl-tests COMMAND mxl-tests)
endif()

if(MXLGW_HAVE_MXL AND MXLGW_HAVE_NMOS)
    add_executable(app-tests
        unit/test_main.cpp
        unit/test_application.cpp
    )
    target_include_directories(app-tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(app-tests PRIVATE mxlgw-pipeline)
    target_compile_definitions(app-tests PRIVATE MXLGW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME app-tests COMMAND app-tests)
endif()

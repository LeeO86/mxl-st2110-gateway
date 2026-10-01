add_executable(mxl-pattern-writer mxl-pattern-writer/main.cpp)
target_link_libraries(mxl-pattern-writer PRIVATE mxlgw-mxl)
target_compile_options(mxl-pattern-writer PRIVATE ${MXLGW_WARNINGS})

add_executable(mxl-verify mxl-verify/main.cpp)
target_link_libraries(mxl-verify PRIVATE mxlgw-mxl)
target_compile_options(mxl-verify PRIVATE ${MXLGW_WARNINGS})

install(TARGETS mxl-pattern-writer mxl-verify RUNTIME DESTINATION bin)

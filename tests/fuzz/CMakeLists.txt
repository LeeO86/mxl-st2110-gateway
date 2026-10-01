# libFuzzer targets (§18): configure with clang and -DMXLGW_BUILD_FUZZERS=ON, run e.g.
#   ./tests/fuzz/fuzz-sdp -max_total_time=60 <corpus-dir> ../tests/fixtures/sdp
add_executable(fuzz-anc8331 fuzz_anc8331.cpp)
target_link_libraries(fuzz-anc8331 PRIVATE mxlgw-core)
target_link_options(fuzz-anc8331 PRIVATE -fsanitize=fuzzer)

if(MXLGW_HAVE_NMOS)
    add_executable(fuzz-sdp fuzz_sdp.cpp)
    target_link_libraries(fuzz-sdp PRIVATE mxlgw-nmos)
    target_link_options(fuzz-sdp PRIVATE -fsanitize=fuzzer)
else()
    message(STATUS "fuzz-sdp needs nmos-cpp: skipped")
endif()

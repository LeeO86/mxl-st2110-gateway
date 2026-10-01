# mxlgw_embed_file(<input> <output-header> <variable>)
# Generates a header defining `inline constexpr char <variable>[]` with the file's bytes.
function(mxlgw_embed_file input output variable)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${input} -DOUTPUT=${output} -DVAR=${variable}
            -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFileScript.cmake
        DEPENDS ${input} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFileScript.cmake
        COMMENT "Embedding ${input}"
        VERBATIM)
endfunction()

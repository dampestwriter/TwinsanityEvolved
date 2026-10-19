# Test fixture paths are explicit command arguments. Leave both unset for the
# ordinary regression suite; use a fresh export root for an artifact run.
set(PS2R_MOVIE_ORACLE_EXPORT_ROOT "" CACHE PATH "Fresh root for Movie oracle artifacts")
set(PS2R_MOVIE_ORACLE_SOURCE_MANIFEST "" CACHE FILEPATH "Candidate source manifest for Movie oracles")

if((PS2R_MOVIE_ORACLE_EXPORT_ROOT STREQUAL "") AND
   NOT (PS2R_MOVIE_ORACLE_SOURCE_MANIFEST STREQUAL ""))
    message(FATAL_ERROR "Movie oracle export requires both EXPORT_ROOT and SOURCE_MANIFEST")
endif()
if(NOT (PS2R_MOVIE_ORACLE_EXPORT_ROOT STREQUAL "") AND
   (PS2R_MOVIE_ORACLE_SOURCE_MANIFEST STREQUAL ""))
    message(FATAL_ERROR "Movie oracle export requires both EXPORT_ROOT and SOURCE_MANIFEST")
endif()

function(movie_oracle_export_args output suite)
    set(arguments)
    if(NOT PS2R_MOVIE_ORACLE_EXPORT_ROOT STREQUAL "")
        get_filename_component(export_root "${PS2R_MOVIE_ORACLE_EXPORT_ROOT}" ABSOLUTE
                               BASE_DIR "${CMAKE_BINARY_DIR}")
        get_filename_component(source_manifest "${PS2R_MOVIE_ORACLE_SOURCE_MANIFEST}" ABSOLUTE
                               BASE_DIR "${CMAKE_BINARY_DIR}")
        list(APPEND arguments --export-dir "${export_root}/${suite}"
                              --source-manifest "${source_manifest}")
    endif()
    set(${output} "${arguments}" PARENT_SCOPE)
endfunction()

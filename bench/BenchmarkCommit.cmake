# Keep the generated header idle on a clean build while following HEAD changes.
function(azookey_add_benchmark_commit_header)
  set(generator "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GenerateBenchmarkCommit.cmake")
  set(dependencies "${generator}")
  if(NOT AZOOKEY_BENCH_GIT_COMMIT)
    set(git_paths HEAD packed-refs)
    execute_process(COMMAND git symbolic-ref -q HEAD
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      RESULT_VARIABLE symbolic_result OUTPUT_VARIABLE symbolic_ref
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(symbolic_result EQUAL 0)
      list(APPEND git_paths "${symbolic_ref}")
    endif()
    foreach(git_path IN LISTS git_paths)
      execute_process(COMMAND git rev-parse --git-path "${git_path}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE path_result OUTPUT_VARIABLE metadata
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      if(path_result EQUAL 0)
        get_filename_component(metadata "${metadata}" ABSOLUTE
          BASE_DIR "${CMAKE_SOURCE_DIR}")
        if(git_path STREQUAL "packed-refs" AND NOT EXISTS "${metadata}")
          # Packing an existing loose ref removes a tracked configure input;
          # that already forces rediscovery without watching unrelated Git files.
          continue()
        endif()
        if(EXISTS "${metadata}")
          list(APPEND dependencies "${metadata}")
        else()
          # A packed branch may gain a loose ref on the next commit. Watch its
          # nearest existing parent until that ref exists, without a glob check
          # that would make every Ninja dry run appear to have pending work.
          while(NOT EXISTS "${metadata}")
            get_filename_component(metadata "${metadata}" DIRECTORY)
          endwhile()
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${metadata}")
      endif()
    endforeach()
  endif()
  add_custom_command(OUTPUT "${AZOOKEY_BENCH_COMMIT_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DOUTPUT_FILE=${AZOOKEY_BENCH_COMMIT_HEADER}"
            "-DAZOOKEY_BENCH_GIT_COMMIT_OVERRIDE=${AZOOKEY_BENCH_GIT_COMMIT}"
            -P "${generator}"
    DEPENDS ${dependencies}
    # scripts/make-vm-verify-package.ps1 recognizes this legacy description.
    COMMENT "Refreshing benchmark commit header"
    VERBATIM)
  add_custom_target(azookey_benchmark_commit_header
    DEPENDS "${AZOOKEY_BENCH_COMMIT_HEADER}")
endfunction()

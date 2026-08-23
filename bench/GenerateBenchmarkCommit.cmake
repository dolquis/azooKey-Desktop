if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_FILE)
  message(FATAL_ERROR "SOURCE_DIR and OUTPUT_FILE are required")
endif()

set(benchmark_commit "${AZOOKEY_BENCH_GIT_COMMIT_OVERRIDE}")
if(NOT benchmark_commit)
  execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE git_result
    OUTPUT_VARIABLE benchmark_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(NOT git_result EQUAL 0 OR NOT benchmark_commit)
    set(benchmark_commit "unknown")
  endif()
endif()

set(header_content "#pragma once\n#define AZOOKEY_BENCH_COMMIT \"${benchmark_commit}\"\n")
set(existing_content "")
if(EXISTS "${OUTPUT_FILE}")
  file(READ "${OUTPUT_FILE}" existing_content)
endif()
if(NOT existing_content STREQUAL header_content)
  get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
  file(MAKE_DIRECTORY "${output_directory}")
  file(WRITE "${OUTPUT_FILE}" "${header_content}")
endif()

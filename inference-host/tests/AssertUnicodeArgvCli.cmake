# Drives the host binary through the platform command line with non-ASCII
# arguments. CMake keeps its strings as UTF-8 and hands them to CreateProcessW
# on Windows, so this exercises the wide argv boundary that unit tests linking
# the CLI libraries directly cannot reach.
foreach(required_variable HOST_EXE WORK_DIR)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

set(data_dir "${WORK_DIR}/日本語")
set(learning_path "${data_dir}/学習.tsv")
set(user_dict_path "${data_dir}/辞書.json")
set(export_path "${data_dir}/書出.json")
set(reading "にほんご")
set(surface "日本語")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${data_dir}")

function(run_host description)
  execute_process(
    COMMAND "${HOST_EXE}" --learning "${learning_path}" --user-dict "${user_dict_path}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout_text
    ERROR_VARIABLE stderr_text
    # Without this, Windows CMake decodes the captured output with the console
    # code page and the UTF-8 the host wrote would not match the expectations.
    ENCODING UTF-8
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${description} failed (${result}): ${stderr_text}${stdout_text}")
  endif()
  set(host_stdout "${stdout_text}" PARENT_SCOPE)
endfunction()

run_host("userdict add" userdict add --offline --reading "${reading}" --surface "${surface}")
if(NOT EXISTS "${user_dict_path}")
  message(FATAL_ERROR "user dictionary was not written to the requested UTF-8 path")
endif()

run_host("lookup exact" lookup --mode exact --query "${reading}" --format tsv)
if(NOT host_stdout MATCHES "user_dict\t${reading}\t${surface}")
  message(FATAL_ERROR "exact lookup did not match the registered entry: ${host_stdout}")
endif()

run_host("lookup prefix" lookup --mode prefix --query "にほ" --format tsv)
if(NOT host_stdout MATCHES "user_dict\t${reading}\t${surface}")
  message(FATAL_ERROR "prefix lookup did not match the registered entry: ${host_stdout}")
endif()

run_host("lookup surface" lookup --mode surface --query "${surface}" --format json)
if(NOT host_stdout MATCHES "\"surface\":\"${surface}\"")
  message(FATAL_ERROR "surface lookup did not match the registered entry: ${host_stdout}")
endif()

run_host("userdict export" userdict export "${export_path}")
if(NOT EXISTS "${export_path}")
  message(FATAL_ERROR "export was not written to the requested UTF-8 path")
endif()
file(READ "${export_path}" exported)
if(NOT exported MATCHES "${surface}")
  message(FATAL_ERROR "export lost the registered surface: ${exported}")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")

# The personal regression bench (conversion-quality-benchmark-spec section 15) as a process: on a
# synthetic plaintext store in WORK_DIR only, the output, stdout and stderr hold no reading or
# surface, the store and its directory are left as they were, rejected options and settings exit
# 2, and the compatibility keys are recorded. Never point this at a real %LOCALAPPDATA% store.
foreach(required_variable BENCH_EXE WORK_DIR)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
set(STORE_DIR "${WORK_DIR}/store")
set(LEARNING "${STORE_DIR}/learning.tsv")
set(LEARNING_V2 "${STORE_DIR}/learning.v2.tsv")
set(CONFIG_DIR "${WORK_DIR}/config")
set(SETTINGS "${CONFIG_DIR}/settings.json")
set(OUT_DIR "${WORK_DIR}/out")
set(OUTPUT "${OUT_DIR}/result.json")
set(PER_CASE "${OUT_DIR}/cases.jsonl")
file(MAKE_DIRECTORY "${STORE_DIR}" "${CONFIG_DIR}" "${OUT_DIR}")

string(ASCII 9 TAB)
set(ROWS "# azookey-learning-tsv escaped=1 version=2\n")
function(add_row reading surface commits rejects app)
  set(ROWS "${ROWS}${reading}${TAB}${surface}${TAB}0.8${TAB}1999999000${TAB}${commits}${TAB}0${TAB}${rejects}${TAB}${app}${TAB}commit${TAB}\n" PARENT_SCOPE)
endfunction()
# The case: a surface summed over two app rows, another net-committed one, a net-rejected one.
add_row("ぷらいべーと" "秘匿表層" 3 0 "")
add_row("ぷらいべーと" "秘匿表層" 1 0 "notepad.exe")
add_row("ぷらいべーと" "秘匿別表層" 1 0 "")
add_row("ぷらいべーと" "拒否表層" 0 2 "")
# Left out: a bundled secure app, the --max-cases limit and a malformed line.
add_row("せきゅあ" "金庫表層" 9 0 "keepass.exe")
add_row("あふれ" "溢れ表層" 1 0 "")
add_row("zqxreading" "zqxsecret" 1 0 "")
set(ROWS "${ROWS}こわれた${TAB}壊れ表層${TAB}bad\n")
# file(WRITE) turns LF into CRLF on Windows; the store is written with LF as the Host writes it.
file(CONFIGURE OUTPUT "${LEARNING_V2}" CONTENT "${ROWS}" @ONLY NEWLINE_STYLE LF)
set(MARKERS ぷらいべーと 秘匿表層 秘匿別表層 拒否表層 せきゅあ 金庫表層 あふれ 溢れ表層 zqxreading
            zqxsecret こわれた 壊れ表層 田中 太郎 日本語の文章)
file(WRITE "${SETTINGS}" "{\"privacy\":{\"mode\":\"normal\"}}")

function(snapshot directory out_var)
  file(GLOB names LIST_DIRECTORIES true RELATIVE "${directory}" "${directory}/*")
  list(SORT names)
  set(state "${names}")
  foreach(name IN LISTS names)
    if(NOT IS_DIRECTORY "${directory}/${name}")
      file(READ "${directory}/${name}" bytes HEX)
      file(TIMESTAMP "${directory}/${name}" stamp "%Y-%m-%dT%H:%M:%S" UTC)
      string(APPEND state "|${name}=${bytes}@${stamp}")
    endif()
  endforeach()
  set(${out_var} "${state}" PARENT_SCOPE)
endfunction()

function(expect_no_markers label text)
  foreach(marker IN LISTS MARKERS)
    string(FIND "${text}" "${marker}" at)
    if(NOT at EQUAL -1)
      message(FATAL_ERROR "${label} holds a personal string")
    endif()
  endforeach()
endfunction()

set(BASE --personal-eval --learning "${LEARNING}" --settings "${SETTINGS}" --iterations 1)
function(run_bench name)
  execute_process(
    COMMAND "${BENCH_EXE}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  expect_no_markers("${name} stdout and stderr" "${stdout}${stderr}")
  if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" report)
    expect_no_markers("${name} --output" "${report}")
  endif()
  set(${name}_result "${result}" PARENT_SCOPE)
  set(${name}_stderr "${stderr}" PARENT_SCOPE)
endfunction()

function(expect_exit name code)
  if(NOT "${${name}_result}" STREQUAL "${code}")
    message(FATAL_ERROR "${name}: expected exit ${code}, got ${${name}_result}: ${${name}_stderr}")
  endif()
endfunction()

function(expect_stderr name text)
  string(FIND "${${name}_stderr}" "${text}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${name}: stderr lacks \"${text}\": ${${name}_stderr}")
  endif()
endfunction()

function(expect_json key expected)
  file(READ "${OUTPUT}" report)
  string(JSON value GET "${report}" ${ARGN})
  if(NOT "${value}" STREQUAL "${expected}")
    message(FATAL_ERROR "${key}: expected ${expected}, got ${value}")
  endif()
endfunction()

snapshot("${STORE_DIR}" store_before)

# Spec sections 15.3 to 15.7: aggregates only, the warning on every run, the exclusions.
run_bench(first ${BASE} --output "${OUTPUT}" --max-cases 1)
expect_exit(first 0)
expect_stderr(first "do not share this result file")
expect_json(eval_source personal config eval_source)
expect_json(profile_sha256 none config profile_sha256)
expect_json(nll off config nll)
expect_json(category_filter all config category_filter)
expect_json(n_cases 1 summary n_cases)
expect_json(n_rejected_cases 1 summary n_rejected_cases)
expect_json(n_rows_read 7 summary n_rows_read)
expect_json(malformed_row 1 summary skipped malformed_row)
expect_json(secure_app 1 summary skipped secure_app)
expect_json(over_max_cases 2 summary skipped over_max_cases)
file(READ "${OUTPUT}" first_report)
string(JSON first_sha GET "${first_report}" config eval_dataset_sha256)
string(JSON first_top1 GET "${first_report}" summary top1_accuracy)
string(JSON first_coverage GET "${first_report}" summary commit_coverage_top1)
file(RENAME "${OUTPUT}" "${OUT_DIR}/baseline.json")

# The same store and configuration give the same hash and accuracy, and compare with each other.
run_bench(second ${BASE} --output "${OUTPUT}" --max-cases 1 --baseline "${OUT_DIR}/baseline.json")
expect_exit(second 0)
file(READ "${OUTPUT}" second_report)
string(JSON second_sha GET "${second_report}" config eval_dataset_sha256)
string(JSON second_top1 GET "${second_report}" summary top1_accuracy)
string(JSON second_coverage GET "${second_report}" summary commit_coverage_top1)
string(JSON diff_top1 GET "${second_report}" diff_vs_baseline top1_accuracy)
if(NOT first_sha STREQUAL second_sha OR NOT first_top1 STREQUAL second_top1 OR
   NOT first_coverage STREQUAL second_coverage OR NOT diff_top1 EQUAL 0)
  message(FATAL_ERROR "repeated runs differ: ${first_report} ${second_report}")
endif()
# Another case limit is another dataset: no diff, only the key names.
run_bench(other_limit ${BASE} --output "${OUTPUT}" --baseline "${OUT_DIR}/baseline.json")
expect_exit(other_limit 0)
expect_stderr(other_limit "eval_dataset_sha256")
file(READ "${OUTPUT}" other_report)
string(JSON other_diff_length LENGTH "${other_report}" diff_vs_baseline)
if(NOT other_diff_length EQUAL 0)
  message(FATAL_ERROR "an incompatible baseline produced a diff: ${other_report}")
endif()

# --per-case needs --include-personal-text, and then warns.
file(REMOVE "${OUTPUT}")
run_bench(per_case_denied ${BASE} --output "${OUTPUT}" --per-case "${PER_CASE}")
expect_exit(per_case_denied 2)
if(EXISTS "${PER_CASE}" OR EXISTS "${OUTPUT}")
  message(FATAL_ERROR "a refused --per-case still wrote a file")
endif()
execute_process(
  COMMAND "${BENCH_EXE}" ${BASE} --output "${OUTPUT}" --per-case "${PER_CASE}"
          --include-personal-text
  RESULT_VARIABLE per_case_result
  ERROR_VARIABLE per_case_stderr
)
if(NOT per_case_result EQUAL 0)
  message(FATAL_ERROR "--per-case --include-personal-text failed: ${per_case_stderr}")
endif()
string(FIND "${per_case_stderr}" "writes the readings and surfaces" warned)
file(READ "${PER_CASE}" per_case)
string(FIND "${per_case}" "\"id\":\"personal_000001\"" first_id)
if(warned EQUAL -1 OR first_id EQUAL -1)
  message(FATAL_ERROR "per-case warning or id missing: ${per_case_stderr}")
endif()
file(REMOVE "${PER_CASE}")

# Options a personal run does not take (spec section 15.5).
file(WRITE "${WORK_DIR}/bench/data/eval.jsonl" "")
foreach(rejected "--eval;${WORK_DIR}/bench/data/eval.jsonl" "--trace"
                 "--trace-output;${OUT_DIR}/trace.jsonl" "--decode-stats;${OUT_DIR}/stats.jsonl"
                 "--category;general" "--typo-mode;off" "--json" "--ipc" "--max-p95-ms;5")
  run_bench(rejected ${BASE} --output "${OUTPUT}" ${rejected})
  expect_exit(rejected 2)
endforeach()
# Unknown tokens are reported by position, and a profile never reaches stderr.
run_bench(unknown ${BASE} --output "${OUTPUT}" 田中太郎)
expect_exit(unknown 2)
expect_stderr(unknown "unknown option at argument")
run_bench(split_profile ${BASE} --output "${OUTPUT}" --profile 田中 太郎)
expect_exit(split_profile 2)
run_bench(profile_without_model ${BASE} --output "${OUTPUT}" --profile 日本語の文章)
expect_exit(profile_without_model 2)
run_bench(learning_without_personal --learning "${LEARNING}")
expect_exit(learning_without_personal 2)

# Spec section 15.2: a missing store is an input error, an empty store gives zero cases.
run_bench(missing_store --personal-eval --learning "${WORK_DIR}/absent/learning.tsv"
          --settings "${SETTINGS}" --output "${OUTPUT}")
expect_exit(missing_store 2)
file(MAKE_DIRECTORY "${WORK_DIR}/empty")
file(CONFIGURE OUTPUT "${WORK_DIR}/empty/learning.v2.tsv"
     CONTENT "# azookey-learning-tsv escaped=1 version=2\n" @ONLY NEWLINE_STYLE LF)
run_bench(empty_store --personal-eval --learning "${WORK_DIR}/empty/learning.tsv"
          --settings "${SETTINGS}" --output "${OUTPUT}" --iterations 1)
expect_exit(empty_store 0)
expect_json(empty_n_cases 0 summary n_cases)
expect_json(empty_top1 0 summary top1_accuracy)
# Results never overwrite the store.
run_bench(output_on_store ${BASE} --output "${LEARNING_V2}")
expect_exit(output_on_store 2)

# Spec section 15.4: settings that stop learning stop the run; an invalid file is not moved.
foreach(settings "{\"privacy\":{\"mode\":\"secure\"}}" "{\"privacy\":{\"mode\":\"private\"}}"
                 "{\"privacy\":{\"mode\":\"custom\",\"custom\":{\"learning\":false}}}"
                 "{\"privacy\":")
  file(WRITE "${SETTINGS}" "${settings}")
  snapshot("${CONFIG_DIR}" config_before)
  run_bench(stopped ${BASE} --output "${OUTPUT}")
  expect_exit(stopped 2)
  snapshot("${CONFIG_DIR}" config_after)
  if(NOT config_before STREQUAL config_after)
    message(FATAL_ERROR "the settings directory changed: ${settings}")
  endif()
endforeach()
run_bench(missing_settings --personal-eval --learning "${LEARNING}"
          --settings "${CONFIG_DIR}/absent.json" --output "${OUTPUT}")
expect_exit(missing_settings 2)
file(WRITE "${SETTINGS}" "{\"privacy\":{\"mode\":\"normal\"}}")

# The bench-only profile: hashed, never written out. The no-llama build loads a bare GGUF header
# with the zenz-v3 format, which has [PROF]; a llama.cpp build cannot load it.
if(NOT WITH_LLAMA)
  set(MODEL "${WORK_DIR}/model/fixture.gguf")
  file(MAKE_DIRECTORY "${WORK_DIR}/model")
  # "GGUF" and version 3 as little-endian bytes; a CMake string cannot hold the NUL bytes.
  execute_process(
    COMMAND powershell -NoProfile -NonInteractive -Command
            "[IO.File]::WriteAllBytes('${MODEL}', [byte[]](71,71,85,70,3,0,0,0))"
    RESULT_VARIABLE model_result)
  if(NOT model_result EQUAL 0)
    message(FATAL_ERROR "cannot write the GGUF header fixture")
  endif()
  file(CONFIGURE OUTPUT "${WORK_DIR}/profile.txt" CONTENT "日本語の文章\n" @ONLY
       NEWLINE_STYLE CRLF)
  run_bench(profile ${BASE} --output "${OUTPUT}" --model "${MODEL}" --nll on
            --profile-file "${WORK_DIR}/profile.txt")
  expect_exit(profile 0)
  expect_stderr(profile "do not share this result file")
  expect_json(profile_nll on config nll)
  file(READ "${OUTPUT}" profile_report)
  string(JSON profile_sha GET "${profile_report}" config profile_sha256)
  string(LENGTH "${profile_sha}" profile_sha_length)
  if(NOT profile_sha MATCHES "^[0-9a-f]+$" OR NOT profile_sha_length EQUAL 64)
    message(FATAL_ERROR "expected a profile SHA-256, got ${profile_sha}")
  endif()
  # The same profile from the command line gives the same hash.
  run_bench(profile_text ${BASE} --output "${OUTPUT}" --model "${MODEL}" --profile 日本語の文章)
  expect_exit(profile_text 0)
  expect_json(profile_text_sha "${profile_sha}" config profile_sha256)
endif()

snapshot("${STORE_DIR}" store_after)
if(NOT store_before STREQUAL store_after)
  message(FATAL_ERROR "the learning store or its directory changed")
endif()

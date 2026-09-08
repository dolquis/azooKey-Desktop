file(MAKE_DIRECTORY "${OUTPUT_DIR}")
configure_file("${SOURCE_DIR}/LICENSE" "${OUTPUT_DIR}/MIT.txt" COPYONLY)
execute_process(
  COMMAND "${PYTHON}" "${SOURCE_DIR}/dictbuild/dictbuild.py"
          "${SOURCE_DIR}/bench/data/nll_fixture.lex.tsv"
          --metadata "${SOURCE_DIR}/bench/data/nll_fixture.metadata.json"
          --layer technical_terms_lexicon --catalog "${OUTPUT_DIR}"
          --output "${OUTPUT_DIR}/fixture.azdic" --notices "${OUTPUT_DIR}/notices.txt"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "NLL fixture dictbuild failed (${result}): ${output}${error}")
endif()

foreach(required IN ITEMS RAG_BENCH FIXTURE_DIR TEST_ROOT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "missing required variable: ${required}")
    endif()
endforeach()

# Corpus::build writes its incremental cache beside the documents. Exercise the
# real directory seam in a build-local copy so the source fixture stays clean.
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${FIXTURE_DIR}/docs" DESTINATION "${TEST_ROOT}")
file(COPY "${FIXTURE_DIR}/judgments.json" DESTINATION "${TEST_ROOT}")

execute_process(
    COMMAND
        "${RAG_BENCH}"
        "${TEST_ROOT}/docs"
        --judgments
        "${TEST_ROOT}/judgments.json"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR
        "rag_bench --judgments exited with ${result}\n"
        "stdout:\n${stdout}\n"
        "stderr:\n${stderr}"
    )
endif()

set(expected "[judgments] cases=1 hit@1=1.000")
string(FIND "${stdout}" "${expected}" match_position)
if(match_position EQUAL -1)
    message(FATAL_ERROR
        "expected output to contain: ${expected}\n"
        "actual stdout:\n${stdout}"
    )
endif()

set(expected_local
    "[judgments] stage=local-rerank candidate_k=4 cases=1 hit@1=1.000")
string(FIND "${stdout}" "${expected_local}" local_match_position)
if(local_match_position EQUAL -1)
    message(FATAL_ERROR
        "expected output to contain: ${expected_local}\n"
        "actual stdout:\n${stdout}"
    )
endif()

foreach(required IN ITEMS RAG_BENCH FIXTURE_DIR TEST_ROOT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "missing required variable: ${required}")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${FIXTURE_DIR}/docs" DESTINATION "${TEST_ROOT}")
file(COPY "${FIXTURE_DIR}/judgments.json" DESTINATION "${TEST_ROOT}")

execute_process(
    COMMAND "${RAG_BENCH}" "${TEST_ROOT}/docs"
        --judgments "${TEST_ROOT}/judgments.json" 1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "rag_bench candidate_k=1 exited with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()

string(REGEX MATCHALL "\\[case\\] [^\r\n]+" reports "${stdout}")
foreach(line IN LISTS reports)
    string(REGEX REPLACE "^\\[case\\] " "" report "${line}")
    string(JSON query GET "${report}" q)
    if(query STREQUAL "deployment")
        string(JSON rank GET "${report}" retrieval passage_rank)
        string(JSON failure_class GET "${report}" failure_class)
        if(NOT rank EQUAL 0 OR NOT failure_class STREQUAL "candidate_miss")
            message(FATAL_ERROR
                "candidate_k=1 must exclude deployment gold passage: ${report}")
        endif()
        set(found_deployment TRUE)
    endif()
endforeach()

if(NOT found_deployment)
    message(FATAL_ERROR "missing deployment case report:\n${stdout}")
endif()

set(expected_summary
    "[judgments] stage=local-rerank candidate_k=1 cases=4")
string(FIND "${stdout}" "${expected_summary}" summary_position)
if(summary_position EQUAL -1)
    message(FATAL_ERROR
        "missing candidate_k=1 summary: ${expected_summary}\n${stdout}")
endif()

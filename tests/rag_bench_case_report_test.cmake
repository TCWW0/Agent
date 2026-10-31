cmake_minimum_required(VERSION 4.4)

foreach(required IN ITEMS RAG_BENCH FIXTURE_DIR TEST_ROOT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "missing required variable: ${required}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${FIXTURE_DIR}/docs" DESTINATION "${TEST_ROOT}")
file(COPY "${FIXTURE_DIR}/judgments.json" DESTINATION "${TEST_ROOT}")

execute_process(
    COMMAND "${RAG_BENCH}" "${TEST_ROOT}/docs"
        --judgments "${TEST_ROOT}/judgments.json"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(NOT "${result}" STREQUAL "0")
    message(FATAL_ERROR
        "rag_bench exited with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()

# Each [case] line is independently parseable JSON. Check behavior rather than
# JSON key order; ranks count passages, 0 means absent from the candidate pool.
string(REGEX MATCHALL "\\[case\\] [^\r\n]+" reports "${stdout}")
list(LENGTH reports report_count)
if(NOT report_count EQUAL 3)
    message(FATAL_ERROR "expected 3 [case] reports, got ${report_count}\n${stdout}")
endif()

set(seen_queries)
foreach(line IN LISTS reports)
    string(REGEX REPLACE "^\\[case\\] " "" report "${line}")
    string(JSON query GET "${report}" q)
    if(query IN_LIST seen_queries)
        message(FATAL_ERROR "duplicate case report: ${query}")
    endif()
    list(APPEND seen_queries "${query}")

    if(query STREQUAL "migrations")
        set(expected_rank 1)
        set(expected_paths procedure.md)
    elseif(query STREQUAL "deployment")
        set(expected_rank 2)
        set(expected_paths overview.md procedure.md)
    elseif(query STREQUAL "unfindabletokenxyz")
        set(expected_rank 0)
        set(expected_paths)
    else()
        message(FATAL_ERROR "unexpected query: ${query}")
    endif()

    string(JSON gold_count LENGTH "${report}" gold)
    string(JSON gold GET "${report}" gold 0)
    if(NOT gold_count EQUAL 1 OR NOT gold STREQUAL "procedure.md")
        message(FATAL_ERROR "wrong gold in report: ${report}")
    endif()

    foreach(stage IN ITEMS retrieval local_rerank)
        string(JSON rank GET "${report}" "${stage}" gold_rank)
        if(NOT rank EQUAL expected_rank)
            message(FATAL_ERROR
                "${query}: ${stage} expected rank ${expected_rank}, got ${rank}")
        endif()
        string(JSON hit_count LENGTH "${report}" "${stage}" hits)
        list(LENGTH expected_paths expected_count)
        if(NOT hit_count EQUAL expected_count)
            message(FATAL_ERROR
                "${query}: ${stage} expected ${expected_count} hits, got ${hit_count}")
        endif()
        set(index 0)
        foreach(expected_path IN LISTS expected_paths)
            string(JSON path GET "${report}" "${stage}" hits ${index} path)
            string(JSON start GET "${report}" "${stage}" hits ${index} line_start)
            string(JSON end GET "${report}" "${stage}" hits ${index} line_end)
            if(NOT path STREQUAL expected_path OR NOT start EQUAL 1 OR NOT end EQUAL 1)
                message(FATAL_ERROR
                    "${query}: ${stage}[${index}] expected ${expected_path}:1-1, got ${path}:${start}-${end}")
            endif()
            math(EXPR index "${index} + 1")
        endforeach()
    endforeach()
endforeach()

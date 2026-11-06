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
if(NOT report_count EQUAL 4)
    message(FATAL_ERROR "expected 4 [case] reports, got ${report_count}\n${stdout}")
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
        set(expected_gold_path procedure.md)
        set(expected_gold_start 1)
        set(expected_gold_end 1)
        set(expected_gold_rank 1)
        set(expected_passage_rank 1)
        set(expected_paths procedure.md)
        set(expected_hit_start 1)
        set(expected_hit_end 1)
    elseif(query STREQUAL "deployment")
        set(expected_gold_path procedure.md)
        set(expected_gold_start 1)
        set(expected_gold_end 1)
        set(expected_gold_rank 2)
        set(expected_passage_rank 2)
        set(expected_paths overview.md procedure.md)
        set(expected_hit_start 1)
        set(expected_hit_end 1)
    elseif(query STREQUAL "unfindabletokenxyz")
        set(expected_gold_path procedure.md)
        set(expected_gold_start 1)
        set(expected_gold_end 1)
        set(expected_gold_rank 0)
        set(expected_passage_rank 0)
        set(expected_paths)
    elseif(query STREQUAL "firstsectiontoken")
        set(expected_gold_path handbook.md)
        set(expected_gold_start 45)
        set(expected_gold_end 45)
        set(expected_gold_rank 1)
        set(expected_passage_rank 0)
        set(expected_paths handbook.md)
        set(expected_hit_start 1)
        set(expected_hit_end 40)
    else()
        message(FATAL_ERROR "unexpected query: ${query}")
    endif()

    string(JSON gold_count LENGTH "${report}" gold)
    if(NOT gold_count EQUAL 1)
        message(FATAL_ERROR "wrong gold count in report: ${report}")
    endif()

    string(JSON gold_path GET "${report}" gold 0 path)
    string(JSON gold_start GET "${report}" gold 0 line_start)
    string(JSON gold_end GET "${report}" gold 0 line_end)
    if(NOT gold_path STREQUAL expected_gold_path
        OR NOT gold_start EQUAL expected_gold_start
        OR NOT gold_end EQUAL expected_gold_end)
        message(FATAL_ERROR "wrong passage gold in report: ${report}")
    endif()

    foreach(stage IN ITEMS retrieval local_rerank)
        string(JSON rank GET "${report}" "${stage}" gold_rank)
        if(NOT rank EQUAL expected_gold_rank)
            message(FATAL_ERROR
                "${query}: ${stage} expected gold rank ${expected_gold_rank}, got ${rank}")
        endif()
        string(JSON passage_rank GET "${report}" "${stage}" passage_rank)
        if(NOT passage_rank EQUAL expected_passage_rank)
            message(FATAL_ERROR
                "${query}: ${stage} expected passage rank ${expected_passage_rank}, got ${passage_rank}")
        endif()
        string(JSON passage_hit_at_one GET
            "${report}" "${stage}" passage_hit_at_one)
        if(expected_passage_rank EQUAL 1)
            set(expected_passage_hit_at_one ON)
        else()
            set(expected_passage_hit_at_one OFF)
        endif()
        if(NOT passage_hit_at_one STREQUAL expected_passage_hit_at_one)
            message(FATAL_ERROR
                "${query}: ${stage} expected passage_hit_at_one=${expected_passage_hit_at_one}, got ${passage_hit_at_one}")
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
            if(NOT path STREQUAL expected_path
                OR NOT start EQUAL expected_hit_start
                OR NOT end EQUAL expected_hit_end)
                message(FATAL_ERROR
                    "${query}: ${stage}[${index}] expected ${expected_path}:${expected_hit_start}-${expected_hit_end}, got ${path}:${start}-${end}")
            endif()
            math(EXPR index "${index} + 1")
        endforeach()
    endforeach()

    if(query STREQUAL "migrations")
        string(FIND "${report}" "\"failure_class\"" failure_class_position)
        if(NOT failure_class_position EQUAL -1)
            message(FATAL_ERROR
                "${query}: passage Hit@1 must not have a failure_class: ${report}")
        endif()
    elseif(query STREQUAL "unfindabletokenxyz")
        string(JSON failure_class GET "${report}" failure_class)
        if(NOT failure_class STREQUAL "candidate_miss")
            message(FATAL_ERROR
                "${query}: expected candidate_miss, got ${failure_class}")
        endif()
    elseif(query STREQUAL "deployment")
        string(JSON failure_class GET "${report}" failure_class)
        if(NOT failure_class STREQUAL "ranking_miss")
            message(FATAL_ERROR
                "${query}: expected ranking_miss, got ${failure_class}")
        endif()
    elseif(query STREQUAL "firstsectiontoken")
        string(JSON failure_class GET "${report}" failure_class)
        if(NOT failure_class STREQUAL "incomplete_evidence")
            message(FATAL_ERROR
                "${query}: expected incomplete_evidence, got ${failure_class}")
        endif()
    endif()
endforeach()

set(expected_retrieval_summary
    "[judgments] cases=4 hit@1=0.500 passage_cases=4 passage_hit@1=0.250")
string(FIND "${stdout}" "${expected_retrieval_summary}" retrieval_summary_position)
if(retrieval_summary_position EQUAL -1)
    message(FATAL_ERROR
        "missing retrieval passage summary: ${expected_retrieval_summary}\n${stdout}")
endif()

set(expected_rerank_summary
    "[judgments] stage=local-rerank candidate_k=4 cases=4 hit@1=0.500 passage_cases=4 passage_hit@1=0.250")
string(FIND "${stdout}" "${expected_rerank_summary}" rerank_summary_position)
if(rerank_summary_position EQUAL -1)
    message(FATAL_ERROR
        "missing rerank passage summary: ${expected_rerank_summary}\n${stdout}")
endif()

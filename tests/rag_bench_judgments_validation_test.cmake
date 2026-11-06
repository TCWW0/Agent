foreach(required IN ITEMS RAG_BENCH FIXTURE_DIR TEST_ROOT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "missing required variable: ${required}")
    endif()
endforeach()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${FIXTURE_DIR}/docs" DESTINATION "${TEST_ROOT}")

set(case_files
    zero_start.json
    reversed_interval.json
    missing_line_end.json
    empty_path.json
)
set(expected_errors
    "case.gold 行区间必须满足 1 <= line_start <= line_end"
    "case.gold 行区间必须满足 1 <= line_start <= line_end"
    "case.gold 的每一项都必须是路径字符串，或包含 path、line_start、line_end 的对象"
    "case.gold 不能包含空路径"
)

list(LENGTH case_files case_count)
math(EXPR last_case "${case_count} - 1")
foreach(index RANGE 0 ${last_case})
    list(GET case_files ${index} case_file)
    list(GET expected_errors ${index} expected_error)

    execute_process(
        COMMAND "${RAG_BENCH}" "${TEST_ROOT}/docs"
            --judgments "${FIXTURE_DIR}/${case_file}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )

    if(NOT result EQUAL 2)
        message(FATAL_ERROR
            "${case_file}: expected exit 2, got ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()

    string(FIND "${stderr}" "${expected_error}" error_position)
    if(error_position EQUAL -1)
        message(FATAL_ERROR
            "${case_file}: expected error '${expected_error}'\nstderr:\n${stderr}")
    endif()

    string(FIND "${stdout}" "[case]" case_report_position)
    if(NOT case_report_position EQUAL -1)
        message(FATAL_ERROR
            "${case_file}: invalid judgments must not emit case reports:\n${stdout}")
    endif()
endforeach()

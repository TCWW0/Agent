if(NOT DEFINED PROJECT_SOURCE_DIR)
    message(FATAL_ERROR "PROJECT_SOURCE_DIR is required")
endif()

set(_roots
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
)
set(_extensions .h .hh .hpp .c .cc .cpp .cxx)
set(_violations)

foreach(_root IN LISTS _roots)
    file(GLOB_RECURSE _files
        LIST_DIRECTORIES false
        "${_root}/*")
    foreach(_file IN LISTS _files)
        get_filename_component(_extension "${_file}" EXT)
        if(NOT _extension IN_LIST _extensions)
            continue()
        endif()

        file(READ "${_file}" _contents)
        string(REGEX MATCH "#[ \t]*include[ \t]*[<\"]maya/app/"
               _direct_app_include "${_contents}")
        string(FIND "${_contents}" "#include <maya/maya.hpp>" _umbrella)
        string(FIND "${_contents}" "#include \"maya/maya.hpp\"" _quoted_umbrella)
        string(REGEX MATCH "maya::run[ \t]*\\(" _maya_run "${_contents}")

        if(_direct_app_include OR _umbrella GREATER_EQUAL 0
           OR _quoted_umbrella GREATER_EQUAL 0 OR _maya_run)
            set(_reason)
            if(_direct_app_include)
                set(_reason "direct maya/app include")
            elseif(_umbrella GREATER_EQUAL 0 OR _quoted_umbrella GREATER_EQUAL 0)
                set(_reason "maya umbrella include")
            else()
                set(_reason "maya::run call")
            endif()
            list(APPEND _violations "${_file}: ${_reason}")
        endif()
    endforeach()
endforeach()

if(_violations)
    string(JOIN "\n" _report ${_violations})
    message(FATAL_ERROR
        "Maya boundary violation(s) in maintained project sources:\n${_report}")
endif()

message(STATUS "Maya boundary scan passed")

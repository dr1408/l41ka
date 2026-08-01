if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT is required")
endif()

if(NOT DEFINED SYMBOL)
    message(FATAL_ERROR "SYMBOL is required")
endif()

if(NOT DEFINED XXD)
    set(XXD xxd)
endif()

execute_process(
    COMMAND "${XXD}" -i -n "${SYMBOL}" "${INPUT}"
    RESULT_VARIABLE xxd_result
    OUTPUT_VARIABLE xxd_output
    ERROR_VARIABLE xxd_error
)

if(NOT xxd_result STREQUAL "0")
    message(FATAL_ERROR "failed to generate ${OUTPUT} with ${XXD}: ${xxd_error}")
endif()

string(REPLACE
    "unsigned char ${SYMBOL}[]"
    "static const unsigned char ${SYMBOL}[]"
    xxd_output
    "${xxd_output}"
)
string(REPLACE
    "unsigned int ${SYMBOL}_len"
    "static const unsigned int ${SYMBOL}_len"
    xxd_output
    "${xxd_output}"
)

file(WRITE "${OUTPUT}" "#pragma once\n\n${xxd_output}")

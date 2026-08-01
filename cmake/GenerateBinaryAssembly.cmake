if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED OUTPUT_ASM)
    message(FATAL_ERROR "OUTPUT_ASM is required")
endif()

if(NOT DEFINED OUTPUT_HEADER)
    message(FATAL_ERROR "OUTPUT_HEADER is required")
endif()

if(NOT DEFINED SYMBOL)
    message(FATAL_ERROR "SYMBOL is required")
endif()

if(NOT DEFINED SECTION)
    set(SECTION .rodata)
endif()

file(SIZE "${INPUT}" input_size)
get_filename_component(output_asm_dir "${OUTPUT_ASM}" DIRECTORY)
get_filename_component(output_header_dir "${OUTPUT_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${output_asm_dir}" "${output_header_dir}")

string(REPLACE "\\" "\\\\" escaped_input "${INPUT}")
string(REPLACE "\"" "\\\"" escaped_input "${escaped_input}")

file(WRITE "${OUTPUT_ASM}" ".section ${SECTION}, \"a\", %progbits\n")
file(APPEND "${OUTPUT_ASM}" ".p2align 2\n")
file(APPEND "${OUTPUT_ASM}" ".globl ${SYMBOL}\n")
file(APPEND "${OUTPUT_ASM}" ".type ${SYMBOL}, %object\n")
file(APPEND "${OUTPUT_ASM}" "${SYMBOL}:\n")
file(APPEND "${OUTPUT_ASM}" ".incbin \"${escaped_input}\"\n")
file(APPEND "${OUTPUT_ASM}" ".globl ${SYMBOL}_end\n")
file(APPEND "${OUTPUT_ASM}" "${SYMBOL}_end:\n")
file(APPEND "${OUTPUT_ASM}" ".size ${SYMBOL}, . - ${SYMBOL}\n")

file(WRITE "${OUTPUT_HEADER}" "#pragma once\n\n")
file(APPEND "${OUTPUT_HEADER}" "#ifdef __cplusplus\nextern \"C\" {\n#endif\n")
file(APPEND "${OUTPUT_HEADER}" "extern const unsigned char ${SYMBOL}[];\n")
file(APPEND "${OUTPUT_HEADER}" "extern const unsigned char ${SYMBOL}_end[];\n")
file(APPEND "${OUTPUT_HEADER}" "#ifdef __cplusplus\n}\n#endif\n\n")
file(APPEND "${OUTPUT_HEADER}" "static const unsigned int ${SYMBOL}_len = ${input_size}u;\n")

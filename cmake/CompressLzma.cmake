if(NOT DEFINED INPUT)
    message(FATAL_ERROR "INPUT is required")
endif()

if(NOT DEFINED OUTPUT)
    message(FATAL_ERROR "OUTPUT is required")
endif()

if(NOT DEFINED XZ)
    set(XZ xz)
endif()

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")

execute_process(
    COMMAND "${XZ}" --format=lzma -9 --stdout "${INPUT}"
    OUTPUT_FILE "${OUTPUT}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    file(REMOVE "${OUTPUT}")
    message(FATAL_ERROR "xz failed with status ${result}")
endif()

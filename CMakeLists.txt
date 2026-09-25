# HERE BE DRAGONS BRO

cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/vendor/pico-sdk" CACHE PATH "Path to the Raspberry Pi Pico SDK")
endif()

if(NOT PICO_BOARD)
    if(DEFINED ENV{PICO_BOARD})
        set(PICO_BOARD "$ENV{PICO_BOARD}" CACHE STRING "Pico board" FORCE)
    else()
        set(PICO_BOARD "waveshare_rp2350_usb_a" CACHE STRING "Pico board" FORCE)
    endif()
endif()

set(L41KA_BOARD_CMAKE "${CMAKE_CURRENT_LIST_DIR}/boards/${PICO_BOARD}.cmake")
if(EXISTS "${L41KA_BOARD_CMAKE}")
    message(STATUS "Using l41ka board configuration from ${L41KA_BOARD_CMAKE}")
    include("${L41KA_BOARD_CMAKE}")
else()
    message(FATAL_ERROR "No l41ka board configuration for PICO_BOARD='${PICO_BOARD}' at ${L41KA_BOARD_CMAKE}")
endif()

if(NOT DEFINED PICO_TOOLCHAIN_PATH AND NOT DEFINED ENV{PICO_TOOLCHAIN_PATH})
    set(PICO_TOOLCHAIN_PATH "${CMAKE_CURRENT_LIST_DIR}/vendor/toolchain" CACHE PATH "Path to the Pico ARM toolchain")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/vendor/pico-sdk/external/pico_sdk_import.cmake)

project(l41ka VERSION 0.1.0 LANGUAGES C CXX ASM)

set(L41KA_VERSION_DEFINITIONS
    L41KA_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    L41KA_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    L41KA_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    L41KA_BOARD_NAME="${PICO_BOARD}"
)
set(L41KA_PAYLOAD_VERSION_FLAGS
    -DL41KA_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    -DL41KA_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    -DL41KA_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
add_compile_definitions(${L41KA_VERSION_DEFINITIONS})

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS 0)

pico_sdk_init()

set(USB_DIR ${CMAKE_CURRENT_LIST_DIR}/src/usb)
set(PIO_USB_DIR ${USB_DIR}/pio_usb/src)

set(L41KA_GENERATED_PAYLOAD_DIR ${CMAKE_CURRENT_LIST_DIR}/include/generated-payloads)
set(L41KA_PAYLOAD_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/payloads)
set(L41KA_PAYLOAD_CLANG clang CACHE STRING "Host clang used to build iOS arm64 payloads")
set(L41KA_PAYLOAD_VMACHO vmacho CACHE STRING "vmacho executable used to extract payload binaries")
set(L41KA_PAYLOAD_XXD xxd CACHE STRING "xxd executable used to generate payload headers")
set(L41KA_PONGOOS_MAKE make CACHE STRING "Make executable used to build PongoOS")
set(L41KA_RAMDISK_IMAGE "${CMAKE_CURRENT_LIST_DIR}/ramdisks/ramdisk.dmg" CACHE FILEPATH "Ramdisk image embedded in the Pi firmware")
set(L41KA_RAMDISK_XZ xz CACHE STRING "xz executable used to compress the embedded ramdisk")
set(L41KA_PONGOOS_EMBEDDED_CFLAGS "" CACHE STRING "Extra CFLAGS passed to the PongoOS build")
set(L41KA_PONGOOS_COMPAT_CFLAGS
    "-Wno-error=incompatible-sysroot -Wno-error=unused-but-set-variable -Wno-error=deprecated-non-prototype")

file(MAKE_DIRECTORY ${L41KA_GENERATED_PAYLOAD_DIR})

set(L41KA_GENERATED_PAYLOAD_HEADERS)
set(L41KA_GENERATED_PAYLOAD_SOURCES)

function(l41ka_add_shellcode_payload payload)
    set(payload_source_dir ${CMAKE_CURRENT_LIST_DIR}/src/payloads/shellcode/${payload})
    set(payload_build_dir ${L41KA_PAYLOAD_BUILD_DIR}/${payload})
    set(payload_symbol shellcode_${payload})
    set(payload_macho ${payload_build_dir}/${payload_symbol}.o)
    set(payload_bin ${payload_build_dir}/${payload_symbol}.bin)
    set(payload_header ${L41KA_GENERATED_PAYLOAD_DIR}/${payload_symbol}.h)
    set(payload_sources
        ${payload_source_dir}/start.S
        ${payload_source_dir}/patch.c
        ${payload_source_dir}/tt.c
        ${payload_source_dir}/offsets.h
        ${payload_source_dir}/symorder.txt
    )

    foreach(payload_source IN LISTS payload_sources)
        if(NOT EXISTS ${payload_source})
            message(FATAL_ERROR "missing payload input: ${payload_source}")
        endif()
    endforeach()

    add_custom_command(
        OUTPUT ${payload_header}
        BYPRODUCTS ${payload_macho} ${payload_bin}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${payload_build_dir} ${L41KA_GENERATED_PAYLOAD_DIR}
        COMMAND ${L41KA_PAYLOAD_CLANG}
            -arch arm64
            -Os
            ${L41KA_PAYLOAD_VERSION_FLAGS}
            -Wl,-preload
            -Wl,-segalign,0x10
            -Wl,-order_file,${payload_source_dir}/symorder.txt
            -nostdlib
            -fno-vectorize
            ${payload_source_dir}/start.S
            ${payload_source_dir}/patch.c
            ${payload_source_dir}/tt.c
            -o ${payload_macho}
        COMMAND ${L41KA_PAYLOAD_VMACHO} -f ${payload_macho} ${payload_bin}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT=${payload_bin}
            -DOUTPUT=${payload_header}
            -DSYMBOL=${payload_symbol}
            -DXXD=${L41KA_PAYLOAD_XXD}
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/GeneratePayloadHeader.cmake
        DEPENDS ${payload_sources} ${CMAKE_CURRENT_LIST_DIR}/cmake/GeneratePayloadHeader.cmake
        WORKING_DIRECTORY ${payload_source_dir}
        COMMENT "Generating ${payload_header}"
        VERBATIM
    )

    list(APPEND L41KA_GENERATED_PAYLOAD_HEADERS ${payload_header})
    set(L41KA_GENERATED_PAYLOAD_HEADERS ${L41KA_GENERATED_PAYLOAD_HEADERS} PARENT_SCOPE)
endfunction()

function(l41ka_add_asm_payload payload_symbol payload_source payload_entry)
    set(payload_build_dir ${L41KA_PAYLOAD_BUILD_DIR}/${payload_symbol})
    set(payload_macho ${payload_build_dir}/${payload_symbol}.o)
    set(payload_bin ${payload_build_dir}/${payload_symbol}.bin)
    set(payload_header ${L41KA_GENERATED_PAYLOAD_DIR}/${payload_symbol}.h)

    if(NOT EXISTS ${payload_source})
        message(FATAL_ERROR "missing payload input: ${payload_source}")
    endif()

    add_custom_command(
        OUTPUT ${payload_header}
        BYPRODUCTS ${payload_macho} ${payload_bin}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${payload_build_dir} ${L41KA_GENERATED_PAYLOAD_DIR}
        COMMAND ${L41KA_PAYLOAD_CLANG}
            -arch arm64
            -Os
            ${L41KA_PAYLOAD_VERSION_FLAGS}
            -Wl,-preload
            -Wl,-segalign,0x10
            -Wl,-e,${payload_entry}
            -nostdlib
            ${payload_source}
            -o ${payload_macho}
        COMMAND ${L41KA_PAYLOAD_VMACHO} -f ${payload_macho} ${payload_bin}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT=${payload_bin}
            -DOUTPUT=${payload_header}
            -DSYMBOL=${payload_symbol}
            -DXXD=${L41KA_PAYLOAD_XXD}
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/GeneratePayloadHeader.cmake
        DEPENDS ${payload_source} ${ARGN} ${CMAKE_CURRENT_LIST_DIR}/cmake/GeneratePayloadHeader.cmake
        COMMENT "Generating ${payload_header}"
        VERBATIM
    )

    list(APPEND L41KA_GENERATED_PAYLOAD_HEADERS ${payload_header})
    set(L41KA_GENERATED_PAYLOAD_HEADERS ${L41KA_GENERATED_PAYLOAD_HEADERS} PARENT_SCOPE)
endfunction()

function(l41ka_add_pongoos_payload pongo_output_variable kpf_output_variable)
    set(pongo_source_dir ${CMAKE_CURRENT_LIST_DIR}/src/payloads/PongoOS)
    set(pongo_build_dir ${L41KA_PAYLOAD_BUILD_DIR}/PongoOS)
    set(pongo_bin ${pongo_build_dir}/Pongo.bin)
    set(kpf_bin ${pongo_build_dir}/checkra1n-kpf-pongo)
    set(pongo_embedded_cflags "${L41KA_PONGOOS_COMPAT_CFLAGS}")
    if(L41KA_PONGOOS_EMBEDDED_CFLAGS)
        string(APPEND pongo_embedded_cflags " ${L41KA_PONGOOS_EMBEDDED_CFLAGS}")
    endif()

    file(GLOB_RECURSE pongo_sources CONFIGURE_DEPENDS
        ${pongo_source_dir}/src/*.c
        ${pongo_source_dir}/src/*.h
        ${pongo_source_dir}/src/*.S
        ${pongo_source_dir}/src/*.txt
        ${pongo_source_dir}/include/*.h
        ${pongo_source_dir}/apple-include/*.h
        ${pongo_source_dir}/checkra1n/kpf/*.c
        ${pongo_source_dir}/checkra1n/kpf/*.h
        ${pongo_source_dir}/checkra1n/kpf/*.S
        ${pongo_source_dir}/tools/vmacho.c
    )
    list(APPEND pongo_sources ${pongo_source_dir}/Makefile)

    add_custom_command(
        OUTPUT ${pongo_bin} ${kpf_bin}
        BYPRODUCTS
            ${pongo_build_dir}/Pongo
            ${pongo_build_dir}/Pongo.dSYM/Contents/Info.plist
            ${pongo_build_dir}/Pongo.dSYM/Contents/Resources/DWARF/Pongo
            ${pongo_build_dir}/Pongo.dSYM/Contents/Resources/Relocations/aarch64/Pongo.yml
            ${pongo_build_dir}/Pongo.lto.o
            ${pongo_build_dir}/checkra1n-kpf-pongo.dSYM/Contents/Info.plist
            ${pongo_build_dir}/checkra1n-kpf-pongo.dSYM/Contents/Resources/DWARF/checkra1n-kpf-pongo
            ${pongo_build_dir}/checkra1n-kpf-pongo.dSYM/Contents/Resources/Relocations/aarch64/checkra1n-kpf-pongo.yml
            ${pongo_build_dir}/checkra1n-kpf-pongo.lto.o
            ${pongo_build_dir}/vmacho
        COMMAND ${CMAKE_COMMAND} -E make_directory ${pongo_build_dir}
        COMMAND ${L41KA_PONGOOS_MAKE}
            -C ${pongo_source_dir}
            BUILD=${pongo_build_dir}
            EMBEDDED_CC=${L41KA_PAYLOAD_CLANG}
            "EMBEDDED_CFLAGS=${pongo_embedded_cflags}"
            ${pongo_bin} ${kpf_bin}
        DEPENDS ${pongo_sources}
        COMMENT "Building PongoOS payload"
        VERBATIM
    )

    set(${pongo_output_variable} ${pongo_bin} PARENT_SCOPE)
    set(${kpf_output_variable} ${kpf_bin} PARENT_SCOPE)
endfunction()

function(l41ka_add_freestanding_payload payload_symbol payload_entry)
    cmake_parse_arguments(PAYLOAD "EMBED_ASM;APPEND_METADATA" "APPEND_BINARY;APPEND_ALIGN;APPEND_MIN_SIZE" "FLAGS;CXX_FLAGS;CXX_SOURCES;PREFIX_SOURCES;SOURCES;DEPENDS" ${ARGN})
    if(PAYLOAD_SOURCES)
        set(payload_sources ${PAYLOAD_SOURCES})
    else()
        set(payload_sources ${ARGN})
    endif()
    set(payload_dependencies ${PAYLOAD_PREFIX_SOURCES} ${payload_sources} ${PAYLOAD_CXX_SOURCES} ${PAYLOAD_DEPENDS})
    set(payload_build_dir ${L41KA_PAYLOAD_BUILD_DIR}/${payload_symbol})
    set(payload_macho ${payload_build_dir}/${payload_symbol}.o)
    set(payload_base_bin ${payload_build_dir}/${payload_symbol}.base.bin)
    set(payload_bin ${payload_build_dir}/${payload_symbol}.bin)
    set(payload_header ${L41KA_GENERATED_PAYLOAD_DIR}/${payload_symbol}.h)
    set(payload_asm ${payload_build_dir}/${payload_symbol}.S)

    if(PAYLOAD_APPEND_BINARY AND NOT PAYLOAD_APPEND_ALIGN)
        set(PAYLOAD_APPEND_ALIGN 0x4000)
    endif()
    if(PAYLOAD_APPEND_BINARY AND NOT PAYLOAD_APPEND_MIN_SIZE)
        set(PAYLOAD_APPEND_MIN_SIZE 0)
    endif()

    if(NOT payload_sources)
        message(FATAL_ERROR "missing payload inputs for ${payload_symbol}")
    endif()

    foreach(payload_source IN LISTS payload_sources)
        if(NOT EXISTS ${payload_source})
            message(FATAL_ERROR "missing payload input: ${payload_source}")
        endif()
    endforeach()

    foreach(payload_source IN LISTS PAYLOAD_PREFIX_SOURCES)
        if(NOT EXISTS ${payload_source})
            message(FATAL_ERROR "missing payload input: ${payload_source}")
        endif()
    endforeach()

    foreach(payload_source IN LISTS PAYLOAD_CXX_SOURCES)
        if(NOT EXISTS ${payload_source})
            message(FATAL_ERROR "missing payload input: ${payload_source}")
        endif()
    endforeach()

    set(payload_outputs ${payload_header})
    if(PAYLOAD_EMBED_ASM)
        list(APPEND payload_outputs ${payload_asm})
    endif()

    set(payload_binary_commands
        COMMAND ${L41KA_PAYLOAD_VMACHO} -f ${payload_macho} ${payload_base_bin}
    )

    if(PAYLOAD_APPEND_BINARY)
        list(APPEND payload_dependencies ${PAYLOAD_APPEND_BINARY} ${CMAKE_CURRENT_LIST_DIR}/cmake/AppendAlignedBinary.sh)
        list(APPEND payload_binary_commands
            COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/cmake/AppendAlignedBinary.sh
                ${payload_base_bin}
                ${PAYLOAD_APPEND_BINARY}
                ${payload_bin}
                ${PAYLOAD_APPEND_ALIGN}
                ${PAYLOAD_APPEND_MIN_SIZE}
                $<IF:$<BOOL:${PAYLOAD_APPEND_METADATA}>,1,0>
        )
    else()
        list(APPEND payload_binary_commands
            COMMAND ${CMAKE_COMMAND} -E copy ${payload_base_bin} ${payload_bin}
        )
    endif()

    if(PAYLOAD_EMBED_ASM)
        list(APPEND payload_dependencies ${CMAKE_CURRENT_LIST_DIR}/cmake/GenerateBinaryAssembly.cmake)
        list(APPEND payload_binary_commands
            COMMAND ${CMAKE_COMMAND}
                -DINPUT=${payload_bin}
                -DOUTPUT_ASM=${payload_asm}
                -DOUTPUT_HEADER=${payload_header}
                -DSYMBOL=${payload_symbol}
                -DSECTION=.flashdata.payloads
                -P ${CMAKE_CURRENT_LIST_DIR}/cmake/GenerateBinaryAssembly.cmake
        )
    else()
        list(APPEND payload_binary_commands
            COMMAND ${CMAKE_COMMAND}
                -DINPUT=${payload_bin}
                -DOUTPUT=${payload_header}
                -DSYMBOL=${payload_symbol}
                -DXXD=${L41KA_PAYLOAD_XXD}
                -P ${CMAKE_CURRENT_LIST_DIR}/cmake/GeneratePayloadHeader.cmake
        )
    endif()

    set(payload_compile_commands)
    set(payload_cxx_objects)
    set(payload_cxx_index 0)
    foreach(payload_source IN LISTS PAYLOAD_CXX_SOURCES)
        set(payload_object ${payload_build_dir}/cxx-${payload_cxx_index}.o)
        list(APPEND payload_cxx_objects ${payload_object})
        list(APPEND payload_compile_commands
            COMMAND ${L41KA_PAYLOAD_CLANG}
                -arch arm64
                -Os
                ${L41KA_PAYLOAD_VERSION_FLAGS}
                -I${CMAKE_CURRENT_LIST_DIR}/include
                -ffreestanding
                -fno-builtin
                -fno-stack-protector
                -fomit-frame-pointer
                -fno-unwind-tables
                -fno-asynchronous-unwind-tables
                -fno-jump-tables
                ${PAYLOAD_CXX_FLAGS}
                -c ${payload_source}
                -o ${payload_object}
        )
        math(EXPR payload_cxx_index "${payload_cxx_index} + 1")
    endforeach()

    add_custom_command(
        OUTPUT ${payload_outputs}
        BYPRODUCTS ${payload_macho} ${payload_base_bin} ${payload_bin} ${payload_cxx_objects}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${payload_build_dir} ${L41KA_GENERATED_PAYLOAD_DIR}
        ${payload_compile_commands}
        COMMAND ${L41KA_PAYLOAD_CLANG}
            -arch arm64
            -Os
            ${L41KA_PAYLOAD_VERSION_FLAGS}
            -ffreestanding
            -fno-builtin
            -fno-stack-protector
            -fomit-frame-pointer
            -fno-unwind-tables
            -fno-asynchronous-unwind-tables
            -fno-jump-tables
            ${PAYLOAD_FLAGS}
            -Wl,-preload
            -Wl,-segalign,0x10
            -Wl,-e,${payload_entry}
            -nostdlib
            ${PAYLOAD_PREFIX_SOURCES}
            ${payload_cxx_objects}
            ${payload_sources}
            -o ${payload_macho}
        ${payload_binary_commands}
        DEPENDS ${payload_dependencies} ${CMAKE_CURRENT_LIST_DIR}/cmake/GeneratePayloadHeader.cmake
        COMMENT "Generating ${payload_header}"
        VERBATIM
    )

    add_custom_target(${payload_symbol}
        DEPENDS ${payload_outputs}
        SOURCES ${PAYLOAD_PREFIX_SOURCES} ${payload_sources} ${PAYLOAD_DEPENDS}
    )

    if(PAYLOAD_CXX_SOURCES)
        add_library(${payload_symbol}_cxx OBJECT EXCLUDE_FROM_ALL ${PAYLOAD_CXX_SOURCES})
        set_target_properties(${payload_symbol}_cxx PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(${payload_symbol}_cxx PRIVATE COMBINED_STAGE2_HOST_TEST)
        target_include_directories(${payload_symbol}_cxx PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
        target_compile_options(${payload_symbol}_cxx PRIVATE
            -ffreestanding
            -fno-builtin
            -fno-stack-protector
            -fomit-frame-pointer
            -fno-unwind-tables
            -fno-asynchronous-unwind-tables
            -fno-jump-tables
            -fno-exceptions
            -fno-rtti
            -fno-threadsafe-statics
        )
    endif()

    list(APPEND L41KA_GENERATED_PAYLOAD_HEADERS ${payload_header})
    if(PAYLOAD_EMBED_ASM)
        set_source_files_properties(${payload_asm} PROPERTIES GENERATED TRUE)
        list(APPEND L41KA_GENERATED_PAYLOAD_SOURCES ${payload_asm})
    endif()
    set(L41KA_GENERATED_PAYLOAD_HEADERS ${L41KA_GENERATED_PAYLOAD_HEADERS} PARENT_SCOPE)
    set(L41KA_GENERATED_PAYLOAD_SOURCES ${L41KA_GENERATED_PAYLOAD_SOURCES} PARENT_SCOPE)
endfunction()

set(L41KA_T8020_SHELLCODE_DIR ${CMAKE_CURRENT_LIST_DIR}/src/payloads/shellcode/t8020)
l41ka_add_asm_payload(shellcode_t8020 ${L41KA_T8020_SHELLCODE_DIR}/start.S start
    ${L41KA_T8020_SHELLCODE_DIR}/offsets.h
    ${L41KA_T8020_SHELLCODE_DIR}/cleanup.S
    ${L41KA_T8020_SHELLCODE_DIR}/blocks.S
)
l41ka_add_shellcode_payload(t8030)
l41ka_add_asm_payload(usb_handler ${CMAKE_CURRENT_LIST_DIR}/src/payloads/usb_handler/handler.S _handler)
set(L41KA_COMBINED_STAGE2_DIR ${CMAKE_CURRENT_LIST_DIR}/src/payloads/iboot)
l41ka_add_freestanding_payload(combined_stage2_payload _start
    CXX_SOURCES
        ${L41KA_COMBINED_STAGE2_DIR}/patchfinder/patchfinder.cpp
    CXX_FLAGS
        -std=c++20
        -Oz
        -fno-exceptions
        -fno-rtti
        -fno-threadsafe-statics
        -fexperimental-relative-c++-abi-vtables
        -fdata-sections
        -ffunction-sections
    # Keep _start first while placing C++ constants and LaikaDFU USB descriptors
    # before the trailing offsets section, which PreparePayload expects at EOF.
    PREFIX_SOURCES
        ${L41KA_COMBINED_STAGE2_DIR}/patchfinder/entry.S
        ${L41KA_COMBINED_STAGE2_DIR}/patchfinder/mmu.S
    SOURCES
        ${L41KA_COMBINED_STAGE2_DIR}/laikadfu/entry.S
        ${L41KA_COMBINED_STAGE2_DIR}/laikadfu/usb/usb.c
        ${L41KA_COMBINED_STAGE2_DIR}/laikadfu/laikadfu.c
        ${L41KA_COMBINED_STAGE2_DIR}/laikadfu/usb/synopsys.c
        ${L41KA_COMBINED_STAGE2_DIR}/laikadfu/ll.S
    DEPENDS
        ${L41KA_COMBINED_STAGE2_DIR}/laikadfu/laikadfu_offsets.h
        ${L41KA_COMBINED_STAGE2_DIR}/laikadfu/diag.h
        ${L41KA_COMBINED_STAGE2_DIR}/laikadfu/usb/ch9.h
        ${L41KA_COMBINED_STAGE2_DIR}/laikadfu/usb/synopsys.h
        ${L41KA_COMBINED_STAGE2_DIR}/laikadfu/usb/usb.h
        ${CMAKE_CURRENT_LIST_DIR}/include/pf_aarch64.h
    FLAGS
        -Wl,-dead_strip
)
l41ka_add_pongoos_payload(L41KA_PONGOOS_BIN L41KA_CHECKRA1N_KPF_BIN)
add_custom_target(pongoos_payload DEPENDS ${L41KA_PONGOOS_BIN} ${L41KA_CHECKRA1N_KPF_BIN})
set(L41KA_PONGO_PAYLOAD_ASM ${L41KA_PAYLOAD_BUILD_DIR}/pongo_payload/pongo_payload.S)
set(L41KA_PONGO_PAYLOAD_HEADER ${L41KA_GENERATED_PAYLOAD_DIR}/pongo_payload.h)
add_custom_command(
    OUTPUT ${L41KA_PONGO_PAYLOAD_ASM} ${L41KA_PONGO_PAYLOAD_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${L41KA_PAYLOAD_BUILD_DIR}/pongo_payload ${L41KA_GENERATED_PAYLOAD_DIR}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${L41KA_PONGOOS_BIN}
        -DOUTPUT_ASM=${L41KA_PONGO_PAYLOAD_ASM}
        -DOUTPUT_HEADER=${L41KA_PONGO_PAYLOAD_HEADER}
        -DSYMBOL=pongo_payload
        -DSECTION=.flashdata.payloads
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/GenerateBinaryAssembly.cmake
    DEPENDS ${L41KA_PONGOOS_BIN} ${CMAKE_CURRENT_LIST_DIR}/cmake/GenerateBinaryAssembly.cmake
    COMMENT "Embedding PongoOS payload"
    VERBATIM
)
set_source_files_properties(${L41KA_PONGO_PAYLOAD_ASM} PROPERTIES GENERATED TRUE)
list(APPEND L41KA_GENERATED_PAYLOAD_HEADERS ${L41KA_PONGO_PAYLOAD_HEADER})
list(APPEND L41KA_GENERATED_PAYLOAD_SOURCES ${L41KA_PONGO_PAYLOAD_ASM})
set(L41KA_CHECKRA1N_KPF_PAYLOAD_ASM ${L41KA_PAYLOAD_BUILD_DIR}/checkra1n_kpf_payload/checkra1n_kpf_payload.S)
set(L41KA_CHECKRA1N_KPF_PAYLOAD_HEADER ${L41KA_GENERATED_PAYLOAD_DIR}/checkra1n_kpf_payload.h)
add_custom_command(
    OUTPUT ${L41KA_CHECKRA1N_KPF_PAYLOAD_ASM} ${L41KA_CHECKRA1N_KPF_PAYLOAD_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${L41KA_PAYLOAD_BUILD_DIR}/checkra1n_kpf_payload ${L41KA_GENERATED_PAYLOAD_DIR}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${L41KA_CHECKRA1N_KPF_BIN}
        -DOUTPUT_ASM=${L41KA_CHECKRA1N_KPF_PAYLOAD_ASM}
        -DOUTPUT_HEADER=${L41KA_CHECKRA1N_KPF_PAYLOAD_HEADER}
        -DSYMBOL=checkra1n_kpf_payload
        -DSECTION=.flashdata.payloads
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/GenerateBinaryAssembly.cmake
    DEPENDS ${L41KA_CHECKRA1N_KPF_BIN} ${CMAKE_CURRENT_LIST_DIR}/cmake/GenerateBinaryAssembly.cmake
    COMMENT "Embedding checkra1n KPF payload"
    VERBATIM
)
set_source_files_properties(${L41KA_CHECKRA1N_KPF_PAYLOAD_ASM} PROPERTIES GENERATED TRUE)
list(APPEND L41KA_GENERATED_PAYLOAD_HEADERS ${L41KA_CHECKRA1N_KPF_PAYLOAD_HEADER})
list(APPEND L41KA_GENERATED_PAYLOAD_SOURCES ${L41KA_CHECKRA1N_KPF_PAYLOAD_ASM})
set(L41KA_RAMDISK_COMPRESSED ${L41KA_PAYLOAD_BUILD_DIR}/ramdisk_payload/ramdisk.dmg.lzma)
set(L41KA_RAMDISK_PAYLOAD_ASM ${L41KA_PAYLOAD_BUILD_DIR}/ramdisk_payload/ramdisk_payload.S)
set(L41KA_RAMDISK_PAYLOAD_HEADER ${L41KA_GENERATED_PAYLOAD_DIR}/ramdisk_payload.h)
if(EXISTS "${L41KA_RAMDISK_IMAGE}")
    file(SIZE "${L41KA_RAMDISK_IMAGE}" L41KA_RAMDISK_UNCOMPRESSED_SIZE)
    add_custom_command(
        OUTPUT ${L41KA_RAMDISK_COMPRESSED}
        COMMAND ${CMAKE_COMMAND}
            -DINPUT=${L41KA_RAMDISK_IMAGE}
            -DOUTPUT=${L41KA_RAMDISK_COMPRESSED}
            -DXZ=${L41KA_RAMDISK_XZ}
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/CompressLzma.cmake
        DEPENDS ${L41KA_RAMDISK_IMAGE} ${CMAKE_CURRENT_LIST_DIR}/cmake/CompressLzma.cmake
        COMMENT "Compressing embedded ramdisk"
        VERBATIM
    )
else()
    set(L41KA_RAMDISK_UNCOMPRESSED_SIZE 0)
    add_custom_command(
        OUTPUT ${L41KA_RAMDISK_COMPRESSED}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${L41KA_PAYLOAD_BUILD_DIR}/ramdisk_payload
        COMMAND ${CMAKE_COMMAND} -E rm -f ${L41KA_RAMDISK_COMPRESSED}
        COMMAND ${CMAKE_COMMAND} -E touch ${L41KA_RAMDISK_COMPRESSED}
        COMMENT "Generating empty embedded ramdisk placeholder"
        VERBATIM
    )
endif()
add_custom_command(
    OUTPUT ${L41KA_RAMDISK_PAYLOAD_ASM} ${L41KA_RAMDISK_PAYLOAD_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${L41KA_RAMDISK_COMPRESSED}
        -DOUTPUT_ASM=${L41KA_RAMDISK_PAYLOAD_ASM}
        -DOUTPUT_HEADER=${L41KA_RAMDISK_PAYLOAD_HEADER}
        -DSYMBOL=ramdisk_payload
        -DSECTION=.flashdata.payloads
        -P ${CMAKE_CURRENT_LIST_DIR}/cmake/GenerateBinaryAssembly.cmake
    DEPENDS ${L41KA_RAMDISK_COMPRESSED} ${CMAKE_CURRENT_LIST_DIR}/cmake/GenerateBinaryAssembly.cmake
    COMMENT "Embedding ramdisk payload"
    VERBATIM
)
set_source_files_properties(${L41KA_RAMDISK_PAYLOAD_ASM} PROPERTIES GENERATED TRUE)
list(APPEND L41KA_GENERATED_PAYLOAD_HEADERS ${L41KA_RAMDISK_PAYLOAD_HEADER})
list(APPEND L41KA_GENERATED_PAYLOAD_SOURCES ${L41KA_RAMDISK_PAYLOAD_ASM})
add_custom_target(payloads DEPENDS ${L41KA_GENERATED_PAYLOAD_HEADERS} ${L41KA_GENERATED_PAYLOAD_SOURCES})

set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/src/exploits/USBLiter8.cpp
        ${CMAKE_CURRENT_LIST_DIR}/src/rom-patcher/iBootPatcherSetup.cpp
    PROPERTIES OBJECT_DEPENDS "${L41KA_GENERATED_PAYLOAD_HEADERS}"
)

add_executable(l41ka
        main.cpp
    ${L41KA_GENERATED_PAYLOAD_SOURCES}
    src/usb/libusb/pico_libusb.c
    ${PIO_USB_DIR}/pio_usb.c
    ${PIO_USB_DIR}/pio_usb_device.c
    ${PIO_USB_DIR}/pio_usb_host.c
    ${PIO_USB_DIR}/usb_crc.c
        src/usb/Device.cpp
        include/usb/Device.h
        include/usb/DeviceHandle.h
        src/usb/DFUDevice.cpp
        include/usb/DFUDevice.h
        src/usb/LaikaDFUDevice.cpp
        include/usb/LaikaDFUDevice.h
        src/usb/PwnedDFUDevice.cpp
        include/usb/PwnedDFUDevice.h
        src/usb/RecoveryDevice.cpp
        include/usb/RecoveryDevice.h
        src/usb/PongoDevice.cpp
        include/usb/PongoDevice.h
        src/control/Protocol.cpp
        include/control/Protocol.h
        src/control/Decoder.cpp
        include/control/Decoder.h
        src/control/Fault.cpp
        include/control/Fault.h
        src/control/TargetWorker.cpp
        include/control/TargetWorker.h
        src/control/DfuInfo.cpp
        include/control/DfuInfo.h
        src/control/AppleDeviceDatabase.cpp
        include/control/AppleDeviceDatabase.h
        src/control/Controller.cpp
        include/control/Controller.h
        src/control/UsbDescriptors.cpp
        include/tusb_config.h
        src/control/Log.cpp
        include/control/Log.h
        src/control/LED.cpp
        include/control/LED.h
        src/exploits/USBLiter8.cpp
        src/exploits/USBLiter8.h
        src/rom-patcher/iBootPatcherSetup.cpp
        include/rom-patcher/iBootPatcherSetup.h
)

pico_generate_pio_header(l41ka ${PIO_USB_DIR}/usb_tx.pio)
pico_generate_pio_header(l41ka ${PIO_USB_DIR}/usb_rx.pio)

target_include_directories(l41ka
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}/include/ui
        ${CMAKE_CURRENT_LIST_DIR}/include/usb
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${L41KA_GENERATED_PAYLOAD_DIR}
        ${PIO_USB_DIR}
)

add_dependencies(l41ka payloads)

target_link_libraries(l41ka
    PRIVATE
        pico_stdlib
        pico_multicore
        pico_util
        hardware_pio
        hardware_dma
        hardware_pwm
        hardware_watchdog
        pico_status_led
        pico_unique_id
        tinyusb_device
)

pico_enable_stdio_usb(l41ka 0)

target_compile_definitions(l41ka
    PRIVATE
        L41KA_RAMDISK_UNCOMPRESSED_SIZE=${L41KA_RAMDISK_UNCOMPRESSED_SIZE}
        PICO_LIBUSB_CONNECT_TIMEOUT_MS=5000
        PICO_STACK_SIZE=0x1000
)

target_compile_options(l41ka
    PRIVATE
        -UCFG_TUSB_DEBUG
        -DCFG_TUSB_DEBUG=0
        -Wall
        -Wextra
)

pico_add_extra_outputs(l41ka)

include(CTest)
add_custom_target(host-tests
    COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/tests/run_host_tests.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    USES_TERMINAL
)
if(BUILD_TESTING)
    add_test(NAME l41ka_host_tests COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/tests/run_host_tests.sh)
    set_tests_properties(l41ka_host_tests PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})
endif()

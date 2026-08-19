cmake_minimum_required(VERSION 3.27)

get_filename_component(SHADER_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang)

if (NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "compile_shaders: glslangValidator not found. Install the Vulkan SDK or a glslang package.")
endif()

find_program(SPIRV_VAL NAMES spirv-val)
find_program(SPIRV_DIS NAMES spirv-dis)

if (NOT SPIRV_DIS)
  message(FATAL_ERROR "compile_shaders: spirv-dis not found. Install SPIRV-Tools; the resource counts are read from it.")
endif()

function(count_descriptor_set DISASSEMBLY SET_INDEX OUTPUT_VARIABLE)
  string(REGEX MATCHALL "DescriptorSet ${SET_INDEX}[^0-9]" MATCHES "${DISASSEMBLY}")
  list(LENGTH MATCHES MATCH_COUNT)
  set(${OUTPUT_VARIABLE} ${MATCH_COUNT} PARENT_SCOPE)
endfunction()

set(SHADER_NAMES world movie model light world_oit model_oit)
set(SHADER_STAGES vert frag)

foreach(SHADER_NAME IN LISTS SHADER_NAMES)

string(SUBSTRING "${SHADER_NAME}" 0 1 NAME_HEAD)
string(SUBSTRING "${SHADER_NAME}" 1 -1 NAME_TAIL)
string(TOUPPER "${NAME_HEAD}" NAME_HEAD)
set(SYMBOL_BASE "${NAME_HEAD}${NAME_TAIL}")

if (SHADER_NAME STREQUAL "world_oit")
  set(SYMBOL_BASE "WorldOit")
elseif (SHADER_NAME STREQUAL "model_oit")
  set(SYMBOL_BASE "ModelOit")
endif()

set(GENERATED_BODY "")

foreach(STAGE IN LISTS SHADER_STAGES)
  set(SHADER_SOURCE_NAME "${SHADER_NAME}")
  set(EXTRA_DEFINES "")

  if (SHADER_NAME STREQUAL "world_oit" OR SHADER_NAME STREQUAL "model_oit")
    if (STAGE STREQUAL "vert")
      continue()
    endif()

    if (SHADER_NAME STREQUAL "world_oit")
      set(SHADER_SOURCE_NAME "world")
    else()
      set(SHADER_SOURCE_NAME "model")
    endif()

    set(EXTRA_DEFINES "-DEDGE_OIT_PASS=1")
  endif()

  set(SOURCE_FILE "${SHADER_DIR}/${SHADER_SOURCE_NAME}.${STAGE}.glsl")
  set(BINARY_FILE "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.${STAGE}.spv")

  execute_process(
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.0 ${EXTRA_DEFINES} -S ${STAGE} -o "${BINARY_FILE}" "${SOURCE_FILE}"
    RESULT_VARIABLE COMPILE_RESULT
    OUTPUT_VARIABLE COMPILE_OUTPUT
    ERROR_VARIABLE COMPILE_OUTPUT)

  if (NOT COMPILE_RESULT EQUAL 0)
    message(FATAL_ERROR "compile_shaders: glslangValidator failed on ${SHADER_NAME}.${STAGE}.glsl\n${COMPILE_OUTPUT}")
  endif()

  if (SPIRV_VAL)
    execute_process(
      COMMAND "${SPIRV_VAL}" "${BINARY_FILE}"
      RESULT_VARIABLE VALIDATE_RESULT
      OUTPUT_VARIABLE VALIDATE_OUTPUT
      ERROR_VARIABLE VALIDATE_OUTPUT)

    if (NOT VALIDATE_RESULT EQUAL 0)
      message(FATAL_ERROR "compile_shaders: spirv-val rejected ${SHADER_NAME}.${STAGE}.spv\n${VALIDATE_OUTPUT}")
    endif()
  endif()

  execute_process(
    COMMAND "${SPIRV_DIS}" --no-color "${BINARY_FILE}"
    RESULT_VARIABLE DISASSEMBLE_RESULT
    OUTPUT_VARIABLE DISASSEMBLY
    ERROR_VARIABLE DISASSEMBLE_ERROR)

  if (NOT DISASSEMBLE_RESULT EQUAL 0)
    message(FATAL_ERROR "compile_shaders: spirv-dis failed on ${SHADER_NAME}.${STAGE}.spv\n${DISASSEMBLE_ERROR}")
  endif()

  if (STAGE STREQUAL "vert")
    set(SAMPLER_SET 0)
    set(UNIFORM_SET 1)
    set(STAGE_PREFIX "k${SYMBOL_BASE}VertexShader")
  else()
    set(SAMPLER_SET 2)
    set(UNIFORM_SET 3)
    set(STAGE_PREFIX "k${SYMBOL_BASE}FragmentShader")
  endif()

  count_descriptor_set("${DISASSEMBLY}" ${SAMPLER_SET} SAMPLER_COUNT)
  count_descriptor_set("${DISASSEMBLY}" ${UNIFORM_SET} UNIFORM_BUFFER_COUNT)

  foreach(STRAY_SET RANGE 0 3)
    if (NOT STRAY_SET EQUAL SAMPLER_SET AND NOT STRAY_SET EQUAL UNIFORM_SET)
      count_descriptor_set("${DISASSEMBLY}" ${STRAY_SET} STRAY_COUNT)
      if (NOT STRAY_COUNT EQUAL 0)
        message(FATAL_ERROR
          "compile_shaders: ${SHADER_NAME}.${STAGE}.glsl declares ${STRAY_COUNT} resource(s) in set ${STRAY_SET}, "
          "which SDL_GPU reserves for the other shader stage.")
      endif()
    endif()
  endforeach()

  file(READ "${BINARY_FILE}" HEX_BYTES HEX)
  string(LENGTH "${HEX_BYTES}" HEX_LENGTH)

  math(EXPR BYTE_COUNT "${HEX_LENGTH} / 2")

  if (NOT BYTE_COUNT EQUAL 0)
    math(EXPR REMAINDER "${BYTE_COUNT} % 4")
    if (NOT REMAINDER EQUAL 0)
      message(FATAL_ERROR "compile_shaders: world.${STAGE}.spv is ${BYTE_COUNT} bytes, not a whole number of words.")
    endif()
  endif()

  math(EXPR WORD_COUNT "${BYTE_COUNT} / 4")
  math(EXPR LAST_WORD "${WORD_COUNT} - 1")

  string(APPEND GENERATED_BODY "static const uint32_t ${STAGE_PREFIX}SamplerCount = ${SAMPLER_COUNT};\n")
  string(APPEND GENERATED_BODY "static const uint32_t ${STAGE_PREFIX}UniformBufferCount = ${UNIFORM_BUFFER_COUNT};\n\n")
  string(APPEND GENERATED_BODY "static const uint32_t ${STAGE_PREFIX}Spirv[] = {")

  foreach(WORD_INDEX RANGE ${LAST_WORD})
    math(EXPR HEX_OFFSET "${WORD_INDEX} * 8")

    string(SUBSTRING "${HEX_BYTES}" ${HEX_OFFSET} 2 BYTE_0)
    math(EXPR HEX_OFFSET "${HEX_OFFSET} + 2")
    string(SUBSTRING "${HEX_BYTES}" ${HEX_OFFSET} 2 BYTE_1)
    math(EXPR HEX_OFFSET "${HEX_OFFSET} + 2")
    string(SUBSTRING "${HEX_BYTES}" ${HEX_OFFSET} 2 BYTE_2)
    math(EXPR HEX_OFFSET "${HEX_OFFSET} + 2")
    string(SUBSTRING "${HEX_BYTES}" ${HEX_OFFSET} 2 BYTE_3)

    math(EXPR COLUMN "${WORD_INDEX} % 6")
    if (COLUMN EQUAL 0)
      string(APPEND GENERATED_BODY "\n    ")
    else()
      string(APPEND GENERATED_BODY " ")
    endif()

    string(APPEND GENERATED_BODY "0x${BYTE_3}${BYTE_2}${BYTE_1}${BYTE_0},")
  endforeach()

  string(APPEND GENERATED_BODY "\n};\n\n")

  message(STATUS "compile_shaders: ${SHADER_NAME}.${STAGE}.glsl -> ${WORD_COUNT} words, ${SAMPLER_COUNT} sampler(s) in set "
                 "${SAMPLER_SET}, ${UNIFORM_BUFFER_COUNT} uniform buffer(s) in set ${UNIFORM_SET}")
endforeach()

set(GENERATED_HEADER "#pragma once\n\n#include <stdint.h>\n\n${GENERATED_BODY}")

file(WRITE "${SHADER_DIR}/${SHADER_NAME}_spirv.h" "${GENERATED_HEADER}")

message(STATUS "compile_shaders: wrote ${SHADER_DIR}/${SHADER_NAME}_spirv.h")

endforeach()

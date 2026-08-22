cmake_minimum_required(VERSION 3.27)

get_filename_component(SHADER_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang)

if (NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "compile_shaders: glslangValidator not found. Install the Vulkan SDK or a glslang package.")
endif()

set(SHADER_NAMES world movie model oit)

set(SHADER_STAGES vert frag)
set(STAGE_SYMBOLS Vertex Fragment)

file(READ "${SHADER_DIR}/../../../r_lightgrid.h" LIGHT_GRID_HEADER)

string(REGEX MATCH "kLightGridMaximumPerTile[ \t]*=[ \t]*([0-9]+)" LIGHT_GRID_MATCH "${LIGHT_GRID_HEADER}")

if (NOT LIGHT_GRID_MATCH)
  message(FATAL_ERROR "compile_shaders: could not read kLightGridMaximumPerTile from r_lightgrid.h")
endif()

set(LIGHT_MAX_PER_TILE "${CMAKE_MATCH_1}")

message(STATUS "compile_shaders: EDGE_LIGHT_MAX_PER_TILE = ${LIGHT_MAX_PER_TILE} (from r_lightgrid.h)")

string(REGEX MATCH "kLightGridMaximumGlows[ \t]*=[ \t]*([0-9]+)" GLOW_MATCH "${LIGHT_GRID_HEADER}")

if (NOT GLOW_MATCH)
  message(FATAL_ERROR "compile_shaders: could not read kLightGridMaximumGlows from r_lightgrid.h")
endif()

set(LIGHT_MAX_GLOWS "${CMAKE_MATCH_1}")

message(STATUS "compile_shaders: EDGE_LIGHT_MAX_GLOWS = ${LIGHT_MAX_GLOWS} (from r_lightgrid.h)")

set(SHARED_DEFINES "#define EDGE_LIGHT_MAX_PER_TILE ${LIGHT_MAX_PER_TILE}\n#define EDGE_LIGHT_MAX_GLOWS ${LIGHT_MAX_GLOWS}\n")

set(GLES_VERTEX_PREAMBLE "#version 100\n${SHARED_DEFINES}")
set(GLES_FRAGMENT_PREAMBLE "#version 100\n${SHARED_DEFINES}#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n")
set(GL_PREAMBLE "#version 110\n${SHARED_DEFINES}")

foreach(SHADER_NAME IN LISTS SHADER_NAMES)

  string(SUBSTRING "${SHADER_NAME}" 0 1 NAME_HEAD)
  string(SUBSTRING "${SHADER_NAME}" 1 -1 NAME_TAIL)
  string(TOUPPER "${NAME_HEAD}" NAME_HEAD)
  set(SYMBOL_BASE "${NAME_HEAD}${NAME_TAIL}")

  set(GENERATED_HEADER "#pragma once\n\n")

  foreach(INDEX RANGE 1)
    list(GET SHADER_STAGES ${INDEX} STAGE)
    list(GET STAGE_SYMBOLS ${INDEX} STAGE_SYMBOL)

    set(SYMBOL "k${SYMBOL_BASE}${STAGE_SYMBOL}Source")

    set(SOURCE_FILE "${SHADER_DIR}/${SHADER_NAME}.${STAGE}.glsl")

    if (NOT EXISTS "${SOURCE_FILE}")
      message(FATAL_ERROR "compile_shaders: missing ${SOURCE_FILE}")
    endif()

    file(READ "${SOURCE_FILE}" STAGE_BODY)

    if (STAGE_BODY MATCHES "EDGE_INCLUDE_LIGHT_COMMON")
      file(READ "${SHADER_DIR}/light_common.glsl" LIGHT_COMMON_BODY)
      string(REPLACE "EDGE_INCLUDE_LIGHT_COMMON" "${LIGHT_COMMON_BODY}" STAGE_BODY "${STAGE_BODY}")
    endif()

    if (STAGE STREQUAL "frag")
      set(GLES_PREAMBLE "${GLES_FRAGMENT_PREAMBLE}")
    else()
      set(GLES_PREAMBLE "${GLES_VERTEX_PREAMBLE}")
    endif()

    foreach(DIALECT GLES GL)
      if (DIALECT STREQUAL "GLES")
        set(PREAMBLE "${GLES_PREAMBLE}")
        set(DIALECT_NAME "GLSL ES 1.00")
      else()
        set(PREAMBLE "${GL_PREAMBLE}")
        set(DIALECT_NAME "GLSL 1.10 (desktop GL 2.0)")
      endif()

      set(DIALECT_FILE "${SHADER_DIR}/${SHADER_NAME}_${DIALECT}.${STAGE}.glsl")

      file(WRITE "${DIALECT_FILE}" "${PREAMBLE}${STAGE_BODY}")

      execute_process(
        COMMAND "${GLSLANG_VALIDATOR}" "${DIALECT_FILE}"
        RESULT_VARIABLE VALIDATE_RESULT
        OUTPUT_VARIABLE VALIDATE_OUTPUT
        ERROR_VARIABLE VALIDATE_OUTPUT
      )

      if (NOT VALIDATE_RESULT EQUAL 0)
        message(FATAL_ERROR "compile_shaders: ${SHADER_NAME}_${DIALECT}.${STAGE}.glsl is not valid ${DIALECT_NAME}:\n${VALIDATE_OUTPUT}")
      endif()

      message(STATUS "compile_shaders: ${SHADER_NAME}_${DIALECT}.${STAGE}.glsl validated as ${DIALECT_NAME}")
    endforeach()

    set(EXPANDED_FILE "${SHADER_DIR}/${SHADER_NAME}_expanded.${STAGE}.glsl")

    file(WRITE "${EXPANDED_FILE}" "${STAGE_BODY}")

    file(STRINGS "${EXPANDED_FILE}" STAGE_LINES)

    file(REMOVE "${EXPANDED_FILE}")

    string(APPEND GENERATED_HEADER "static const char ${SYMBOL}[] =\n")

    foreach(LINE IN LISTS STAGE_LINES)
      string(REPLACE "\\" "\\\\" LINE "${LINE}")
      string(REPLACE "\"" "\\\"" LINE "${LINE}")
      string(APPEND GENERATED_HEADER "    \"${LINE}\\n\"\n")
    endforeach()

    string(APPEND GENERATED_HEADER "    ;\n\n")
  endforeach()

  file(WRITE "${SHADER_DIR}/${SHADER_NAME}_glsl.h" "${GENERATED_HEADER}")

  message(STATUS "compile_shaders: wrote ${SHADER_DIR}/${SHADER_NAME}_glsl.h")

endforeach()

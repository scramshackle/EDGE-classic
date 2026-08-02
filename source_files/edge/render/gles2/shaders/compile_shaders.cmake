cmake_minimum_required(VERSION 3.27)

get_filename_component(SHADER_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang)

if (NOT GLSLANG_VALIDATOR)
  message(FATAL_ERROR "compile_shaders: glslangValidator not found. Install the Vulkan SDK or a glslang package.")
endif()

set(SHADER_NAMES world movie)

set(SHADER_STAGES vert frag)
set(STAGE_SYMBOLS Vertex Fragment)

set(GLES_VERTEX_PREAMBLE "#version 100\n")
set(GLES_FRAGMENT_PREAMBLE "#version 100\n#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n")
set(GL_PREAMBLE "#version 110\n")

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

    file(STRINGS "${SOURCE_FILE}" STAGE_LINES)

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

##########################################
# epi
##########################################

cmake_minimum_required(VERSION 3.27)

project(epi LANGUAGES CXX)

option(EPI_EXTRA_CHECKS "Enable diagnostic checks/functions" OFF)

if (MSVC)
  string(REGEX REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

add_library(
  epi
  epi.cc
  epi_color.cc
  epi_crc.cc
  epi_file.cc
  epi_filesystem.cc
  epi_md5.cc
  epi_str_compare.cc
  epi_str_hash.cc
  epi_str_util.cc
  epi_vector.cc
  epi_scanner.cpp
)

target_include_directories(epi PUBLIC ./)

target_compile_features(epi PUBLIC cxx_std_17)

if (EPI_EXTRA_CHECKS)
  target_compile_definitions(epi PUBLIC EPI_EXTRA_CHECKS)
endif()

if (MSVC)
  target_compile_definitions(epi PRIVATE _CRT_SECURE_NO_WARNINGS _HAS_EXCEPTIONS=0)
  target_compile_options(epi PRIVATE /GR- /W4 /wd4100 /wd4244 /wd4267 /wd4996)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(epi PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wunreachable-code-aggressive)
else()
  target_compile_options(epi PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra)
endif()

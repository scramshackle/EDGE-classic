##########################################
# epi
##########################################

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

if (EPI_EXTRA_CHECKS)
  target_compile_definitions(epi PUBLIC EPI_EXTRA_CHECKS)
endif()

target_compile_options(epi PRIVATE ${EDGE_WARNING_LEVEL})

##########################################
# dr_libs
##########################################

add_library(dr_libs dr_libs.c)

target_include_directories(dr_libs PUBLIC ./)

target_compile_definitions(dr_libs PUBLIC
  DR_WAV_NO_STDIO
  DR_FLAC_NO_STDIO
  DR_MP3_NO_STDIO
)

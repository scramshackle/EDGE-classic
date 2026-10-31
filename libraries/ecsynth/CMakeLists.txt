##########################################
# ecsynth
##########################################

add_library(
  ecsynth
  ecsynth.c
  )

target_include_directories(ecsynth PUBLIC ./)
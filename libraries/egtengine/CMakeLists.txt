##########################################
# edgetracker
##########################################

add_library(
  edgetracker
  egtlib.c
  )

target_include_directories(edgetracker PUBLIC ./)
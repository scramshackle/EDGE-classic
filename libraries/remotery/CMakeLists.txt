##########################################
# remotery
##########################################

if (EDGE_PROFILING)
  find_package(Threads REQUIRED)

  add_library(remotery Remotery.c)

  target_include_directories(remotery PUBLIC ./)

  target_link_libraries(remotery PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

  if (WIN32)
    target_link_libraries(remotery PUBLIC ws2_32 winmm)
  endif()
endif()

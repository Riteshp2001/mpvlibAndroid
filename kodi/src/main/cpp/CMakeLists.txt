cmake_minimum_required(VERSION 3.22.1)
project(kodiengine C)

# Starts libkodi.so from a Service (see kodi_engine.c). Plain C, so it does not depend on the
# C++ runtime that Kodi's libraries share with the host application.
add_library(kodiengine SHARED kodi_engine.c)
target_compile_options(kodiengine PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_options(kodiengine PRIVATE "-Wl,-z,max-page-size=16384")
target_link_libraries(kodiengine android log dl)

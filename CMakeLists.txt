cmake_minimum_required(VERSION 3.15)
project(zapp)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL2 REQUIRED)

add_executable(zapp
     main.cpp

     includes/imgui/imgui.cpp
     includes/imgui/imgui_draw.cpp
     includes/imgui/imgui_widgets.cpp
     includes/imgui/imgui_tables.cpp

     includes/imgui/backends/imgui_impl_sdl2.cpp
     includes/imgui/backends/imgui_impl_sdlrenderer2.cpp

     includes/tinyfiledialogs/tinyfiledialogs.c
 )

target_include_directories(zapp PRIVATE
     includes/imgui
     includes/imgui/backends
     SDL2_INCLUDE_DIRS
     includes/minimp3
     includes/json
     includes/tinyfiledialogs
     includes/zapp
 )

target_link_libraries(zapp PRIVATE SDL2::SDL2)

cmake_minimum_required(VERSION 3.15)
project(zapp)

set(CMAKE_CXX_STANDARD 17)

find_package(SDL2 REQUIRED)

add_executable(mp3_player
     main.cpp

     imgui/imgui.cpp
     imgui/imgui_draw.cpp
     imgui/imgui_widgets.cpp
     imgui/imgui_tables.cpp

     imgui/backends/imgui_impl_sdl2.cpp
     imgui/backends/imgui_impl_sdlrenderer2.cpp
 )

target_include_directories(mp3_player PRIVATE
     imgui
     imgui/backends
     SDL2_INCLUDE_DIRS
     minimp3
 )

target_link_libraries(mp3_player PRIVATE SDL2::SDL2)

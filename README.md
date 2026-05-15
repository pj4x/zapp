# Zapp MP3 Player

A minimal desktop MP3 player built with C++, SDL2, and Dear ImGui.

## Features

* MP3 playback
* Playlist management
* Local music library support
* Folder selection dialog
* JSON-based song and playlist storage
* SDL2 rendering and audio backend
* Dear ImGui user interface

## Dependencies

* C++17
* CMake 3.15+
* SDL2

Included third-party libraries:

* Dear ImGui
* minimp3
* nlohmann/json
* tinyfiledialogs

## Build

```bash
mkdir build
cd build
cmake ..
make
```

## Run

```bash
./zapp
```

## Project Structure

```text
.
├── main.cpp
├── CMakeLists.txt
├── includes/
│   ├── imgui/
│   ├── json/
│   ├── minimp3/
│   ├── tinyfiledialogs/
│   └── zapp/
└── build/
```

## Notes

* Song and playlist data are stored locally.
* The application currently targets desktop platforms supported by SDL2.
* MP3 decoding is handled through minimp3.

// main.cpp
//
// Simple MP3 Player
// Uses:
//   - minimp3
//   - SDL2
//   - Dear ImGui
//
// Features:
//   - Open MP3 file
//   - Play / Pause
//   - Stop
//   - Seek bar
//   - Progress display
//
// ------------------------------------------------------------
// BUILD NOTES
// ------------------------------------------------------------
//
// Required libraries:
//
// SDL2:
//   https://www.libsdl.org
//
// Dear ImGui:
//   https://github.com/ocornut/imgui
//
// minimp3:
//   https://github.com/lieff/minimp3
//
// You need these ImGui files:
//
//   imgui.cpp
//   imgui_draw.cpp
//   imgui_widgets.cpp
//   imgui_tables.cpp
//   imgui_impl_sdl2.cpp
//   imgui_impl_sdlrenderer2.cpp
//
// ------------------------------------------------------------
// EXAMPLE CMAKE
// ------------------------------------------------------------
//
// cmake_minimum_required(VERSION 3.15)
// project(mp3_player)
//
// set(CMAKE_CXX_STANDARD 17)
//
// find_package(SDL2 REQUIRED)
//
// add_executable(mp3_player
//     main.cpp
//
//     imgui/imgui.cpp
//     imgui/imgui_draw.cpp
//     imgui/imgui_widgets.cpp
//     imgui/imgui_tables.cpp
//
//     imgui/backends/imgui_impl_sdl2.cpp
//     imgui/backends/imgui_impl_sdlrenderer2.cpp
// )
//
// target_include_directories(mp3_player PRIVATE
//     imgui
//     imgui/backends
//     SDL2_INCLUDE_DIRS
//     minimp3
// )
//
// target_link_libraries(mp3_player PRIVATE SDL2::SDL2)
//
// ------------------------------------------------------------

#include <SDL.h>
#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

// ------------------------------------------------------------
// minimp3
// ------------------------------------------------------------

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3_ex.h"

// ------------------------------------------------------------
// Audio State
// ------------------------------------------------------------

struct AudioState
{
    std::vector<float> pcm;

    int channels = 0;
    int sampleRate = 0;

    std::atomic<size_t> currentFrame = 0;

    size_t totalFrames = 0;

    std::atomic<bool> playing = false;

    std::mutex mutex;
};

AudioState gAudio;

// ------------------------------------------------------------
// Audio Callback
// ------------------------------------------------------------

void audio_callback(void* userdata, Uint8* stream, int len)
{
    AudioState* audio = (AudioState*)userdata;

    SDL_memset(stream, 0, len);

    if (!audio->playing)
        return;

    float* out = (float*)stream;

    int floatCount = len / sizeof(float);

    size_t currentSample =
        audio->currentFrame * audio->channels;

    size_t totalSamples =
        audio->totalFrames * audio->channels;

    for (int i = 0; i < floatCount; ++i)
    {
        if (currentSample >= totalSamples)
        {
            audio->playing = false;
            break;
        }

        out[i] = audio->pcm[currentSample++];
    }

    audio->currentFrame =
        currentSample / audio->channels;
}

// ------------------------------------------------------------
// Load MP3
// ------------------------------------------------------------

bool load_mp3(const std::string& path)
{
    mp3dec_ex_t dec;

    if (mp3dec_ex_open(
        &dec,
        path.c_str(),
        MP3D_SEEK_TO_SAMPLE))
    {
        std::cerr << "Failed to open MP3\n";
        return false;
    }

    gAudio.channels = dec.info.channels;
    gAudio.sampleRate = dec.info.hz;

    gAudio.totalFrames = dec.samples / gAudio.channels;

    gAudio.pcm.resize(dec.samples);

    size_t read = mp3dec_ex_read(
        &dec,
        gAudio.pcm.data(),
        dec.samples);

    if (read == 0)
    {
        std::cerr << "Failed to decode MP3\n";
        mp3dec_ex_close(&dec);
        return false;
    }

    mp3dec_ex_close(&dec);

    gAudio.currentFrame = 0;

    std::cout << "Loaded MP3\n";
    std::cout << "Channels: " << gAudio.channels << "\n";
    std::cout << "Sample Rate: " << gAudio.sampleRate << "\n";

    return true;
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cout << "Usage:\n";
        std::cout << "mp3_player song.mp3\n";
        return 0;
    }

    if (!load_mp3(argv[1]))
    {
        return -1;
    }

    // --------------------------------------------------------
    // SDL Init
    // --------------------------------------------------------

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0)
    {
        std::cerr << "SDL init failed\n";
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "MP3 Player",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        800,
        300,
        SDL_WINDOW_SHOWN);

    SDL_Renderer* renderer =
        SDL_CreateRenderer(
            window,
            -1,
            SDL_RENDERER_ACCELERATED);

    // --------------------------------------------------------
    // Audio Setup
    // --------------------------------------------------------

    SDL_AudioSpec spec{};
    SDL_AudioSpec obtained{};

    spec.freq = gAudio.sampleRate;
    spec.format = AUDIO_F32;
    spec.channels = (Uint8)gAudio.channels;
    spec.samples = 4096;
    spec.callback = audio_callback;
    spec.userdata = &gAudio;

    if (SDL_OpenAudio(&spec, &obtained) < 0)
    {
        std::cerr << "Failed to open audio\n";
        return -1;
    }

    SDL_PauseAudio(0);

    // --------------------------------------------------------
    // ImGui Setup
    // --------------------------------------------------------

    IMGUI_CHECKVERSION();

    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    bool running = true;

    // --------------------------------------------------------
    // Main Loop
    // --------------------------------------------------------

    while (running)
    {
        SDL_Event event;

        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);

            if (event.type == SDL_QUIT)
            {
                running = false;
            }
        }

        // ----------------------------------------------------
        // ImGui Frame
        // ----------------------------------------------------

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();

        ImGui::NewFrame();

        ImGui::Begin("MP3 Player");

        // ----------------------------------------------------
        // Playback Controls
        // ----------------------------------------------------

        if (ImGui::Button("Play"))
        {
            gAudio.playing = true;
        }

        ImGui::SameLine();

        if (ImGui::Button("Pause"))
        {
            gAudio.playing = false;
        }

        ImGui::SameLine();

        if (ImGui::Button("Stop"))
        {
            gAudio.playing = false;
            gAudio.currentFrame = 0;
        }

        // ----------------------------------------------------
        // Time Info
        // ----------------------------------------------------

        float currentSeconds =
            (float)gAudio.currentFrame /
            (float)gAudio.sampleRate;

        float totalSeconds =
            (float)gAudio.totalFrames /
            (float)gAudio.sampleRate;

        ImGui::Text(
            "Time: %.2f / %.2f sec",
            currentSeconds,
            totalSeconds);

        // ----------------------------------------------------
        // Seek Bar
        // ----------------------------------------------------

        float progress =
            (float)gAudio.currentFrame /
            (float)gAudio.totalFrames;

        if (ImGui::SliderFloat(
            "Seek",
            &progress,
            0.0f,
            1.0f))
        {
            size_t newFrame =
                (size_t)(progress * gAudio.totalFrames);

            gAudio.currentFrame = newFrame;
        }

        // ----------------------------------------------------
        // Audio Info
        // ----------------------------------------------------

        ImGui::Separator();

        ImGui::Text("Sample Rate: %d", gAudio.sampleRate);
        ImGui::Text("Channels: %d", gAudio.channels);
        ImGui::Text("Frames: %zu", gAudio.totalFrames);

        ImGui::End();

        // ----------------------------------------------------
        // Render
        // ----------------------------------------------------

        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);

        ImGui_ImplSDLRenderer2_RenderDrawData(
            ImGui::GetDrawData());

        SDL_RenderPresent(renderer);
    }

    // --------------------------------------------------------
    // Cleanup
    // --------------------------------------------------------

    SDL_CloseAudio();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();

    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();

    return 0;
}

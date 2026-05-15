#include <SDL.h>
#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstring>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

// JSON library (single header - https://github.com/nlohmann/json)
#include <nlohmann/json.hpp>

// Tiny file dialogs for folder selection
#define TINYFILEDIALOGS_IMPLEMENTATION
#include "tinyfiledialogs.h"

// ------------------------------------------------------------
// minimp3
// ------------------------------------------------------------

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3_ex.h"

// ------------------------------------------------------------
// Song Info Structure
// ------------------------------------------------------------

struct SongInfo
{
    std::string path;
    std::string title;
    std::string artist;
};

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
std::vector<SongInfo> g_songList;
SongInfo g_nowPlaying;
std::mutex g_songMutex;
std::atomic<bool> g_scanning = false;
std::string g_scanStatus = "";

// ------------------------------------------------------------
// MP3 Metadata Extraction (ID3v1)
// ------------------------------------------------------------

bool read_id3v1(const std::string& path, std::string& title, std::string& artist)
{
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;

    // Seek to 128 bytes from end
    fseek(file, -128, SEEK_END);

    char tag[4] = {0};
    fread(tag, 1, 3, file);
    tag[3] = '\0';

    if (strcmp(tag, "TAG") != 0)
    {
        fclose(file);
        return false;
    }

    char titleBuf[31] = {0};
    char artistBuf[31] = {0};

    fread(titleBuf, 1, 30, file);
    fread(artistBuf, 1, 30, file);

    fclose(file);

    // Trim trailing spaces and nulls
    title = std::string(titleBuf);
    artist = std::string(artistBuf);

    auto trim = [](std::string& s) {
        s.erase(s.find_last_not_of(" \0") + 1);
        s.erase(0, s.find_first_not_of(" \0"));
    };

    trim(title);
    trim(artist);

    return true;
}

std::string get_filename_without_ext(const std::string& path)
{
    std::filesystem::path p(path);
    return p.stem().string();
}

void extract_metadata(const std::string& path, std::string& title, std::string& artist)
{
    if (!read_id3v1(path, title, artist))
    {
        title = get_filename_without_ext(path);
        artist = "Unknown Artist";
    }

    if (title.empty()) title = get_filename_without_ext(path);
    if (artist.empty()) artist = "Unknown Artist";
}

// ------------------------------------------------------------
// JSON Database Functions
// ------------------------------------------------------------

const std::string DB_FILENAME = "songs_db.json";

void save_database(const std::vector<SongInfo>& songs)
{
    nlohmann::json j;
    for (const auto& song : songs)
    {
        nlohmann::json songJson;
        songJson["path"] = song.path;
        songJson["title"] = song.title;
        songJson["artist"] = song.artist;
        j.push_back(songJson);
    }

    std::ofstream file(DB_FILENAME);
    if (file.is_open())
    {
        file << j.dump(4);
    }
}

std::vector<SongInfo> load_database()
{
    std::vector<SongInfo> songs;

    std::ifstream file(DB_FILENAME);
    if (!file.is_open()) return songs;

    nlohmann::json j;
    file >> j;

    for (const auto& item : j)
    {
        SongInfo song;
        song.path = item.value("path", "");
        song.title = item.value("title", "");
        song.artist = item.value("artist", "");
        if (!song.path.empty())
        {
            songs.push_back(song);
        }
    }

    return songs;
}

// ------------------------------------------------------------
// Folder Scanning (Worker Thread)
// ------------------------------------------------------------

void scan_folder_worker(const std::string& folderPath)
{
    g_scanStatus = "Scanning folder: " + folderPath;

    std::vector<SongInfo> newSongs;

    try
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(folderPath))
        {
            if (!g_scanning) break; // Allow cancellation

            if (entry.is_regular_file())
            {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                if (ext == ".mp3")
                {
                    SongInfo song;
                    song.path = entry.path().string();
                    extract_metadata(song.path, song.title, song.artist);
                    newSongs.push_back(song);

                    g_scanStatus = "Found: " + song.title;
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        g_scanStatus = "Error: " + std::string(e.what());
    }

    if (g_scanning)
    {
        // Save to database
        save_database(newSongs);

        // Update global song list
        {
            std::lock_guard<std::mutex> lock(g_songMutex);
            g_songList = std::move(newSongs);
        }

        g_scanStatus = "Scan complete! Found " + std::to_string(g_songList.size()) + " songs";
    }
    else
    {
        g_scanStatus = "Scan cancelled";
    }

    g_scanning = false;
}

void start_folder_scan(const std::string& folderPath)
{
    if (g_scanning) return;

    g_scanning = true;
    std::thread scanThread(scan_folder_worker, folderPath);
    scanThread.detach();
}

// ------------------------------------------------------------
// Load MP3 with thread safety
// ------------------------------------------------------------

bool load_mp3(const std::string& path)
{
    mp3dec_ex_t dec;

    if (mp3dec_ex_open(&dec, path.c_str(), MP3D_SEEK_TO_SAMPLE))
    {
        std::cerr << "Failed to open MP3: " << path << "\n";
        return false;
    }

    // Prepare new audio data
    AudioState newAudio;
    newAudio.channels = dec.info.channels;
    newAudio.sampleRate = dec.info.hz;
    newAudio.totalFrames = dec.samples / newAudio.channels;
    newAudio.pcm.resize(dec.samples);
    newAudio.playing = false;
    newAudio.currentFrame = 0;

    size_t read = mp3dec_ex_read(&dec, newAudio.pcm.data(), dec.samples);

    mp3dec_ex_close(&dec);

    if (read == 0)
    {
        std::cerr << "Failed to decode MP3\n";
        return false;
    }

    // Lock audio callback while swapping data
    SDL_LockAudio();

    // Swap with global audio state
    {
        std::lock_guard<std::mutex> lock(gAudio.mutex);
        gAudio.pcm.swap(newAudio.pcm);
        gAudio.channels = newAudio.channels;
        gAudio.sampleRate = newAudio.sampleRate;
        gAudio.totalFrames = newAudio.totalFrames;
        gAudio.currentFrame = 0;
        gAudio.playing = true; // Auto-start playback
    }

    SDL_UnlockAudio();

    std::cout << "Loaded MP3: " << path << "\n";
    std::cout << "Channels: " << gAudio.channels << "\n";
    std::cout << "Sample Rate: " << gAudio.sampleRate << "\n";

    return true;
}

// ------------------------------------------------------------
// Audio Callback (thread-safe)
// ------------------------------------------------------------

void audio_callback(void* userdata, Uint8* stream, int len)
{
    AudioState* audio = (AudioState*)userdata;

    SDL_memset(stream, 0, len);

    if (!audio->playing)
        return;

    std::lock_guard<std::mutex> lock(audio->mutex);

    float* out = (float*)stream;
    int floatCount = len / sizeof(float);

    size_t currentSample = audio->currentFrame * audio->channels;
    size_t totalSamples = audio->totalFrames * audio->channels;

    for (int i = 0; i < floatCount; ++i)
    {
        if (currentSample >= totalSamples)
        {
            audio->playing = false;
            break;
        }

        out[i] = audio->pcm[currentSample++];
    }

    audio->currentFrame = currentSample / audio->channels;
}

// ------------------------------------------------------------
// Dear ImGui Helpers
// ------------------------------------------------------------

void SetInvertedBlackAndWhiteTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // White background, black text (like paper)
    colors[ImGuiCol_Text] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
}

void SetBlackAndWhiteTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Base colors
    colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.65f, 0.65f, 0.65f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.70f, 0.70f, 0.70f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.25f, 0.25f, 0.25f, 1.00f);
    colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.50f, 0.50f, 0.50f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.70f, 0.70f, 0.70f, 0.90f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.70f, 0.70f, 0.70f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.10f, 0.10f, 0.10f, 0.50f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.10f, 0.10f, 0.10f, 0.50f);

    // Style settings
    style.WindowRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
}

void SetHighContrastBlackAndWhiteTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Pure black and white theme
    colors[ImGuiCol_Text] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.80f, 0.80f, 0.80f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);

    // Sharp corners for minimalist look
    style.WindowRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.TabRounding = 0.0f;

    // Increase contrast with thicker borders
    style.WindowBorderSize = 2.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 2.0f;
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------

int main(int argc, char** argv)
{
    // Load song database
    g_songList = load_database();
    std::cout << "Loaded " << g_songList.size() << " songs from database\n";

    // --------------------------------------------------------
    // SDL Init
    // --------------------------------------------------------

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0)
    {
        std::cerr << "SDL init failed\n";
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "zapp",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1000,
        600,
        SDL_WINDOW_SHOWN
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    // --------------------------------------------------------
    // Audio Setup
    // --------------------------------------------------------

    // Use default audio spec initially (will be updated when loading a song)
    SDL_AudioSpec spec{};
    SDL_AudioSpec obtained{};

    spec.freq = 44100;
    spec.format = AUDIO_F32;
    spec.channels = 2;
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
    SetBlackAndWhiteTheme();

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

        // ----------------------------------------------------
        // Player Window
        // ----------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(400, 250), ImGuiCond_Appearing);

        ImGuiWindowFlags playerFlags =
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse;

        ImGui::Begin("player", nullptr, playerFlags);

        if(!g_nowPlaying.path.empty()){
            ImGui::TextColored(ImVec4(0,1,0,1), "now playing: %s by %s", g_nowPlaying.title.c_str(), g_nowPlaying.artist.c_str());
        }
        ImGui::Separator();

        if (ImGui::Button("play/pause"))
        {
            gAudio.playing = !gAudio.playing;
        }

        ImGui::SameLine();

        if (ImGui::Button("stop"))
        {
            SDL_LockAudio();
            {
                std::lock_guard<std::mutex> lock(gAudio.mutex);
                gAudio.playing = false;
                gAudio.currentFrame = 0;
            }
            SDL_UnlockAudio();
        }

        ImGui::Separator();

        // Time Info
        float currentSeconds = 0.0f, totalSeconds = 0.0f;
        {
            std::lock_guard<std::mutex> lock(gAudio.mutex);
            if (gAudio.sampleRate > 0)
            {
                currentSeconds = (float)gAudio.currentFrame / (float)gAudio.sampleRate;
                totalSeconds = (float)gAudio.totalFrames / (float)gAudio.sampleRate;
            }
        }

        ImGui::Text("%.2f / %.2f sec", currentSeconds, totalSeconds);

        // Seek Bar
        float progress = 0.0f;
        {
            std::lock_guard<std::mutex> lock(gAudio.mutex);
            if (gAudio.totalFrames > 0)
                progress = (float)gAudio.currentFrame / (float)gAudio.totalFrames;
        }

        if (ImGui::SliderFloat("seek", &progress, 0.0f, 1.0f))
        {
            SDL_LockAudio();
            {
                std::lock_guard<std::mutex> lock(gAudio.mutex);
                size_t newFrame = (size_t)(progress * gAudio.totalFrames);
                gAudio.currentFrame = newFrame;
            }
            SDL_UnlockAudio();
        }

        // Audio Info
        ImGui::Separator();
        {
            std::lock_guard<std::mutex> lock(gAudio.mutex);
            ImGui::Text("Sample Rate: %d Hz", gAudio.sampleRate);
            ImGui::Text("Channels: %d", gAudio.channels);
            ImGui::Text("Total Frames: %zu", gAudio.totalFrames);
        }

        ImGui::End();

        // ----------------------------------------------------
        // Songs List Window
        // ----------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(420, 10), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(570, 580), ImGuiCond_Appearing);

        ImGuiWindowFlags libraryFlags =
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize;

        ImGui::Begin("library", nullptr, libraryFlags);

        if (ImGui::Button("open folder"))
        {
            if (!g_scanning)
            {
                const char* folder = tinyfd_selectFolderDialog("Select Music Folder", "");
                if (folder)
                {
                    start_folder_scan(folder);
                }
            }
        }


        ImGui::SameLine();

        if (ImGui::Button("refresh"))
        {
            std::lock_guard<std::mutex> lock(g_songMutex);
            g_songList = load_database();
        }

        ImGui::SameLine();
        ImGui::Text("total songs: %zu", g_songList.size());

        ImGui::SameLine();

        if (g_scanning)
        {
            ImGui::TextColored(ImVec4(1,1,0,1), "scanning...");
            ImGui::SameLine();
            ImGui::Text("%s", g_scanStatus.c_str());
        }

        ImGui::Separator();

        // Song list with filtering
        static char filter[256] = "";
        ImGui::InputText("filter", filter, sizeof(filter));

        ImGui::BeginChild("SongList", ImVec2(0, 0), true);

        {
            std::lock_guard<std::mutex> lock(g_songMutex);

            for (size_t i = 0; i < g_songList.size(); ++i)
            {
                const auto& song = g_songList[i];

                // Apply filter
                std::string filterLower = filter;
                std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

                std::string titleLower = song.title;
                std::string artistLower = song.artist;
                std::transform(titleLower.begin(), titleLower.end(), titleLower.begin(), ::tolower);
                std::transform(artistLower.begin(), artistLower.end(), artistLower.begin(), ::tolower);

                if (!filterLower.empty() &&
                    titleLower.find(filterLower) == std::string::npos &&
                    artistLower.find(filterLower) == std::string::npos)
                {
                    continue;
                }

                std::string displayText = song.title + " - " + song.artist;

                if (ImGui::Selectable(displayText.c_str()))
                {
                    // Load selected song
                    if (load_mp3(song.path))
                    {
                        std::cout << "Now playing: " << song.title << "\n";
                        g_nowPlaying = song;
                    }
                }
            }
        }

        ImGui::EndChild();
        ImGui::End();

        // ----------------------------------------------------
        // Render
        // ----------------------------------------------------

        ImGui::Render();

        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);

        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);

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

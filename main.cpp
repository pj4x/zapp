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
#include <set>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

// JSON library (single header - https://github.com/nlohmann/json)
#include <nlohmann/json.hpp>

// Tiny file dialogs for folder selection
#define TINYFILEDIALOGS_IMPLEMENTATION
#include "tinyfiledialogs.h"

// minimp3
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3_ex.h"

#include "definitions.h"
#include "globals.h"
#include "helpers.h"
#include "mp3.h"
#include "database.h"
#include "folder.h"
#include "audio.h"
#include "helperImgui.h"

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------

int main(int argc, char** argv)
{
    // Load song database
    g_allSongs = load_songs_database();
    std::cout << "Loaded " << g_allSongs.size() << " songs from database\n";

    // Load playlists
    g_playlists = load_playlists_database();
    ensure_library_playlist(); // Make sure library exists and is up-to-date

    // Set initial current playlist to "library"
    for (size_t i = 0; i < g_playlists.size(); ++i)
    {
        if (g_playlists[i].name == "library")
        {
            g_currentPlaylistIndex = i;
            break;
        }
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
    spec.userdata = &gPlayback;

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

    // Popup state for "Add from Library"
    static char addFilter[256] = "";

    // Playlist creation popup
    static char newPlaylistName[128] = "";
    static bool showNewPlaylistPopup = false;

    // Playlist deletion popup
    static bool showDeletePlaylistPopup = false;

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

        // preload next song to cache
        if(gAudio.playing && !g_hasPreloadedForCurrentSong){
            preload_next_song_background();
            g_hasPreloadedForCurrentSong = true;
        }

        // ----------------------------------------------------
        // Auto-play next song when current finishes
        // ----------------------------------------------------

        if (g_requestNextSong)
        {
            g_requestNextSong = false;
            g_hasPreloadedForCurrentSong = false;

            if (g_currentPlaylistIndex >= 0 && g_currentPlaylistIndex < (int)g_playlists.size())
            {
                const auto& currentPlaylist = g_playlists[g_currentPlaylistIndex];
                if (!currentPlaylist.songIds.empty())
                {
                    int currentId = getCurrentPlayingId();
                    if (currentId != -1) {
                        int nextId = get_next_song_in_playlist(currentId);
                        if (nextId != -1) play_song_by_id(nextId);
                    }
                }
            }
        }

        // Increment play count
        if (g_incrementPlayCount)
        {
            g_incrementPlayCount = false;
            increment_play_count(g_songToIncrement);
            g_songToIncrement = -1;
        }

        // ----------------------------------------------------
        // Player Window (Top Left)
        // ----------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(400, 250), ImGuiCond_Appearing);

        ImGuiWindowFlags playerFlags =
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse;

        ImGui::Begin("player", nullptr, playerFlags);

        // now playing display
        SongInfo currentSong = getCurrentPlayingMetadata();
        if(!currentSong.path.empty()){
            ImGui::TextColored(ImVec4(0,1,0,1), "now playing: %s by %s",
                               currentSong.title.c_str(),
                               currentSong.artist.c_str());
        }
        ImGui::Separator();

        if (ImGui::Button("<"))
        {
            if (g_currentPlaylistIndex >= 0 && g_currentPlaylistIndex < (int)g_playlists.size())
            {
                const auto& currentPlaylist = g_playlists[g_currentPlaylistIndex];
                if (!currentPlaylist.songIds.empty())
                {
                    int currentId = getCurrentPlayingId();
                    if (currentId != -1) {
                        int prevId = get_prev_song_in_playlist(currentId);
                        if (prevId != -1) play_song_by_id(prevId);
                    }
                }
            }
        }

        ImGui::SameLine();

        if (ImGui::Button("play/pause"))
        {
            gPlayback.playing = !gPlayback.playing;
        }

        ImGui::SameLine();

        if (ImGui::Button(">"))
        {
            if (g_currentPlaylistIndex >= 0 && g_currentPlaylistIndex < (int)g_playlists.size())
            {
                const auto& currentPlaylist = g_playlists[g_currentPlaylistIndex];
                if (!currentPlaylist.songIds.empty())
                {
                    int currentId = getCurrentPlayingId();
                    if (currentId != -1) {
                        int nextId = get_next_song_in_playlist(currentId);
                        if (nextId != -1) play_song_by_id(nextId);
                    }
                }
            }
        }

        ImGui::Separator();

        // Time Info
        float currentSeconds = 0.0f, totalSeconds = 0.0f;
        {
            std::lock_guard<std::mutex> lock(gPlayback.mutex);
            if (gPlayback.sampleRate > 0)
            {
                currentSeconds = (float)gPlayback.currentFrame / (float)gPlayback.sampleRate;
                totalSeconds = (float)gPlayback.totalFrames / (float)gPlayback.sampleRate;
            }
        }

        ImGui::Text("%.2f / %.2f sec", currentSeconds, totalSeconds);

        // Seek Bar
        float progress = 0.0f;
        {
            std::lock_guard<std::mutex> lock(gPlayback.mutex);
            if (gPlayback.totalFrames > 0)
                progress = (float)gPlayback.currentFrame / (float)gPlayback.totalFrames;
        }

        if (ImGui::SliderFloat("seek", &progress, 0.0f, 1.0f))
        {
            SDL_LockAudio();
            {
                std::lock_guard<std::mutex> lock(gPlayback.mutex);
                size_t newFrame = (size_t)(progress * gPlayback.totalFrames);
                gPlayback.currentFrame = newFrame;
            }
            SDL_UnlockAudio();
        }

        // Audio Info
        ImGui::Separator();
        {
            std::lock_guard<std::mutex> lock(gPlayback.mutex);
            ImGui::Text("Sample Rate: %d Hz", gPlayback.sampleRate);
            ImGui::Text("Channels: %d", gPlayback.channels);
            ImGui::Text("Total Frames: %zu", gPlayback.totalFrames);
        }

        ImGui::End();

        // ----------------------------------------------------
        // Playlists Window (Bottom Left)
        // ----------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(10, 270), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(400, 320), ImGuiCond_Appearing);

        ImGui::Begin("playlists", nullptr);

        if (ImGui::Button("new"))
        {
            showNewPlaylistPopup = true;
            memset(newPlaylistName, 0, sizeof(newPlaylistName));
        }

        ImGui::SameLine();
        if (ImGui::Button("delete"))
        {
            if(g_playlists[g_currentPlaylistIndex].name != "library"){
                showDeletePlaylistPopup = true;
            }
        }

        ImGui::Separator();

        ImGui::BeginChild("PlaylistList", ImVec2(0, 0), true);

        for (size_t i = 0; i < g_playlists.size(); ++i)
        {
            const auto& playlist = g_playlists[i];
            bool isSelected = (g_currentPlaylistIndex == (int)i);

            std::string displayName = playlist.name;

            if (ImGui::Selectable(displayName.c_str(), isSelected))
            {
                g_currentPlaylistIndex = i;
            }

        }

        ImGui::EndChild();
        ImGui::End();

        // New Playlist Popup
        if (showNewPlaylistPopup)
        {
            ImGui::OpenPopup("create playlist");
            showNewPlaylistPopup = false;
        }

        if (ImGui::BeginPopupModal("create playlist", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::InputText("name", newPlaylistName, sizeof(newPlaylistName));
            if (ImGui::Button("create"))
            {
                if (strlen(newPlaylistName) > 0 && create_playlist(newPlaylistName))
                {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Delete Playlist Popup
        if (showDeletePlaylistPopup)
        {
            ImGui::OpenPopup("delete playlist");
            showDeletePlaylistPopup = false;
        }

        if (ImGui::BeginPopupModal("delete playlist", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const auto& playlist = g_playlists[g_currentPlaylistIndex];
            ImGui::Text("delete playlist \"%s\" ?", playlist.name.c_str());
            if (ImGui::Button("delete"))
            {
                delete_playlist(playlist.name);
                // select library
                for (size_t j = 0; j < g_playlists.size(); ++j)
                {
                    if (g_playlists[j].name == "library")
                    {
                        g_currentPlaylistIndex = j;
                        break;
                    }
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ----------------------------------------------------
        // Current Playlist Window (Right)
        // ----------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(420, 10), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(570, 580), ImGuiCond_Appearing);

        ImGuiWindowFlags playlistWindowFlags =
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize;

        std::string playlistTitle = "";
        if (g_currentPlaylistIndex >= 0 && g_currentPlaylistIndex < (int)g_playlists.size())
            playlistTitle += g_playlists[g_currentPlaylistIndex].name;
        else
            playlistTitle += "none";

        ImGui::Begin(playlistTitle.c_str(), nullptr, playlistWindowFlags);

        // Toolbar
        if(g_playlists[g_currentPlaylistIndex].name == "library"){
            if (ImGui::Button("open folder"))
            {
                if (!g_scanning)
                {
                    const char* folder = tinyfd_selectFolderDialog("select music folder", "");
                    if (folder)
                    {
                        start_folder_scan(folder);
                    }
                }
            }

            ImGui::SameLine();
        }
        if (ImGui::Button("add songs") && g_playlists[g_currentPlaylistIndex].name != "library")
        {
            g_showAddFromLibraryPopup = true;
            memset(addFilter, 0, sizeof(addFilter));
        }

        ImGui::SameLine();
        ImGui::Text("total songs: %zu", g_allSongs.size());

        ImGui::SameLine();

        if (g_scanning)
        {
            ImGui::TextColored(ImVec4(1,1,0,1), "scanning...");
            ImGui::SameLine();
            ImGui::Text("%s", g_scanStatus.c_str());
        }

        ImGui::Separator();

        // Song filter for current playlist
        static char playlistFilter[256] = "";
        ImGui::InputText("filter", playlistFilter, sizeof(playlistFilter));

        ImGui::BeginChild("PlaylistSongs", ImVec2(0, 0), true);

        if (g_currentPlaylistIndex >= 0 && g_currentPlaylistIndex < (int)g_playlists.size())
        {
            std::vector<SongInfo> playlistSongs = get_playlist_songs(g_playlists[g_currentPlaylistIndex]);

            for (const auto& song : playlistSongs)
            {
                // Apply filter
                std::string filterLower = playlistFilter;
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

                /// Push ID
                ImGui::PushID(song.id);

                // Calculate available width (reserve space for + button)
                float availableWidth = ImGui::GetContentRegionAvail().x - 50;

                // Create a child area for this song to handle layout automatically
                ImGui::BeginGroup();

                // Create selectable that wraps with custom height
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.2f, 0.2f, 0.2f, 0.3f));

                // Draw title with wrapping
                ImGui::SetWindowFontScale(1.2f);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + availableWidth);
                ImGui::TextWrapped("%s", song.title.c_str());
                ImGui::PopTextWrapPos();
                ImGui::SetWindowFontScale(1.0f);

                // Get the rect of the title text
                ImVec2 titleMin = ImGui::GetItemRectMin();
                ImVec2 titleMax = ImGui::GetItemRectMax();

                // Create a selectable that covers the title area
                ImGui::SetCursorScreenPos(titleMin);
                if (ImGui::Selectable("##title_select", false, ImGuiSelectableFlags_None, ImVec2(titleMax.x - titleMin.x, titleMax.y - titleMin.y)))
                {
                    // safety check
                    if (song.id >= 0) {
                        if (play_song_by_id(song.id))
                        {
                            std::cout << "Now playing: " << song.title << "\n";
                        }
                        else
                        {
                            std::cerr << "Failed to play song: " << song.title << std::endl;
                        }
                    }
                }
                ImGui::PopStyleColor(3);

                // Artist text
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::TextWrapped("%s", song.artist.c_str());
                ImGui::PopStyleColor();

                ImGui::EndGroup();

                // Option button
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 50);
                if (ImGui::Button("▼"))
                {
                    g_selectedSongForOptions = song;
                    g_showSongOptionsPopup = true;
                }

                ImGui::PopID();
                ImGui::Spacing();
                ImGui::Separator();
            }
        }

        ImGui::EndChild();
        ImGui::End();

        // ----------------------------------------------------
        // Add from Library Popup
        // ----------------------------------------------------
        if (g_showAddFromLibraryPopup)
        {
            ImGui::OpenPopup("add songs##popup");
            g_showAddFromLibraryPopup = false;
        }

        if (ImGui::BeginPopupModal("add songs##popup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::InputText("search", addFilter, sizeof(addFilter));

            ImGui::BeginChild("LibrarySongs", ImVec2(500, 300), true);

            // Take a snapshot of all songs under mutex protection
            std::vector<SongInfo> songsSnapshot;
            {
                std::lock_guard<std::mutex> lock(g_songMutex);
                songsSnapshot = g_allSongs; // Copy the vector
            }

            for (const auto& song : songsSnapshot)
            {
                std::string filterLower = addFilter;
                std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);
                std::string titleLower = song.title;
                std::string artistLower = song.artist;
                std::transform(titleLower.begin(), titleLower.end(), titleLower.begin(), ::tolower);
                std::transform(artistLower.begin(), artistLower.end(), artistLower.begin(), ::tolower);

                if (!filterLower.empty() &&
                    titleLower.find(filterLower) == std::string::npos &&
                    artistLower.find(filterLower) == std::string::npos)
                    continue;

                std::string displayText = song.title + " - " + song.artist;
                if (ImGui::Selectable(displayText.c_str()))
                {
                    // Add to current playlist
                    if (g_currentPlaylistIndex >= 0 && g_currentPlaylistIndex < (int)g_playlists.size())
                    {
                        const std::string& playlistName = g_playlists[g_currentPlaylistIndex].name;
                        add_song_to_playlist(playlistName, song.id);
                    }
                }
            }

            ImGui::EndChild();

            if (ImGui::Button("close"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ----------------------------------------------------
        // Song options Popup
        // ----------------------------------------------------
        if (g_showSongOptionsPopup)
        {
            ImGui::OpenPopup("song options##choice");
            g_showSongOptionsPopup = false;
        }

        if (ImGui::BeginPopupModal("song options##choice", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextColored(ImVec4(0,1,0,1), "%s", g_selectedSongForOptions.title.c_str());
            ImGui::Text("play count: %d", g_selectedSongForOptions.playCount);

            ImGui::Separator();

            if (ImGui::Button("add to playlist"))
            {
                g_showAddToPlaylistPopup = true;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();

            if (ImGui::Button("remove from playlist"))
            {
                remove_song_from_playlist(g_playlists[g_currentPlaylistIndex].name, g_selectedSongForOptions.id);
                ImGui::CloseCurrentPopup();
            }

            ImGui::Separator();

            if (ImGui::Button("close"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ----------------------------------------------------
        // Add song to playlist popup
        // ----------------------------------------------------

        if (g_showAddToPlaylistPopup)
        {
            ImGui::OpenPopup("add to playlist##choice");
            g_showAddToPlaylistPopup = false;
        }

        if (ImGui::BeginPopupModal("add to playlist##choice", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            // Get song name for display
            std::string songName = "unknown";
            {
                std::lock_guard<std::mutex> lock(g_songMutex);
                auto it = std::find_if(g_allSongs.begin(), g_allSongs.end(),
                    [](const SongInfo& s) { return s.id == g_selectedSongForOptions.id; });
                if (it != g_allSongs.end())
                    songName = it->title + " by " + it->artist;
            }
            ImGui::TextColored(ImVec4(0,1,0,1), "%s", songName.c_str());

            ImGui::Separator();

            for (const auto& playlist : g_playlists)
            {
                // Skip library
                if(playlist.name != "library"){
                    if (ImGui::Selectable(playlist.name.c_str()))
                    {
                        if (add_song_to_playlist(playlist.name, g_selectedSongForOptions.id))
                        {
                            // Success
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::Spacing();
                }
            }

            ImGui::Separator();
            if (ImGui::Button("cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

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

#include <SDL.h>
#include <iostream>
#include <ostream>
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
#include "includes/zapp/globals.h"
#include "includes/zapp/helperImgui.h"

#include <nlohmann/json.hpp>

#define TINYFILEDIALOGS_IMPLEMENTATION
#include "tinyfiledialogs.h"

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
#include "helperSdl.h"

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
    // set view Playlist index to playback playlist index
    g_viewPlaylistIndex = g_currentPlaylistIndex;

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
    // In main(), initialize with a default (will be reconfigured as needed)
    if (!ReconfigureAudio(44100, 2))
    {
        std::cerr << "Failed to initialize audio" << std::endl;
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

    // load user settings
    load_settings();

    bool running = true;

    // Popup state for "Add from Library"
    static char addFilter[256] = "";

    // Playlist creation popup
    static char newPlaylistName[128] = "";
    static bool showNewPlaylistPopup = false;

    // Playlist deletion popup
    static bool showDeletePlaylistPopup = false;

    // Add before the main loop
    int g_cachedViewPlaylistIndex = -1;
    std::vector<SongInfo> g_cachedPlaylistSongs;

    // --------------------------------------------------------
    // Main Loop
    // --------------------------------------------------------

    Uint32 lastRenderTime = 0;
    const Uint32 TARGET_FRAME_TIME = 16; // ~60 FPS max

    while (running)
    {
        Uint32 frameStart = SDL_GetTicks();

        SDL_Event event;
        bool hasEvents = false;
        while (SDL_PollEvent(&event))
        {
            hasEvents = true;
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }
        Uint32 windowFlags = SDL_GetWindowFlags(window);
        bool g_minimized = (windowFlags & SDL_WINDOW_MINIMIZED) != 0;

            // preload next song to cache
            if(gPlayback.playing && !g_hasPreloadedForCurrentSong){
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

            if (g_minimized){
                SDL_Delay(TARGET_FRAME_TIME*5);
                continue;
            }

            // ----------------------------------------------------
            // ImGui Frame
            // ----------------------------------------------------
            ImGuiIO& io = ImGui::GetIO();
            Uint32 currentTime = SDL_GetTicks();
            io.DeltaTime = (currentTime - lastRenderTime) / 1000.0f;
            lastRenderTime = currentTime;

            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();

            // ----------------------------------------------------
            // Player Window (Top Left)
            // ----------------------------------------------------
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Appearing);
            ImGui::SetNextWindowSize(ImVec2(400, 250), ImGuiCond_Appearing);

            ImGuiWindowFlags playerFlags =
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse;

            ImGui::PushStyleColor(ImGuiCol_Text, g_highlightColor);
            ImGui::Begin("player", nullptr, playerFlags);
            ImGui::PopStyleColor();

            // now playing display
            SongInfo currentSong = getCurrentPlayingMetadata();
            if(!currentSong.path.empty()){
                ImGui::TextColored(g_highlightColor, "%s by %s",
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
                if (gPlayback.mp3SampleRate > 0 && gPlayback.outputSampleRate > 0)
                {
                    // Current time uses output sample rate (matches actual playback speed)
                    currentSeconds = (float)gPlayback.currentFrame / (float)gPlayback.outputSampleRate;
                    // Total time uses MP3 sample rate (true duration)
                    totalSeconds = (float)gPlayback.totalFrames / (float)gPlayback.mp3SampleRate;
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

            if (ImGui::SliderFloat("##seek", &progress, 0.0f, 1.0f))
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
                ImGui::Text("Sample Rate: %d Hz", gPlayback.mp3SampleRate);
                ImGui::Text("Channels: %d", gPlayback.channels);
                ImGui::Text("Total Frames: %zu", gPlayback.totalFrames);
            }

            ImGui::End();

            // ----------------------------------------------------
            // Playlists Window (Bottom Left)
            // ----------------------------------------------------
            ImGui::SetNextWindowPos(ImVec2(10, 270), ImGuiCond_Appearing);
            ImGui::SetNextWindowSize(ImVec2(400, 320), ImGuiCond_Appearing);
            ImGuiWindowFlags playlistsFlags =
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse;

            ImGui::PushStyleColor(ImGuiCol_Text, g_highlightColor);
            ImGui::Begin("playlists", nullptr, playlistsFlags);
            ImGui::PopStyleColor();

            if (ImGui::Button("new"))
            {
                showNewPlaylistPopup = true;
                memset(newPlaylistName, 0, sizeof(newPlaylistName));
            }

            ImGui::SameLine();
            if(g_playlists[g_viewPlaylistIndex].name != "library"){
                if (ImGui::Button("delete"))
                {
                    showDeletePlaylistPopup = true;
                }
            }

            ImGui::Separator();

            ImGui::BeginChild("PlaylistList", ImVec2(0, 0), true);

            for (size_t i = 0; i < g_playlists.size(); ++i)
            {
                const auto& playlist = g_playlists[i];
                bool isSelected = (g_viewPlaylistIndex == (int)i);

                std::string displayName = playlist.name;
                if(displayName == g_playlists[g_currentPlaylistIndex].name){
                    ImGui::PushStyleColor(ImGuiCol_Text, g_highlightColor);
                }
                if (ImGui::Selectable(displayName.c_str(), isSelected))
                {
                    g_viewPlaylistIndex = i;
                }
                if(displayName == g_playlists[g_currentPlaylistIndex].name){
                    ImGui::PopStyleColor();
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
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoCollapse;

            std::string playlistTitle = "";
            if (g_viewPlaylistIndex >= 0 && g_viewPlaylistIndex < (int)g_playlists.size())
                playlistTitle += g_playlists[g_viewPlaylistIndex].name;
            else
                playlistTitle += "none";

            ImGui::PushStyleColor(ImGuiCol_Text, g_highlightColor);
            ImGui::Begin(playlistTitle.c_str(), nullptr, playlistWindowFlags);
            ImGui::PopStyleColor();

            // Toolbar
            if(g_playlists[g_viewPlaylistIndex].name == "library"){
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
            if(g_playlists[g_viewPlaylistIndex].name != "library"){
                if (ImGui::Button("add songs"))
                {
                    g_showAddFromLibraryPopup = true;
                    memset(addFilter, 0, sizeof(addFilter));
                }
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
            ImGui::InputText("##filter", playlistFilter, sizeof(playlistFilter));

            ImGui::BeginChild("PlaylistSongs", ImVec2(0, 0), true);

            if (g_viewPlaylistIndex >= 0 && g_viewPlaylistIndex < (int)g_playlists.size())
            {
                if (g_viewPlaylistIndex != g_cachedViewPlaylistIndex)
                {
                    g_cachedPlaylistSongs = get_playlist_songs(g_playlists[g_viewPlaylistIndex]);
                    g_cachedViewPlaylistIndex = g_viewPlaylistIndex;
                }
                const std::vector<SongInfo>& playlistSongs = g_cachedPlaylistSongs;

                std::string filterLower = playlistFilter;
                std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

                for (const auto& song : playlistSongs)
                {
                    if (!filterLower.empty())
                    {
                        std::string titleLower = song.title;
                        std::string artistLower = song.artist;
                        std::transform(titleLower.begin(), titleLower.end(), titleLower.begin(), ::tolower);
                        std::transform(artistLower.begin(), artistLower.end(), artistLower.begin(), ::tolower);

                        if (titleLower.find(filterLower) == std::string::npos &&
                            artistLower.find(filterLower) == std::string::npos)
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
                    bool is_current = (song.id == getCurrentPlayingId()) && g_viewPlaylistIndex == g_currentPlaylistIndex;
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + availableWidth);
                    ImGui::SetWindowFontScale(1.1f);
                    if(is_current){
                        ImGui::SetWindowFontScale(1.2f);
                        ImGui::PushStyleColor(ImGuiCol_Text, g_highlightColor);
                    }
                    ImGui::TextWrapped("%s", song.title.c_str());
                    ImGui::PopTextWrapPos();
                    if(is_current){
                        ImGui::PopStyleColor();
                    }
                    ImGui::SetWindowFontScale(1.0f);

                    // Get the rect of the title text
                    ImVec2 titleMin = ImGui::GetItemRectMin();
                    ImVec2 titleMax = ImGui::GetItemRectMax();

                    // Create a selectable that covers the title area
                    ImGui::SetCursorScreenPos(titleMin);
                    if (ImGui::Selectable("##title_select", false, ImGuiSelectableFlags_None, ImVec2(titleMax.x - titleMin.x, titleMax.y - titleMin.y)))
                    {
                        // set playback playlist index to view playlist index
                        g_currentPlaylistIndex = g_viewPlaylistIndex;
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
                        if (g_viewPlaylistIndex >= 0 && g_viewPlaylistIndex < (int)g_playlists.size())
                        {
                            const std::string& playlistName = g_playlists[g_viewPlaylistIndex].name;
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
                    remove_song_from_playlist(g_playlists[g_viewPlaylistIndex].name, g_selectedSongForOptions.id);
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
            // Settings Popup
            // ----------------------------------------------------
            if (g_showSettingsPopup)
            {
                ImGui::OpenPopup("settings");
                g_showSettingsPopup = false;
            }

            if (ImGui::BeginPopupModal("settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("preferences");
                ImGui::Separator();

                // Cache Size Slider
                ImGui::Text("cache size");
                ImGui::Indent();

                int oldCacheSize = g_cacheSizeSetting;
                if (ImGui::SliderInt("##cache size", &g_cacheSizeSetting, 1, 20))
                {
                    if (oldCacheSize != g_cacheSizeSetting)
                    {
                        std::cout << "Cache size changed to " << g_cacheSizeSetting << "\n";
                    }
                }

                ImGui::Unindent();
                ImGui::Spacing();

                // Theme Dropdown
                ImGui::Text("appearance");
                ImGui::Indent();

                int oldTheme = g_currentTheme;
                if (ImGui::Combo("##theme", &g_currentTheme, g_themeNames, IM_ARRAYSIZE(g_themeNames)))
                {
                    if (oldTheme != g_currentTheme)
                    {
                        ApplyTheme(g_currentTheme);
                        std::cout << "Theme changed to " << g_themeNames[g_currentTheme] << std::endl;
                    }
                }

                ImGui::Unindent();
                ImGui::Spacing();

                ImGui::Text("color");

                // Color picker
                ImGui::ColorEdit3("##color", &g_highlightColor.x, ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoInputs);

                // Additional settings can be added here
                ImGui::Separator();
                ImGui::Text("about");
                ImGui::Indent();
                ImGui::Text("zapp music player");
                ImGui::Text("ver 1.0");
                ImGui::Unindent();

                ImGui::Spacing();
                ImGui::Separator();

                // Buttons
                if (ImGui::Button("close", ImVec2(120, 0)))
                {
                    save_settings();
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }

            // ----------------------------------------------------
            // Keyboard inputs
            // ----------------------------------------------------

            // Check for Ctrl/Cmd + Comma
            bool ctrlOrCmd = io.KeyCtrl || io.KeySuper;
            if (ctrlOrCmd && ImGui::IsKeyPressed(ImGuiKey_Comma))
            {
                g_showSettingsPopup = true;
            }

            // Check for spacebar to play/pause
            if (ImGui::IsKeyPressed(ImGuiKey_Space))
            {
                gPlayback.playing = !gPlayback.playing;
            }

            // ----------------------------------------------------
            // Render
            // ----------------------------------------------------
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);

            // Frame rate limiting
            Uint32 frameTime = SDL_GetTicks() - frameStart;
            if (frameTime < TARGET_FRAME_TIME)
            {
                SDL_Delay(TARGET_FRAME_TIME - frameTime);
            }

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

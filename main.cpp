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

// ------------------------------------------------------------
// minimp3
// ------------------------------------------------------------

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3_ex.h"

// ------------------------------------------------------------
// Song Info Structure with ID
// ------------------------------------------------------------

struct SongInfo
{
    int id = -1;
    std::string path;
    std::string title;
    std::string artist;
    int playCount = 0;
};

// ------------------------------------------------------------
// Playlist Structure
// ------------------------------------------------------------

struct Playlist
{
    std::string name;
    std::vector<int> songIds;  // IDs of songs in this playlist
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
std::vector<SongInfo> g_allSongs;          // All songs with IDs
std::vector<Playlist> g_playlists;         // All playlists (includes "library")
int g_currentPlaylistIndex = -1;           // Index of currently open playlist
SongInfo g_nowPlaying;
std::mutex g_songMutex;
std::atomic<bool> g_scanning = false;
std::string g_scanStatus = "";
std::atomic<bool> g_requestNextSong = false;  // Set by audio thread when song ends

// For "Add to playlist" popup
SongInfo g_selectedSongForOptions;
bool g_showSongOptionsPopup = false;
bool g_showAddToPlaylistPopup = false;

// For "Add from Library" popup
bool g_showAddFromLibraryPopup = false;

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

// Convert Latin-1 (ISO-8859-1) to UTF-8
std::string latin1_to_utf8(const std::string& latin1)
{
    std::string utf8;
    for (unsigned char c : latin1)
    {
        if (c < 0x80)
        {
            utf8 += c;
        }
        else
        {
            utf8 += static_cast<char>(0xC0 | (c >> 6));
            utf8 += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return utf8;
}

// Sanitize a string to ensure it contains only valid UTF-8 sequences
std::string sanitize_utf8(const std::string& s)
{
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); )
    {
        unsigned char c = s[i];
        if (c < 0x80)
        {
            // ASCII
            result += c;
            ++i;
        }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size())
        {
            // 2-byte UTF-8
            unsigned char c2 = s[i+1];
            if ((c2 & 0xC0) == 0x80)
            {
                result += c;
                result += c2;
                i += 2;
            }
            else
            {
                // invalid, skip
                ++i;
            }
        }
        else if ((c & 0xF0) == 0xE0 && i + 2 < s.size())
        {
            // 3-byte UTF-8
            unsigned char c2 = s[i+1];
            unsigned char c3 = s[i+2];
            if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80)
            {
                result += c;
                result += c2;
                result += c3;
                i += 3;
            }
            else
            {
                ++i;
            }
        }
        else if ((c & 0xF8) == 0xF0 && i + 3 < s.size())
        {
            // 4-byte UTF-8
            unsigned char c2 = s[i+1];
            unsigned char c3 = s[i+2];
            unsigned char c4 = s[i+3];
            if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80 && (c4 & 0xC0) == 0x80)
            {
                result += c;
                result += c2;
                result += c3;
                result += c4;
                i += 4;
            }
            else
            {
                ++i;
            }
        }
        else
        {
            // invalid byte, skip
            ++i;
        }
    }
    return result;
}

void extract_metadata(const std::string& path, std::string& title, std::string& artist)
{
    bool has_id3 = false;
    if (read_id3v1(path, title, artist))
    {
        // ID3v1 fields are Latin-1, convert to UTF-8
        title = latin1_to_utf8(title);
        artist = latin1_to_utf8(artist);
        has_id3 = true;
    }

    if (!has_id3 || title.empty() || title.find_first_not_of(" \0") == std::string::npos)
    {
        title = get_filename_without_ext(path);
        // Filename may already be UTF-8, but sanitize anyway
        title = sanitize_utf8(title);
    }

    if (!has_id3 || artist.empty() || artist.find_first_not_of(" \0") == std::string::npos)
    {
        artist = "Unknown Artist";
    }

    // Final sanitization to be safe
    title = sanitize_utf8(title);
    artist = sanitize_utf8(artist);
}

// ------------------------------------------------------------
// JSON Database Functions (Songs)
// ------------------------------------------------------------

const std::string SONGS_DB_FILENAME = "songs_db.json";

void save_songs_database(const std::vector<SongInfo>& songs)
{
    nlohmann::json j;
    for (const auto& song : songs)
    {
        nlohmann::json songJson;
        songJson["id"] = song.id;
        songJson["path"] = song.path;
        songJson["title"] = song.title;
        songJson["artist"] = song.artist;
        songJson["playCount"] = song.playCount;
        j.push_back(songJson);
    }

    std::ofstream file(SONGS_DB_FILENAME);
    if (file.is_open())
    {
        file << j.dump(4);
    }
}

std::vector<SongInfo> load_songs_database()
{
    std::vector<SongInfo> songs;

    std::ifstream file(SONGS_DB_FILENAME);
    if (!file.is_open()) return songs;

    nlohmann::json j;
    try
    {
        file >> j;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error parsing songs database: " << e.what() << std::endl;
        return songs;
    }

    int maxId = 0;
    for (const auto& item : j)
    {
        SongInfo song;
        if (item.contains("id"))
            song.id = item["id"];
        else
            song.id = -1; // will assign later

        song.path = item.value("path", "");
        song.title = item.value("title", "");
        song.artist = item.value("artist", "");
        song.playCount = item.value("playCount", 0);
        if (!song.path.empty())
        {
            // sanitize song title and artist name
            song.title = sanitize_utf8(song.title);
            song.artist = sanitize_utf8(song.artist);
            songs.push_back(song);
            if (song.id > maxId) maxId = song.id;
        }
    }

    // Assign IDs to songs that don't have them (legacy database)
    bool needsSave = false;
    for (auto& song : songs)
    {
        if (song.id == -1)
        {
            song.id = ++maxId;
            needsSave = true;
        }
    }

    if (needsSave)
        save_songs_database(songs);

    return songs;
}

// Increment play count for a song and save to database
void increment_play_count(int songId)
{
    std::lock_guard<std::mutex> lock(g_songMutex);
    auto it = std::find_if(g_allSongs.begin(), g_allSongs.end(),
        [songId](const SongInfo& s) { return s.id == songId; });
    if (it != g_allSongs.end())
    {
        it->playCount++;
        save_songs_database(g_allSongs);
        std::cout << "Play count for " << it->title << " is now " << it->playCount << std::endl;
    }
}

// ------------------------------------------------------------
// Playlist Database Functions
// ------------------------------------------------------------

const std::string PLAYLISTS_DB_FILENAME = "playlists.json";

void save_playlists_database(const std::vector<Playlist>& playlists)
{
    nlohmann::json j;
    for (const auto& pl : playlists)
    {
        nlohmann::json plJson;
        plJson["name"] = pl.name;
        plJson["songIds"] = pl.songIds;
        j.push_back(plJson);
    }

    std::ofstream file(PLAYLISTS_DB_FILENAME);
    if (file.is_open())
    {
        file << j.dump(4);
    }
}

std::vector<Playlist> load_playlists_database()
{
    std::vector<Playlist> playlists;

    std::ifstream file(PLAYLISTS_DB_FILENAME);
    if (!file.is_open()) return playlists;

    nlohmann::json j;
    file >> j;

    for (const auto& item : j)
    {
        Playlist pl;
        pl.name = item.value("name", "");
        if (item.contains("songIds"))
            pl.songIds = item["songIds"].get<std::vector<int>>();
        if (!pl.name.empty())
            playlists.push_back(pl);
    }

    return playlists;
}

// Ensure "library" playlist exists and contains all song IDs
void ensure_library_playlist()
{
    // Find library playlist
    auto it = std::find_if(g_playlists.begin(), g_playlists.end(),
        [](const Playlist& p) { return p.name == "library"; });

    if (it == g_playlists.end())
    {
        // Create library playlist with all song IDs
        Playlist library;
        library.name = "library";
        for (const auto& song : g_allSongs)
            library.songIds.push_back(song.id);
        g_playlists.push_back(library);
    }
    else
    {
        // Update library playlist to contain all song IDs
        it->songIds.clear();
        for (const auto& song : g_allSongs)
            it->songIds.push_back(song.id);
    }
    save_playlists_database(g_playlists);
}

// Add a song to a playlist (avoid duplicates)
bool add_song_to_playlist(const std::string& playlistName, int songId)
{
    auto it = std::find_if(g_playlists.begin(), g_playlists.end(),
        [&](const Playlist& p) { return p.name == playlistName; });
    if (it == g_playlists.end()) return false;

    // Check for duplicate
    if (std::find(it->songIds.begin(), it->songIds.end(), songId) != it->songIds.end())
        return false; // Already in playlist

    it->songIds.push_back(songId);
    save_playlists_database(g_playlists);
    return true;
}

// Remove a song from a playlist
bool remove_song_from_playlist(const std::string& playlistName, int songId)
{
    auto it = std::find_if(g_playlists.begin(), g_playlists.end(),
        [&](const Playlist& p) { return p.name == playlistName; });
    if (it == g_playlists.end()) return false;

    // Cannot remove from library playlist? (optional - library should always have all songs)
    if (playlistName == "library") return false;

    // Find and remove the song ID from the playlist
    auto songIt = std::find(it->songIds.begin(), it->songIds.end(), songId);
    if (songIt == it->songIds.end()) return false; // Song not in playlist

    it->songIds.erase(songIt);
    save_playlists_database(g_playlists);
    return true;
}

// Create a new playlist
bool create_playlist(const std::string& name)
{
    // Check if name already exists
    auto it = std::find_if(g_playlists.begin(), g_playlists.end(),
        [&](const Playlist& p) { return p.name == name; });
    if (it != g_playlists.end()) return false;

    Playlist newPlaylist;
    newPlaylist.name = name;
    g_playlists.push_back(newPlaylist);
    save_playlists_database(g_playlists);
    return true;
}

// Delete a playlist (cannot delete "library")
bool delete_playlist(const std::string& name)
{
    if (name == "library") return false;

    auto it = std::find_if(g_playlists.begin(), g_playlists.end(),
        [&](const Playlist& p) { return p.name == name; });
    if (it == g_playlists.end()) return false;

    g_playlists.erase(it);
    save_playlists_database(g_playlists);
    return true;
}

// Get songs for a playlist (returns vector of full SongInfo)
std::vector<SongInfo> get_playlist_songs(const Playlist& playlist)
{
    std::vector<SongInfo> songs;
    std::lock_guard<std::mutex> lock(g_songMutex);
    for (int id : playlist.songIds)
    {
        auto it = std::find_if(g_allSongs.begin(), g_allSongs.end(),
            [id](const SongInfo& s) { return s.id == id; });
        if (it != g_allSongs.end())
            songs.push_back(*it);
    }
    return songs;
}

// Get the next song in the current playlist
int get_next_song_in_playlist(int currentSongId)
{
    if (g_currentPlaylistIndex < 0 || g_currentPlaylistIndex >= (int)g_playlists.size())
        return -1;

    const auto& playlist = g_playlists[g_currentPlaylistIndex];
    auto it = std::find(playlist.songIds.begin(), playlist.songIds.end(), currentSongId);
    if (it == playlist.songIds.end() || it + 1 == playlist.songIds.end())
        return -1;

    return *(it + 1);
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
                    // sanitize
                    song.title = sanitize_utf8(song.title);
                    song.artist = sanitize_utf8(song.artist);
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
        // Assign new IDs and merge with existing songs (avoid duplicates by path)
        std::lock_guard<std::mutex> lock(g_songMutex);
        int maxId = 0;
        for (const auto& song : g_allSongs)
            if (song.id > maxId) maxId = song.id;

        for (auto& song : newSongs)
        {
            // Check if song already exists by path
            auto it = std::find_if(g_allSongs.begin(), g_allSongs.end(),
                [&](const SongInfo& s) { return s.path == song.path; });
            if (it == g_allSongs.end())
            {
                song.id = ++maxId;
                g_allSongs.push_back(song);
            }
        }

        // Save songs database
        save_songs_database(g_allSongs);

        // Update library playlist
        ensure_library_playlist();

        g_scanStatus = "Scan complete! Found " + std::to_string(newSongs.size()) + " new songs";
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

// Play a song by its ID
bool play_song_by_id(int songId)
{
    auto it = std::find_if(g_allSongs.begin(), g_allSongs.end(),
        [songId](const SongInfo& s) { return s.id == songId; });
    if (it != g_allSongs.end())
    {
        if (load_mp3(it->path))
        {
            g_nowPlaying = *it;
            return true;
        }
    }
    return false;
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
            if (audio->playing)  // Only trigger once when song ends
            {
                audio->playing = false;
                g_requestNextSong = true;  // Signal main thread to play next song

                // increment play count for song
                increment_play_count(g_nowPlaying.id);
            }
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

        // ----------------------------------------------------
        // Auto-play next song when current finishes
        // ----------------------------------------------------

        if (g_requestNextSong)
        {
            g_requestNextSong = false;  // Reset flag

            int nextId = get_next_song_in_playlist(g_nowPlaying.id);
            if (nextId != -1)
            {
                play_song_by_id(nextId);
            }
            else
            {
                // go back to start if playlist is over
                play_song_by_id(g_playlists[g_currentPlaylistIndex].songIds[0]);
            }
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
                    if (play_song_by_id(song.id))
                    {
                        std::cout << "Now playing: " << song.title << "\n";
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

            for (const auto& song : g_allSongs)
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
                        if (add_song_to_playlist(playlistName, song.id))
                        {
                            // Success
                        }
                        else
                        {
                            // Could show a tooltip, but ignore for now
                        }
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

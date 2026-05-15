#ifndef ZAPP_DATA_H
#define ZAPP_DATA_H

#include <iostream>
#include <string.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <thread>
#include <nlohmann/json.hpp>
#include "definitions.h"
#include "globals.h"
#include "helpers.h"
#include "helperImgui.h"

// ------------------------------------------------------------
// JSON Database Functions (Songs)
// ------------------------------------------------------------

inline const std::string SONGS_DB_FILENAME = "songs_db.json";

inline void save_songs_worker(const std::vector<SongInfo>& songs)
{
    g_savingSongs = true;
    {
        std::lock_guard<std::mutex> lock(g_saveMutex);
        g_saveStatus = "Saving songs database...";
    }

    // Perform the actual file write
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
        {
            std::lock_guard<std::mutex> lock(g_saveMutex);
            g_saveStatus = "Songs database saved successfully!";
        }
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(g_saveMutex);
            g_saveStatus = "ERROR: Could not save songs database!";
        }
        std::cerr << "Failed to save songs database\n";
    }

    g_savingSongs = false;
}

inline void save_songs_database_async(const std::vector<SongInfo>& songs)
{
    if (g_savingSongs) return; // Already saving, skip

    // Make a copy of the songs to save (in case the original changes)
    std::vector<SongInfo> songsCopy = songs;

    std::thread saveThread([songsCopy]() {
        save_songs_worker(songsCopy);
    });
    saveThread.detach();
}

inline std::vector<SongInfo> load_songs_database()
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
        save_songs_database_async(songs);

    return songs;
}

// Increment play count for a song and save to database
inline void increment_play_count(int songId)
{
    std::lock_guard<std::mutex> lock(g_songMutex);
    auto it = std::find_if(g_allSongs.begin(), g_allSongs.end(),
        [songId](const SongInfo& s) { return s.id == songId; });
    if (it != g_allSongs.end())
    {
        it->playCount++;
        save_songs_database_async(g_allSongs);
        std::cout << "Play count for " << it->title << " is now " << it->playCount << std::endl;
    }
}

// ------------------------------------------------------------
// Playlist Database Functions
// ------------------------------------------------------------

inline const std::string PLAYLISTS_DB_FILENAME = "playlists.json";

inline void save_playlists_worker(const std::vector<Playlist>& playlists)
{
    g_savingPlaylists = true;
    {
        std::lock_guard<std::mutex> lock(g_saveMutex);
        g_saveStatus = "Saving playlists database...";
    }

    // Perform the actual file write
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
        {
            std::lock_guard<std::mutex> lock(g_saveMutex);
            g_saveStatus = "Playlists database saved successfully!";
        }
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(g_saveMutex);
            g_saveStatus = "ERROR: Could not save playlists database!";
        }
        std::cerr << "Failed to save playlists database\n";
    }

    g_savingPlaylists = false;
}

inline void save_playlists_database_async(const std::vector<Playlist>& playlists)
{
    if (g_savingPlaylists) return; // Already saving, skip
    g_songlistUpdate = true;

    // Make a copy of the playlists to save (in case the original changes)
    std::vector<Playlist> playlistsCopy = playlists;

    std::thread saveThread([playlistsCopy]() {
        save_playlists_worker(playlistsCopy);
    });
    saveThread.detach();
}

inline std::vector<Playlist> load_playlists_database()
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
inline void ensure_library_playlist()
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
    save_playlists_database_async(g_playlists);
}

// Add a song to a playlist (avoid duplicates)
inline bool add_song_to_playlist(const std::string& playlistName, int songId)
{
    auto it = std::find_if(g_playlists.begin(), g_playlists.end(),
        [&](const Playlist& p) { return p.name == playlistName; });
    if (it == g_playlists.end()) return false;

    // Check for duplicate
    if (std::find(it->songIds.begin(), it->songIds.end(), songId) != it->songIds.end())
        return false; // Already in playlist

    it->songIds.push_back(songId);
    save_playlists_database_async(g_playlists);
    return true;
}

// Remove a song from a playlist
inline bool remove_song_from_playlist(const std::string& playlistName, int songId)
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
    save_playlists_database_async(g_playlists);
    return true;
}

// Create a new playlist
inline bool create_playlist(const std::string& name)
{
    // Check if name already exists
    auto it = std::find_if(g_playlists.begin(), g_playlists.end(),
        [&](const Playlist& p) { return p.name == name; });
    if (it != g_playlists.end()) return false;

    Playlist newPlaylist;
    newPlaylist.name = name;
    g_playlists.push_back(newPlaylist);
    save_playlists_database_async(g_playlists);
    return true;
}

// Delete a playlist (cannot delete "library")
inline bool delete_playlist(const std::string& name)
{
    if (name == "library") return false;

    auto it = std::find_if(g_playlists.begin(), g_playlists.end(),
        [&](const Playlist& p) { return p.name == name; });
    if (it == g_playlists.end()) return false;

    g_playlists.erase(it);
    save_playlists_database_async(g_playlists);
    return true;
}

// Get songs for a playlist (returns vector of full SongInfo)
inline std::vector<SongInfo> get_playlist_songs(const Playlist& playlist)
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

// Get the next song in the current playlist (with wrap-around)
inline int get_next_song_in_playlist(int currentSongId)
{
    if (g_currentPlaylistIndex < 0 || g_currentPlaylistIndex >= (int)g_playlists.size())
        return -1;

    const auto& playlist = g_playlists[g_currentPlaylistIndex];

    if (playlist.songIds.empty())
        return -1;

    auto it = std::find(playlist.songIds.begin(), playlist.songIds.end(), currentSongId);
    if (it == playlist.songIds.end())
        return -1;

    // Wrap to first song if at the end
    if (it + 1 == playlist.songIds.end())
        return playlist.songIds.front();

    return *(it + 1);
}

// Get the previous song in the current playlist (with wrap-around)
inline int get_prev_song_in_playlist(int currentSongId)
{
    if (g_currentPlaylistIndex < 0 || g_currentPlaylistIndex >= (int)g_playlists.size())
        return -1;

    const auto& playlist = g_playlists[g_currentPlaylistIndex];

    if (playlist.songIds.empty())
        return -1;

    auto it = std::find(playlist.songIds.begin(), playlist.songIds.end(), currentSongId);
    if (it == playlist.songIds.end())
        return -1;

    // Wrap to last song if at the beginning
    if (it == playlist.songIds.begin())
        return playlist.songIds.back();

    return *(it - 1);
}


// save/load settings

inline const std::string SETTINGS_FILENAME = "settings.json";

void save_settings()
{
    nlohmann::json j;

    // Save basic settings
    j["cacheSize"] = g_cacheSizeSetting;
    j["theme"] = g_currentTheme;

    // Save highlight color (RGBA)
    j["highlightColor"]["r"] = g_highlightColor.x;
    j["highlightColor"]["g"] = g_highlightColor.y;
    j["highlightColor"]["b"] = g_highlightColor.z;
    j["highlightColor"]["a"] = g_highlightColor.w;

    // Write to file
    std::ofstream file(SETTINGS_FILENAME);
    if (file.is_open())
    {
        file << j.dump(4);  // Pretty print with 4 spaces
        std::cout << "Settings saved to " << SETTINGS_FILENAME << std::endl;
    }
    else
    {
        std::cerr << "Failed to save settings to " << SETTINGS_FILENAME << std::endl;
    }
}

void load_settings()
{
    std::ifstream file(SETTINGS_FILENAME);
    if (!file.is_open())
    {
        std::cout << "No settings file found, using defaults" << std::endl;
        return;
    }

    nlohmann::json j;
    try
    {
        file >> j;

        // Load basic settings
        g_cacheSizeSetting = j.value("cacheSize", 5);
        g_currentTheme = j.value("theme", 0);

        // Load highlight color
        if (j.contains("highlightColor"))
        {
            g_highlightColor.x = j["highlightColor"].value("r", 0.0f);
            g_highlightColor.y = j["highlightColor"].value("g", 1.0f);
            g_highlightColor.z = j["highlightColor"].value("b", 0.0f);
            g_highlightColor.w = j["highlightColor"].value("a", 1.0f);
        }

        // Clamp values to valid ranges
        if (g_cacheSizeSetting < 1) g_cacheSizeSetting = 1;
        if (g_cacheSizeSetting > 20) g_cacheSizeSetting = 20;
        if (g_currentTheme < 0) g_currentTheme = 0;
        if (g_currentTheme > 2) g_currentTheme = 2;

        // Apply loaded theme
        ApplyTheme(g_currentTheme);

        std::cout << "Settings loaded: Cache=" << g_cacheSizeSetting
                  << ", Theme=" << g_themeNames[g_currentTheme]
                  << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error loading settings: " << e.what() << std::endl;
    }
}

#endif

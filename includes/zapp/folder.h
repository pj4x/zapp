#ifndef ZAPP_FOLDER_H
#define ZAPP_FOLDER_H

#include <iostream>
#include <string.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <thread>
#include "definitions.h"
#include "globals.h"
#include "helpers.h"
#include "mp3.h"
#include "database.h"

// ------------------------------------------------------------
// Folder Scanning (Worker Thread)
// ------------------------------------------------------------

inline void scan_folder_worker(const std::string& folderPath, bool addToPlaylist, std::string& playlistName)
{
    g_scanStatus = "Scanning folder: " + folderPath;

    std::vector<SongInfo> newSongs;
    std::vector<int> newSongIds;  // Store IDs of newly added songs

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

                    // Sanitize after metadata extraction
                    song.title = sanitize_utf8(song.title);
                    song.artist = sanitize_utf8(song.artist);
                    song.playCount = 0;

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
        // Lock both song and playlist mutexes
        std::lock_guard<std::mutex> songLock(g_songMutex);
        std::lock_guard<std::mutex> playlistLock(g_playlistsMutex);

        // Find max existing ID
        int maxId = 0;
        for (const auto& song : g_allSongs)
            if (song.id > maxId) maxId = song.id;

        // Add new songs and collect their IDs
        for (auto& song : newSongs)
        {
            // Check if song already exists by path
            auto it = std::find_if(g_allSongs.begin(), g_allSongs.end(),
                [&](const SongInfo& s) { return s.path == song.path; });
            if (it == g_allSongs.end())
            {
                song.id = ++maxId;
                g_allSongs.push_back(song);
                newSongIds.push_back(song.id);
            }
            else
            {
                // Song already exists, use existing ID
                newSongIds.push_back(it->id);
            }
        }

        // If addToPlaylist is true, add all new songs to the specified playlist
        if (addToPlaylist && !playlistName.empty() && !newSongIds.empty())
        {
            auto playlistIt = std::find_if(g_playlists.begin(), g_playlists.end(),
                [&](const Playlist& p) { return p.name == playlistName; });

            if (playlistIt != g_playlists.end())
            {
                for (int songId : newSongIds)
                {
                    // Check for duplicates
                    if (std::find(playlistIt->songIds.begin(), playlistIt->songIds.end(), songId) == playlistIt->songIds.end())
                    {
                        playlistIt->songIds.push_back(songId);
                    }
                }
            }
            else if (playlistName != "library")
            {
                // Create the playlist if it doesn't exist
                Playlist newPlaylist;
                newPlaylist.name = playlistName;
                newPlaylist.songIds = newSongIds;
                g_playlists.push_back(newPlaylist);
            }
        }

        // Save databases (async is fine here since we're done with locks)
        save_songs_database_async(g_allSongs);
        save_playlists_database_async(g_playlists);

        // Update library playlist to include all songs
        // Do this after the lock is released to avoid holding it during save
        // Actually, we can call ensure_library_playlist here - it will lock again
    }

    // Release locks before calling ensure_library_playlist
    {
        std::lock_guard<std::mutex> lock(g_playlistsMutex);
        ensure_library_playlist();  // This will lock internally
    }

    if (g_scanning)
    {
        g_scanStatus = "Scan complete! Found " + std::to_string(newSongs.size()) + " new songs";
    }
    else
    {
        g_scanStatus = "Scan cancelled";
    }

    g_scanning = false;
}

void start_folder_scan(const std::string& folderPath, bool addToPlaylist = false, const std::string& playlistName = "")
{
    if (g_scanning) return;

    g_scanning = true;
    // Make a copy of playlistName for the thread
    std::string playlistNameCopy = playlistName;
    std::thread scanThread([folderPath, addToPlaylist, playlistNameCopy]() {
        scan_folder_worker(folderPath, addToPlaylist, const_cast<std::string&>(playlistNameCopy));
    });
    scanThread.detach();
}

#endif

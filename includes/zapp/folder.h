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

inline void scan_folder_worker(const std::string& folderPath)
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
        save_songs_database_async(g_allSongs);

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

inline void start_folder_scan(const std::string& folderPath)
{
    if (g_scanning) return;

    g_scanning = true;
    std::thread scanThread(scan_folder_worker, folderPath);
    scanThread.detach();
}

#endif

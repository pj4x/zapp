#ifndef ZAPP_MP3_H
#define ZAPP_MP3_H

#include <iostream>
#include <string.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <thread>
#include "minimp3_ex.h"
#include "definitions.h"
#include "globals.h"
#include "helpers.h"
#include "helperSdl.h"
#include "database.h"

// ------------------------------------------------------------
// MP3 Metadata Extraction (ID3v1)
// ------------------------------------------------------------

inline bool read_id3v1(const std::string& path, std::string& title, std::string& artist)
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

inline void extract_metadata(const std::string& path, std::string& title, std::string& artist)
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
// Load MP3 with thread safety
// ------------------------------------------------------------

// Clear least recently used from cache (simple FIFO for now)
// WARNING: This function assumes g_cacheMutex is already locked!
inline void manage_cache_size()
{
    // Do NOT lock here - mutex should already be locked by caller
    if (g_audioCache.size() > g_cacheSizeSetting)
    {
        // Find least recently used (LRU)
        auto lru = g_audioCache.begin();
        for (auto it = g_audioCache.begin(); it != g_audioCache.end(); ++it)
        {
            if (it->second.lastUsed < lru->second.lastUsed)
                lru = it;
        }

        std::cout << "Cache size exceeded, removing LRU song ID: " << lru->first << "\n";
        g_audioCache.erase(lru);
    }
}

// load mp3 from disk into cache
inline bool load_mp3(int songId, const std::string& path)
{
    std::cout << "load_mp3: Starting for song " << songId << std::endl;

    // Check if already in cache
    {
        std::lock_guard<std::mutex> cacheLock(g_cacheMutex);
        auto it = g_audioCache.find(songId);
        if (it != g_audioCache.end() && it->second.isValid)
        {
            std::cout << "Song " << songId << " already in cache\n";
            return true;
        }
    }

    std::cout << "Loading song " << songId << " from disk: " << path << "\n";
    std::cout << "Opening MP3 file..." << std::endl;

    mp3dec_ex_t dec;
    int result = mp3dec_ex_open(&dec, path.c_str(), MP3D_SEEK_TO_SAMPLE);
    if (result)
    {
        std::cerr << "Failed to open MP3: " << path << " error code: " << result << "\n";
        return false;
    }

    std::cout << "MP3 opened successfully. Channels: " << dec.info.channels
              << ", Sample rate: " << dec.info.hz
              << ", Samples: " << dec.samples << std::endl;

    CachedAudio cached;
    cached.channels = dec.info.channels;
    cached.sampleRate = dec.info.hz;
    cached.totalFrames = dec.samples / cached.channels;
    cached.pcm.resize(dec.samples);
    cached.isValid = true;
    cached.lastUsed = std::chrono::steady_clock::now();

    std::cout << "Reading PCM data (" << dec.samples << " samples)..." << std::endl;
    size_t read = mp3dec_ex_read(&dec, cached.pcm.data(), dec.samples);
    std::cout << "Read " << read << " samples" << std::endl;

    mp3dec_ex_close(&dec);

    if (read == 0)
    {
        std::cerr << "Failed to decode MP3: " << path << "\n";
        return false;
    }

    std::cout << "Storing in cache..." << std::endl;
    // Store in cache
    {
        std::lock_guard<std::mutex> cacheLock(g_cacheMutex);
        g_audioCache[songId] = std::move(cached);
        manage_cache_size();
    }

    std::cout << "Successfully loaded and cached song " << songId << "\n";
    return true;
}

inline bool play_song_by_id(int songId)
{
    std::cout << "play_song_by_id: Starting for song ID " << songId << std::endl;

    // Quick validation
    if (songId < 0) {
        std::cerr << "Invalid song ID: " << songId << std::endl;
        return false;
    }

    // Find the song info
    SongInfo songInfo;
    {
        std::lock_guard<std::mutex> lock(g_songMutex);

        if (g_allSongs.empty()) {
            std::cerr << "No songs in library!" << std::endl;
            return false;
        }

        auto it = std::find_if(g_allSongs.begin(), g_allSongs.end(),
            [songId](const SongInfo& s) { return s.id == songId; });
        if (it == g_allSongs.end())
        {
            std::cerr << "Song ID " << songId << " not found in database\n";
            return false;
        }
        songInfo = *it;
    }

    // Validate path
    if (songInfo.path.empty()) {
        std::cerr << "Empty path for song ID " << songId << std::endl;
        return false;
    }

    std::cout << "Found song: " << songInfo.title << " at path: " << songInfo.path << std::endl;

    // Check if song is cached, if not, load it
    bool needsLoad = false;
    {
        std::lock_guard<std::mutex> cacheLock(g_cacheMutex);
        auto cacheIt = g_audioCache.find(songId);
        if (cacheIt == g_audioCache.end() || !cacheIt->second.isValid)
        {
            needsLoad = true;
        }
    }

    if (needsLoad)
    {
        std::cout << "Song " << songId << " not cached, loading first...\n";
        if (!load_mp3(songId, songInfo.path))
        {
            std::cerr << "Failed to load song " << songId << "\n";
            return false;
        }
    }

    // Get the cached audio info BEFORE locking for playback
    int mp3SampleRate = 0;
    int mp3Channels = 0;
    size_t mp3TotalFrames = 0;

    {
        std::lock_guard<std::mutex> cacheLock(g_cacheMutex);
        auto cacheIt = g_audioCache.find(songId);
        if (cacheIt == g_audioCache.end() || !cacheIt->second.isValid)
        {
            std::cerr << "Song " << songId << " not found in cache after loading\n";
            return false;
        }
        mp3SampleRate = cacheIt->second.sampleRate;
        mp3Channels = cacheIt->second.channels;
        mp3TotalFrames = cacheIt->second.totalFrames;
    }

    // Reconfigure audio if needed (sample rate or channels differ)
    if (!g_audioInitialized ||
        g_currentAudioSpec.freq != mp3SampleRate ||
        g_currentAudioSpec.channels != mp3Channels)
    {
        std::cout << "Reconfiguring audio from " << g_currentAudioSpec.freq
                  << "Hz/" << g_currentAudioSpec.channels << "ch to "
                  << mp3SampleRate << "Hz/" << mp3Channels << "ch" << std::endl;

        if (!ReconfigureAudio(mp3SampleRate, mp3Channels))
        {
            std::cerr << "Failed to reconfigure audio for sample rate "
                      << mp3SampleRate << std::endl;
            // Try to fall back to original config
            ReconfigureAudio(44100, 2);
            return false;
        }
    }

    // Move from cache to global audio state
    {
        std::lock_guard<std::mutex> cacheLock(g_cacheMutex);
        std::lock_guard<std::mutex> audioLock(gPlayback.mutex);

        auto cacheIt = g_audioCache.find(songId);
        if (cacheIt == g_audioCache.end() || !cacheIt->second.isValid)
        {
            std::cerr << "Song " << songId << " not found in cache after loading\n";
            return false;
        }

        // Validate PCM data
        if (cacheIt->second.pcm.empty()) {
            std::cerr << "Empty PCM data for song " << songId << std::endl;
            return false;
        }

        // Point to cached data
        gPlayback.pcm = cacheIt->second.pcm.data();
        gPlayback.pcmSize = cacheIt->second.pcm.size();
        gPlayback.channels = cacheIt->second.channels;
        gPlayback.mp3SampleRate = cacheIt->second.sampleRate;
        gPlayback.outputSampleRate = g_currentAudioSpec.freq;  // Use actual output rate
        gPlayback.totalFrames = cacheIt->second.totalFrames;
        gPlayback.currentFrame = 0;
        gPlayback.playing = true;
        gPlayback.currentSongId = songId;

        // Update last used time
        cacheIt->second.lastUsed = std::chrono::steady_clock::now();

        std::cout << "Audio playback set up. MP3 rate: " << gPlayback.mp3SampleRate
                  << " Hz, Output rate: " << gPlayback.outputSampleRate
                  << " Hz, Channels: " << gPlayback.channels
                  << ", Total frames: " << gPlayback.totalFrames << std::endl;
    }

    // Update current playing song
    setCurrentPlaying(songInfo);

    // Reset preload flag for the new song
    g_hasPreloadedForCurrentSong = false;

    std::cout << "Now playing song ID " << songId << ": " << songInfo.title << std::endl;
    g_hasPreloadedForCurrentSong = false;
    return true;
}

inline void preload_next_song_background()
{
    if (g_preloadingNext) return;

    // Safety check
    if (g_currentPlaylistIndex < 0 || g_currentPlaylistIndex >= (int)g_playlists.size())
        return;

    const auto& playlist = g_playlists[g_currentPlaylistIndex];

    // Check if playlist is empty
    if (playlist.songIds.empty())
        return;

    // Get current playing ID atomically
    int currentId = getCurrentPlayingId();
    if (currentId == -1) return;

    int nextId = get_next_song_in_playlist(currentId);

    // Don't preload if no next song or if it's the same song
    if (nextId != -1 && nextId != currentId)
    {
        std::string songPath;
        {
            std::lock_guard<std::mutex> lock(g_songMutex);
            auto it = std::find_if(g_allSongs.begin(), g_allSongs.end(),
                [nextId](const SongInfo& s) { return s.id == nextId; });
            if (it != g_allSongs.end())
            {
                songPath = it->path;
            }
        }

        if (!songPath.empty())
        {
            g_preloadingNext = true;
            std::thread preloadThread([nextId, path = std::move(songPath)]() {

                load_mp3(nextId, path);
                g_preloadingNext = false;
            });
            preloadThread.detach();
        }
    }
}

#endif

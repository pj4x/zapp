#ifndef ZAPP_DEF_H
#define ZAPP_DEF_H

#include <string.h>
#include <atomic>
#include <vector>
#include <mutex>

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

struct AudioPlayback
{
    float* pcm = nullptr;  // Pointer to PCM data (owned by cache)
    size_t pcmSize = 0;
    int channels = 0;
    int mp3SampleRate = 0;      // NEW: Sample rate from MP3 file
    int outputSampleRate = 0;   // NEW: Actual SDL output sample rate
    std::atomic<size_t> currentFrame = 0;
    size_t totalFrames = 0;
    std::atomic<bool> playing = false;
    std::mutex mutex;
    int currentSongId = -1;
};

// Audio cache structure
struct CachedAudio
{
    std::vector<float> pcm;
    int channels = 0;
    int sampleRate = 0;
    size_t totalFrames = 0;
    bool isValid = false;
    std::chrono::steady_clock::time_point lastUsed;
};

#endif

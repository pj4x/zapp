#ifndef ZAPP_AUDIO_H
#define ZAPP_AUDIO_H

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

// ------------------------------------------------------------
// Audio Callback (thread-safe)
// ------------------------------------------------------------

inline void audio_callback(void* userdata, Uint8* stream, int len)
{
    AudioPlayback* audio = (AudioPlayback*)userdata;

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

                // Set flag for main thread to increment play count
                g_incrementPlayCount = true;
                g_songToIncrement = getCurrentPlayingId();  // Use atomic getter
            }
            break;
        }

        out[i] = audio->pcm[currentSample++];
    }

    audio->currentFrame = currentSample / audio->channels;
}

#endif

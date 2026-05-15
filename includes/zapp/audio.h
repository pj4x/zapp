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
    int samplesPerFrame = audio->channels;

    size_t currentSample = audio->currentFrame * samplesPerFrame;
    size_t totalSamples = audio->totalFrames * samplesPerFrame;

    for (int i = 0; i < floatCount; i += samplesPerFrame)
    {
        if (currentSample >= totalSamples)
        {
            if (audio->playing)
            {
                audio->playing = false;
                g_requestNextSong = true;
                g_incrementPlayCount = true;
                g_songToIncrement = audio->currentSongId;
            }
            break;
        }

        // Copy the samples
        for (int c = 0; c < samplesPerFrame; c++)
        {
            if (currentSample + c < totalSamples)
                out[i + c] = audio->pcm[currentSample + c];
            else
                out[i + c] = 0.0f;
        }

        currentSample += samplesPerFrame;
    }

    // Update current frame based on samples consumed
    audio->currentFrame = currentSample / samplesPerFrame;
}

#endif

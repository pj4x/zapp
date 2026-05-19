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
#include "visualizer.h"

// ------------------------------------------------------------
// Audio Callback (thread-safe)
// ------------------------------------------------------------

inline void audio_callback(void* userdata, Uint8* stream, int len) {
    AudioPlayback* audio = (AudioPlayback*)userdata;

    SDL_memset(stream, 0, len);

    if (!audio->playing || !audio->pcm) {
        return;
    }

    std::lock_guard<std::mutex> lock(audio->mutex);

    float* out = (float*)stream;
    int floatCount = len / sizeof(float);
    int samplesPerFrame = audio->channels;

    size_t currentSample = audio->currentFrame * samplesPerFrame;
    size_t totalSamples = audio->totalFrames * samplesPerFrame;

    // Buffer for spectrum analysis
    static std::vector<float> spectrumBuffer;
    spectrumBuffer.clear();

    for (int i = 0; i < floatCount; i += samplesPerFrame) {
        if (currentSample >= totalSamples) {
            if (audio->playing) {
                audio->playing = false;
                g_requestNextSong = true;
                g_incrementPlayCount = true;
                g_songToIncrement = audio->currentSongId;
            }
            break;
        }

        // Copy samples
        for (int c = 0; c < samplesPerFrame && currentSample + c < totalSamples; c++) {
            out[i + c] = audio->pcm[currentSample + c];
            if (c == 0) {  // Use left channel for spectrum
                spectrumBuffer.push_back(std::abs(out[i + c]));
            }
        }

        currentSample += samplesPerFrame;
    }

    // Update spectrum visualization
    if (!spectrumBuffer.empty() && !g_minimized) {
        update_audio_spectrum_fft(spectrumBuffer.data(), spectrumBuffer.size(), 1);
    }

    audio->currentFrame = currentSample / samplesPerFrame;
}

#endif

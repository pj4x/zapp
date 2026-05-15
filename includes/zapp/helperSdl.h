#ifndef ZAPP_SDLHELP_H
#define ZAPP_SDLHELP_H

#include <SDL.h>
#include <string.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include "imgui.h"
#include "definitions.h"
#include "globals.h"
#include "audio.h"

// Reconfigure audio device for different sample rate/channels
inline bool ReconfigureAudio(int sampleRate, int channels)
{
    // Close current audio if open
    if (g_audioInitialized)
    {
        SDL_CloseAudio();
        g_audioInitialized = false;
    }

    SDL_AudioSpec spec;
    SDL_AudioSpec obtained;

    spec.freq = sampleRate;
    spec.format = AUDIO_F32;
    spec.channels = channels;
    spec.samples = 4096;
    spec.callback = audio_callback;
    spec.userdata = &gPlayback;

    if (SDL_OpenAudio(&spec, &obtained) < 0)
    {
        std::cerr << "Failed to open audio at " << sampleRate
                  << " Hz, " << channels << " channels: " << SDL_GetError() << std::endl;
        return false;
    }

    g_currentAudioSpec = obtained;
    g_audioInitialized = true;

    std::cout << "Audio reconfigured to " << obtained.freq << " Hz, "
              << obtained.channels << " channels" << std::endl;

    // Check if we got what we asked for
    if (obtained.freq != sampleRate)
    {
        std::cout << "WARNING: Requested " << sampleRate << " Hz but got "
                  << obtained.freq << " Hz" << std::endl;
    }

    SDL_PauseAudio(0);
    return true;
}

#endif

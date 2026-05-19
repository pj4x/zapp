#ifndef ZAPP_VIS_H
#define ZAPP_VIS_H

#include <vector>
#include "imgui.h"
#include "definitions.h"
#include "globals.h"
#include "helpers.h"
#include "helperSdl.h"
#include "database.h"

// Forward declarations
int getCurrentPlayingId();

namespace FFT {

    static void fft(float* real, float* imag, int n, bool invert) {
        if (n == 1) return;

        int n2 = n / 2;
        float* re_left = new float[n2];
        float* im_left = new float[n2];
        float* re_right = new float[n2];
        float* im_right = new float[n2];

        for (int i = 0; i < n2; i++) {
            re_left[i] = real[i * 2];
            im_left[i] = imag[i * 2];
            re_right[i] = real[i * 2 + 1];
            im_right[i] = imag[i * 2 + 1];
        }

        fft(re_left, im_left, n2, invert);
        fft(re_right, im_right, n2, invert);

        float ang = 2.0f * 3.14159265359f / n * (invert ? -1 : 1);
        float wreal = cosf(ang);
        float wimag = sinf(ang);
        float wr = 1.0f;
        float wi = 0.0f;

        for (int i = 0; i < n2; i++) {
            float re = wr * re_right[i] - wi * im_right[i];
            float im = wr * im_right[i] + wi * re_right[i];

            real[i] = re_left[i] + re;
            imag[i] = im_left[i] + im;
            real[i + n2] = re_left[i] - re;
            imag[i + n2] = im_left[i] - im;

            float wr_next = wr * wreal - wi * wimag;
            float wi_next = wr * wimag + wi * wreal;
            wr = wr_next;
            wi = wi_next;
        }

        delete[] re_left;
        delete[] im_left;
        delete[] re_right;
        delete[] im_right;
    }

    static void compute_magnitudes(const float* samples, int sampleCount, float* magnitudes, int magnitudeCount) {
        // FFT size (power of 2)
        int fftSize = 1024;
        while (fftSize > sampleCount) fftSize /= 2;
        if (fftSize < 32) fftSize = 32;

        // Prepare buffers
        float* real = new float[fftSize];
        float* imag = new float[fftSize];

        // Apply Hanning window and copy samples
        for (int i = 0; i < fftSize; i++) {
            float window = 0.5f * (1.0f - cosf(2.0f * 3.14159265359f * i / (fftSize - 1)));
            float sample = (i < sampleCount) ? samples[i] : 0;
            real[i] = sample * window;
            imag[i] = 0;
        }

        // Perform FFT
        fft(real, imag, fftSize, false);

        // Compute magnitudes and map to frequency bands
        int bands = magnitudeCount;
        for (int i = 0; i < bands; i++) {
            // Logarithmic frequency scaling (more bass resolution)
            float norm = (float)i / bands;
            int startIdx = (int)(norm * norm * fftSize / 2);
            int endIdx = (int)(((float)(i + 1) / bands) * ((float)(i + 1) / bands) * fftSize / 2);
            if (endIdx <= startIdx) endIdx = startIdx + 1;
            if (endIdx > fftSize / 2) endIdx = fftSize / 2;

            float sum = 0;
            for (int j = startIdx; j < endIdx && j < fftSize / 2; j++) {
                float magnitude = sqrtf(real[j] * real[j] + imag[j] * imag[j]);
                sum += magnitude;
            }

            float avg = sum / (endIdx - startIdx);
            // Scale for better visualization
            avg = avg * 0.1f;
            if (avg > 1.0f) avg = 1.0f;

            magnitudes[i] = avg;
        }

        delete[] real;
        delete[] imag;
    }
}

struct WaveformVisualizer {
    std::vector<float> waveformData;
    int currentSongId = -1;

    // FFT spectrum
    float fftMagnitudes[128] = {0};
    float fftPeaks[128] = {0};
    float fftDecay[128] = {0};
    float fftSmoothing[128] = {0};

    // Rolling history for smoother animation
    float history[128][8] = {{0}};
    int historyIndex = 0;

    // Settings
    float speed = 0.85f;
    int fftSize = 1024;
} gWaveform;

void update_audio_spectrum_fft(const float* pcm, int sampleCount, int channels) {
    if (sampleCount < 256) return;

    // Get mono samples (average channels)
    int monoCount = sampleCount / channels;
    float* mono = new float[monoCount];

    for (int i = 0; i < monoCount; i++) {
        float sum = 0;
        for (int c = 0; c < channels; c++) {
            if (i * channels + c < sampleCount) {
                sum += pcm[i * channels + c];
            }
        }
        mono[i] = sum / channels;
    }

    // Compute FFT magnitudes
    FFT::compute_magnitudes(mono, monoCount, gWaveform.fftMagnitudes, 128);

    delete[] mono;

    // Frequency compensation weights
    static float compensationWeights[128];
    static bool weightsInitialized = false;

    if (!weightsInitialized) {
        for (int i = 0; i < 128; i++) {
            float norm = (float)i / 127.0f;

            float compensation = 0.15f * expf(3.0f * norm);  // 0.15 to ~4.5

            // Clamp
            compensation = std::min(compensation, 6.0f);
            compensation = std::max(compensation, 0.1f);

            compensationWeights[i] = compensation;

            // Debug print
            if (i == 0 || i == 31 || i == 63 || i == 95 || i == 127) {
                printf("FFT band %d (norm=%.2f) compensation: %.2f\n", i, norm, compensation);
            }
        }
        weightsInitialized = true;
    }

    // Apply normalization and update peaks
    for (int i = 0; i < 128; i++) {
        // Apply frequency compensation
        float compensated = gWaveform.fftMagnitudes[i] * compensationWeights[i];

        // Additional dynamic range compression (makes quiet sounds louder)
        compensated = powf(compensated, 0.7f);  // Soft compression

        // Limit to 0-1 range
        if (compensated > 1.0f) compensated = 1.0f;

        // Rolling average for smoother animation
        gWaveform.history[i][gWaveform.historyIndex % 8] = compensated;
        float avg = 0;
        for (int h = 0; h < 8; h++) {
            avg += gWaveform.history[i][h];
        }
        avg /= 8;
        gWaveform.fftSmoothing[i] = avg;

        // Update peak with decay
        if (gWaveform.fftSmoothing[i] > gWaveform.fftPeaks[i]) {
            gWaveform.fftPeaks[i] = gWaveform.fftSmoothing[i];
            gWaveform.fftDecay[i] = 0;
        } else {
            gWaveform.fftDecay[i] += 0.01f;
            gWaveform.fftPeaks[i] = std::max(0.0f, gWaveform.fftPeaks[i] * 0.98f);
        }
    }

    gWaveform.historyIndex++;
}

void draw_fft_spectrum(float width, float height) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    // Background with gradient
    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                            IM_COL32(5, 5, 15, 255));

    // Border
    drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
                      IM_COL32(100, 100, 120, 255), 2.0f);

    if (!gPlayback.playing) {
        ImVec2 textSize = ImGui::CalcTextSize("No audio playing");
        ImVec2 textPos = ImVec2(pos.x + width/2 - textSize.x/2, pos.y + height/2 - textSize.y/2);
        drawList->AddText(textPos, IM_COL32(150, 150, 150, 200), "No audio playing");
        ImGui::Dummy(ImVec2(width, height));
        return;
    }

    int bars = 128;
    float barWidth = width / bars;


    float maxVal = 0.01f;
    for (int i = 0; i < bars; i++) {
        if (gWaveform.fftSmoothing[i] > maxVal) maxVal = gWaveform.fftSmoothing[i];
    }

    for (int i = 0; i < bars; i++) {
        // Use smoothed FFT magnitudes
        float magnitude = gWaveform.fftSmoothing[i];

        magnitude = magnitude / maxVal;
        if (magnitude > 1.0f) magnitude = 1.0f;

        float barHeight = magnitude * height;
        if (barHeight < 2.0f && magnitude > 0.01f) barHeight = 2.0f; // Minimum height

        float x = pos.x + i * barWidth;
        float y = pos.y + height - barHeight;

        // color gradient
        ImU32 color;

        float r = g_highlightColor.x;
        float g_c = g_highlightColor.y;
        float b = g_highlightColor.z;

        // Frequency-based brightness (darker left, brighter right)
        float freqNorm = (float)i / 127.0f;
        float brightness = powf(freqNorm, 1.4f);

        // Add magnitude pulse
        float magnitudePulse = magnitude * 0.8f;
        brightness = std::min(brightness + magnitudePulse, 1.0f);

        // Apply gamma-like curve for better visual distribution
        brightness = brightness * brightness;

        color = IM_COL32(
            (int)(r * brightness * 255),
            (int)(g_c * brightness * 255),
            (int)(b * brightness * 255),
            200
        );

        // Draw bar
        drawList->AddRectFilled(ImVec2(x, y),
                               ImVec2(x + barWidth - 1, pos.y + height),
                               color);

        // Add glow effect for peaks
        if (magnitude > 0.7f) {
            drawList->AddRectFilled(ImVec2(x, y),
                                   ImVec2(x + barWidth - 1, y + 4),
                                   IM_COL32(255, 255, 255, 100));
        }

        // Draw peak marker
        float peakHeight = gWaveform.fftPeaks[i] * height;
        if (peakHeight > barHeight + 2) {
            float peakY = pos.y + height - peakHeight;
            drawList->AddRectFilled(ImVec2(x, peakY - 2),
                                   ImVec2(x + barWidth, peakY),
                                   IM_COL32(255, 255, 255, 180));
        }
    }

    // frequency labels
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 180));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 5, pos.y + height - 15));
    ImGui::Text("20Hz");
    ImGui::SetCursorScreenPos(ImVec2(pos.x + width * 0.25f - 20, pos.y + height - 15));
    ImGui::Text("250Hz");
    ImGui::SetCursorScreenPos(ImVec2(pos.x + width * 0.5f - 20, pos.y + height - 15));
    ImGui::Text("2kHz");
    ImGui::SetCursorScreenPos(ImVec2(pos.x + width - 45, pos.y + height - 15));
    ImGui::Text("20kHz");
    ImGui::PopStyleColor();
}

#endif

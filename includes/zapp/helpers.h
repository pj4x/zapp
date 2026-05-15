#ifndef ZAPP_HELPERS_H
#define ZAPP_HELPERS_H

#include <string.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include "definitions.h"
#include "globals.h"

inline int getCurrentPlayingId() {
    return g_currentPlayingId.load();
}

inline SongInfo getCurrentPlayingMetadata() {
    std::lock_guard<std::mutex> lock(g_currentPlayingMetadataMutex);
    return g_currentPlayingMetadata;
}

inline void setCurrentPlaying(const SongInfo& song) {
    g_currentPlayingId.store(song.id);
    std::lock_guard<std::mutex> lock(g_currentPlayingMetadataMutex);
    g_currentPlayingMetadata = song;
}


inline std::string get_filename_without_ext(const std::string& path)
{
    std::filesystem::path p(path);
    return p.stem().string();
}

// Convert Latin-1 (ISO-8859-1) to UTF-8
inline std::string latin1_to_utf8(const std::string& latin1)
{
    std::string utf8;
    for (unsigned char c : latin1)
    {
        if (c < 0x80)
        {
            utf8 += c;
        }
        else
        {
            utf8 += static_cast<char>(0xC0 | (c >> 6));
            utf8 += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return utf8;
}

// Sanitize a string to ensure it contains only valid UTF-8 sequences
inline std::string sanitize_utf8(const std::string& s)
{
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); )
    {
        unsigned char c = s[i];
        if (c < 0x80)
        {
            // ASCII
            result += c;
            ++i;
        }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size())
        {
            // 2-byte UTF-8
            unsigned char c2 = s[i+1];
            if ((c2 & 0xC0) == 0x80)
            {
                result += c;
                result += c2;
                i += 2;
            }
            else
            {
                // invalid, skip
                ++i;
            }
        }
        else if ((c & 0xF0) == 0xE0 && i + 2 < s.size())
        {
            // 3-byte UTF-8
            unsigned char c2 = s[i+1];
            unsigned char c3 = s[i+2];
            if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80)
            {
                result += c;
                result += c2;
                result += c3;
                i += 3;
            }
            else
            {
                ++i;
            }
        }
        else if ((c & 0xF8) == 0xF0 && i + 3 < s.size())
        {
            // 4-byte UTF-8
            unsigned char c2 = s[i+1];
            unsigned char c3 = s[i+2];
            unsigned char c4 = s[i+3];
            if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80 && (c4 & 0xC0) == 0x80)
            {
                result += c;
                result += c2;
                result += c3;
                result += c4;
                i += 4;
            }
            else
            {
                ++i;
            }
        }
        else
        {
            // invalid byte, skip
            ++i;
        }
    }
    return result;
}

#endif

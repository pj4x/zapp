#ifndef ZAPP_GLOBALS_H
#define ZAPP_GLOBALS_H

#include <string.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
#include "definitions.h"

inline std::unordered_map<int, CachedAudio> g_audioCache;
inline std::mutex g_cacheMutex;
inline std::atomic<bool> g_preloadingNext = false;
inline const size_t MAX_CACHE_SIZE = 10; // Maximum number of songs to keep in cache (adjust as needed)
inline std::atomic<bool> g_hasPreloadedForCurrentSong = false;

inline std::vector<SongInfo> g_allSongs;          // All songs with IDs
inline std::vector<Playlist> g_playlists;         // All playlists (includes "library")
inline int g_currentPlaylistIndex = -1;           // Index of currently open playlist for playback
inline std::mutex g_songMutex;
inline std::atomic<bool> g_scanning = false;
inline std::string g_scanStatus = "";
inline std::atomic<bool> g_requestNextSong = false;  // Set by audio thread when song ends

// global current songinfo
inline AudioPlayback gPlayback;
inline std::atomic<int> g_currentPlayingId{-1};
inline std::mutex g_currentPlayingMetadataMutex;
inline SongInfo g_currentPlayingMetadata;

// For "Add to playlist" popup
inline SongInfo g_selectedSongForOptions;
inline bool g_showSongOptionsPopup = false;
inline bool g_showAddToPlaylistPopup = false;

// For "Add from Library" popup
inline bool g_showAddFromLibraryPopup = false;

// For async saving
inline std::atomic<bool> g_savingSongs = false;
inline std::atomic<bool> g_savingPlaylists = false;
inline std::string g_saveStatus = "";
inline std::mutex g_saveMutex;

// for play count increase
inline std::atomic<bool> g_incrementPlayCount = false;
inline std::atomic<int> g_songToIncrement = -1;

#endif

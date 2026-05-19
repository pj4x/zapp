#ifndef ZAPP_GLOBALS_H
#define ZAPP_GLOBALS_H

#include <SDL.h>
#include <string.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
#include "imgui.h"
#include "definitions.h"

inline std::unordered_map<int, CachedAudio> g_audioCache;
inline std::mutex g_cacheMutex;
inline std::atomic<bool> g_preloadingNext = false;
inline std::atomic<bool> g_hasPreloadedForCurrentSong = false;

inline std::vector<SongInfo> g_allSongs;          // All songs with IDs
inline std::vector<Playlist> g_playlists;         // All playlists (includes "library")
inline std::mutex g_playlistsMutex;
inline int g_currentPlaylistIndex = -1;           // Index of currently open playlist for playback
inline int g_viewPlaylistIndex = -1;
inline std::mutex g_songMutex;
inline std::atomic<bool> g_scanning = false;
inline std::string g_scanStatus = "";
inline std::atomic<bool> g_requestNextSong = false;  // Set by audio thread when song ends
inline bool g_minimized = false;

// audio specs
inline SDL_AudioSpec g_currentAudioSpec;
inline bool g_audioInitialized = false;

// global current songinfo
inline AudioPlayback gPlayback;
inline std::atomic<int> g_currentPlayingId{-1};
inline std::mutex g_currentPlayingMetadataMutex;
inline SongInfo g_currentPlayingMetadata;

// Song options and add to playlist popup
inline SongInfo g_selectedSongForOptions;
inline bool g_showSongOptionsPopup = false;
inline bool g_showAddToPlaylistPopup = false;

// Add from Library popup
inline bool g_showAddFromLibraryPopup = false;

// settings popup
inline bool g_showSettingsPopup = false;
// Settings
inline static int g_currentTheme = 0;  // 0=Dark, 1=Light, 2=High Contrast
inline const char* g_themeNames[] = { "Dark Theme", "Light Theme", "High Contrast" };
inline static int g_cacheSizeSetting = 10;
inline static ImVec4 g_highlightColor = ImVec4(0.56f, 0.84f, 1.00f, 1.00f);  // Default: light-blue
inline bool g_showArtistName = true;
inline bool g_shuffle = false;
inline bool g_nextIdRand = false;
inline int g_RandId = -1;

// For async saving
inline std::atomic<bool> g_savingSongs = false;
inline std::atomic<bool> g_savingPlaylists = false;
inline std::string g_saveStatus = "";
inline std::mutex g_saveMutex;

// for play count increase
inline std::atomic<bool> g_incrementPlayCount = false;
inline std::atomic<int> g_songToIncrement = -1;

// ui update
inline bool g_songlistUpdate = false;

#endif

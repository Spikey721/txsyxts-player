#pragma once

#include "spotify.hpp"
#include <string>
#include <vector>

namespace txs {

// yt-dlp based search and stream URL extraction
std::vector<Track> yt_search(const std::string& query, int max_results = 5);

// Get similar songs from a youtube mix
std::vector<Track> yt_get_similar(const std::string& uri, int max_results = 5);

// Fetch tracks from a direct URL (video or playlist)
std::vector<Track> yt_fetch_playlist(const std::string& url, int max_results = 100);

// Extract direct stream URL
std::string yt_stream_url(const std::string& video_url);

// auto-recommendations — search for related songs
std::vector<Track> yt_recommend(const std::string& artist, const std::string& title, int count = 5);

// ytfzf direct play
bool ytfzf_available();
int ytfzf_play(const std::string& query); // returns pid, 0 on failure

// local files
std::vector<Track> scan_local(const std::string& directory);

} // namespace txs

#pragma once

#include "spotify.hpp"
#include <string>
#include <vector>

namespace txs {

// ── YouTube / yt-dlp ──────────────────────────────────────────────────────────
std::vector<Track> yt_search(const std::string& query, int max_results = 5);
std::vector<Track> yt_get_similar(const std::string& uri, int max_results = 5);
std::vector<Track> yt_fetch_playlist(const std::string& url, int max_results = 100);
std::string        yt_stream_url(const std::string& video_url);
std::vector<Track> yt_recommend(const std::string& artist, const std::string& title, int count = 5);

// ytfzf (optional external tool)
bool ytfzf_available();
int  ytfzf_play(const std::string& query); // returns pid, 0 on failure

// Local file scanner
std::vector<Track> scan_local(const std::string& directory);

// ── Download ──────────────────────────────────────────────────────────────────
// Downloads a YouTube URL to ~/Music/ as best-audio.
// Returns the output filename, empty string on failure.
// Runs asynchronously via popen; progress is shown via the returned command string.
std::string yt_download(const std::string& video_url, const std::string& out_dir = "");

// ── Lyrics ────────────────────────────────────────────────────────────────────
struct LyricLine {
    double time_sec;
    std::string text;
};
struct Lyrics {
    std::vector<LyricLine> lines;
    std::string plain;
    bool empty() const { return lines.empty() && plain.empty(); }
};

// Fetches lyrics from lrclib.net (free, no auth). Returns formatted text or
// an error message string.
Lyrics fetch_lyrics(const std::string& artist, const std::string& title);

// ── Podcast / RSS ─────────────────────────────────────────────────────────────
// Parse a podcast RSS feed URL and return episodes as Track objects.
// Each Track's uri is the direct audio enclosure URL.
std::vector<Track> fetch_podcast(const std::string& rss_url, int max_episodes = 20);

// ── yt-dlp version check ──────────────────────────────────────────────────────
// Returns the installed yt-dlp version string, e.g. "2024.11.18".
// Returns "" if yt-dlp is not found.
std::string ytdlp_version();
// Returns true if yt-dlp version is at least min_version (date-based, e.g. "2024.01.01")
bool ytdlp_is_recent(const std::string& min_version = "2024.01.01");

} // namespace txs

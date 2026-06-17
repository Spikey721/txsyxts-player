#include "youtube.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <algorithm>
#include <unistd.h>
#include <signal.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace txs {

// ── shell exec helper ──────────────────────────────────────────

static std::string exec_cmd(const std::string& cmd) {
    std::array<char, 4096> buffer;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    return result;
}

// ── yt-dlp search ──────────────────────────────────────────────

std::vector<Track> yt_search(const std::string& query, int max_results) {
    std::vector<Track> tracks;

    // use yt-dlp to search and dump JSON
    std::string cmd = "yt-dlp --dump-json --flat-playlist --no-warnings -q "
                      "\"ytsearch" + std::to_string(max_results) + ":" + query + "\" 2>/dev/null";
    std::string output = exec_cmd(cmd);
    if (output.empty()) return tracks;

    // each line is a separate JSON object
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            Track t;
            t.title = j.value("title", "Unknown");
            t.artist = j.value("uploader", j.value("channel", ""));
            t.duration_ms = j.value("duration", 0) * 1000;
            t.source = "youtube";
            t.uri = j.value("url", j.value("webpage_url", ""));
            if (t.uri.empty() && j.contains("id")) {
                t.uri = "https://www.youtube.com/watch?v=" + j["id"].get<std::string>();
            }
            if (j.contains("thumbnails") && j["thumbnails"].is_array() && !j["thumbnails"].empty()) {
                auto thumbs = j["thumbnails"];
                std::string raw_url = thumbs.back().value("url", "");
                auto pos = raw_url.find("?");
                if (pos != std::string::npos) raw_url = raw_url.substr(0, pos);
                t.thumbnail_url = raw_url;
            }
            tracks.push_back(t);
        } catch (...) {}
    }
    return tracks;
}

// ── yt-dlp similar mix ─────────────────────────────────────────

std::vector<Track> yt_get_similar(const std::string& uri, int max_results) {
    std::vector<Track> tracks;

    // Extract video ID
    std::string vid;
    auto pos = uri.find("v=");
    if (pos != std::string::npos) {
        vid = uri.substr(pos + 2);
        auto amp = vid.find('&');
        if (amp != std::string::npos) vid = vid.substr(0, amp);
    } else {
        auto pos2 = uri.find("youtu.be/");
        if (pos2 != std::string::npos) {
            vid = uri.substr(pos2 + 9);
            auto q = vid.find('?');
            if (q != std::string::npos) vid = vid.substr(0, q);
        }
    }

    if (vid.empty()) return tracks;

    // fetch Mix playlist
    std::string mix_url = "https://www.youtube.com/watch?v=" + vid + "&list=RD" + vid;
    std::string cmd = "yt-dlp --dump-json --flat-playlist --playlist-end " + std::to_string(max_results) + 
                      " --no-warnings -q \"" + mix_url + "\" 2>/dev/null";
                      
    std::string output = exec_cmd(cmd);
    if (output.empty()) return tracks;

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            Track t;
            t.title = j.value("title", "Unknown");
            t.artist = j.value("uploader", j.value("channel", ""));
            t.duration_ms = j.value("duration", 0) * 1000;
            t.source = "youtube";
            t.uri = j.value("url", j.value("webpage_url", ""));
            if (t.uri.empty() && j.contains("id")) {
                t.uri = "https://www.youtube.com/watch?v=" + j["id"].get<std::string>();
            }
            if (j.contains("thumbnails") && j["thumbnails"].is_array() && !j["thumbnails"].empty()) {
                auto thumbs = j["thumbnails"];
                std::string raw_url = thumbs.back().value("url", "");
                auto qpos = raw_url.find("?");
                if (qpos != std::string::npos) raw_url = raw_url.substr(0, qpos);
                t.thumbnail_url = raw_url;
            }
            tracks.push_back(t);
        } catch (...) {}
    }
    return tracks;
}

std::vector<Track> yt_fetch_playlist(const std::string& url, int max_results) {
    std::vector<Track> tracks;
    
    std::string cmd = "yt-dlp --dump-json --flat-playlist " + 
                      (max_results > 0 ? "--playlist-end " + std::to_string(max_results) : "") + 
                      " --no-warnings -q \"" + url + "\" 2>/dev/null";
                      
    std::string output = exec_cmd(cmd);
    if (output.empty()) return tracks;

    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            Track t;
            t.title = j.value("title", "Unknown");
            t.artist = j.value("uploader", j.value("channel", ""));
            t.duration_ms = j.value("duration", 0) * 1000;
            t.source = "youtube";
            t.uri = j.value("url", j.value("webpage_url", ""));
            if (t.uri.empty() && j.contains("id")) {
                t.uri = "https://www.youtube.com/watch?v=" + j["id"].get<std::string>();
            }
            if (j.contains("thumbnails") && j["thumbnails"].is_array() && !j["thumbnails"].empty()) {
                auto thumbs = j["thumbnails"];
                std::string raw_url = thumbs.back().value("url", "");
                auto qpos = raw_url.find("?");
                if (qpos != std::string::npos) raw_url = raw_url.substr(0, qpos);
                t.thumbnail_url = raw_url;
            }
            tracks.push_back(t);
        } catch (...) {}
    }
    return tracks;
}

// ── yt-dlp stream URL extraction ───────────────────────────────

std::string yt_stream_url(const std::string& video_url) {
    std::string cmd = "yt-dlp -f \"ba/b\" -g --no-warnings -q \""
                    + video_url + "\" 2>/dev/null";
    std::string url = exec_cmd(cmd);
    // trim whitespace
    while (!url.empty() && (url.back() == '\n' || url.back() == '\r' || url.back() == ' '))
        url.pop_back();
    return url;
}

// ── ytfzf ──────────────────────────────────────────────────────

bool ytfzf_available() {
    return system("command -v ytfzf >/dev/null 2>&1") == 0;
}

int ytfzf_play(const std::string& query) {
    if (!ytfzf_available()) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        // child — redirect output
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp("ytfzf", "ytfzf", "-a", "-m", query.c_str(), nullptr);
        _exit(1);
    }
    return pid > 0 ? pid : 0;
}

// ── auto-recommendations ───────────────────────────────────────

std::vector<Track> yt_recommend(const std::string& artist, const std::string& title, int count) {
    // build a search query that finds related music
    std::string query;
    if (!artist.empty() && !title.empty()) {
        query = artist + " " + title + " similar songs mix";
    } else if (!title.empty()) {
        query = title + " similar songs mix";
    } else {
        query = "popular music mix";
    }
    return yt_search(query, count);
}

// ── local files ────────────────────────────────────────────────

std::vector<Track> scan_local(const std::string& directory) {
    std::vector<Track> tracks;
    std::string path = directory;

    // expand ~
    if (!path.empty() && path[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) path = std::string(home) + path.substr(1);
    }

    if (!fs::is_directory(path)) return tracks;

    std::vector<std::string> exts = {".mp3", ".flac", ".ogg", ".opus", ".m4a", ".wav"};

    for (auto& entry : fs::directory_iterator(path)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        bool match = false;
        for (auto& e : exts) {
            if (ext == e) { match = true; break; }
        }
        if (!match) continue;

        Track t;
        std::string stem = entry.path().stem().string();

        // try "Artist - Title" format
        auto pos = stem.find(" - ");
        if (pos != std::string::npos) {
            t.artist = stem.substr(0, pos);
            t.title = stem.substr(pos + 3);
        } else {
            t.title = stem;
        }

        t.source = "local";
        t.uri = entry.path().string();
        tracks.push_back(t);
    }

    std::sort(tracks.begin(), tracks.end(),
              [](const Track& a, const Track& b) { return a.title < b.title; });
    return tracks;
}

} // namespace txs

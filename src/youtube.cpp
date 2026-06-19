#include "youtube.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <signal.h>
#include <curl/curl.h>

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
// Tries multiple format selectors and retries up to 3 times with
// exponential backoff to handle YouTube rate-limiting and transient failures.

std::string yt_stream_url(const std::string& video_url) {
    // Format preference: m4a audio → webm audio → any audio → best
    static const char* fmts[] = {
        "ba[ext=m4a]/ba[ext=webm]/ba",
        "bestaudio/best",
        "b"
    };

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            // 2s on 1st retry, 4s on 2nd
            std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
        }

        for (const char* fmt : fmts) {
            std::string cmd = std::string("yt-dlp -f \"") + fmt +
                              "\" -g --no-warnings -q \"" + video_url + "\" 2>/dev/null";
            std::string url = exec_cmd(cmd);
            while (!url.empty() && (url.back() == '\n' || url.back() == '\r' || url.back() == ' '))
                url.pop_back();
            if (!url.empty()) return url;
        }
    }
    return "";
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

// ── Download ──────────────────────────────────────────────────────────────────

std::string yt_download(const std::string& video_url, const std::string& out_dir) {
    std::string dir = out_dir;
    if (dir.empty()) {
        const char* home = std::getenv("HOME");
        dir = home ? std::string(home) + "/Music" : "/tmp";
    }
    // Create dir if needed
    fs::create_directories(dir);

    std::string cmd = "yt-dlp -f \"ba[ext=m4a]/ba/b\" --no-warnings -q "
                      "--output \"" + dir + "/%(artist)s - %(title)s.%(ext)s\" "
                      "\"" + video_url + "\" 2>/dev/null";
    // Run and return the output path (we derive it from title after download)
    int ret = system(cmd.c_str());
    return ret == 0 ? dir : "";
}

// ── Lyrics ────────────────────────────────────────────────────────────────────
// Uses lrclib.net — completely free, no auth, returns plain text lyrics

static size_t lyrics_write_cb(void* c, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(static_cast<char*>(c), s * n);
    return s * n;
}

static std::string url_encode_simple(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += c;
        else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

Lyrics fetch_lyrics(const std::string& artist, const std::string& title) {
    Lyrics result;
    if (artist.empty() && title.empty()) {
        result.plain = "No track info available.";
        return result;
    }

    std::string q = artist + " " + title;
    std::string url = "https://lrclib.net/api/search?q=" + url_encode_simple(q);

    std::string response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.plain = "Failed to init curl.";
        return result;
    }

    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, lyrics_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "txsyxts/0.1");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // Bypass CA cert issues
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        result.plain = "Network error: " + std::string(errbuf[0] ? errbuf : curl_easy_strerror(res));
        return result;
    }
    
    if (response.empty()) {
        result.plain = "Empty response from lyrics server.";
        return result;
    }

    try {
        auto j = nlohmann::json::parse(response);
        if (j.is_array() && !j.empty()) {
            auto first = j[0];
            if (first.contains("plainLyrics") && !first["plainLyrics"].is_null()) {
                result.plain = first["plainLyrics"].get<std::string>();
            }
            if (first.contains("syncedLyrics") && !first["syncedLyrics"].is_null()) {
                std::string raw = first["syncedLyrics"].get<std::string>();
            std::istringstream ss(raw);
            std::string line;
            while (std::getline(ss, line)) {
                auto rb = line.find(']');
                if (line.size() > 1 && line[0] == '[' && rb != std::string::npos) {
                    std::string ts = line.substr(1, rb - 1);
                    std::string text = line.substr(rb + 1);
                    if (!text.empty() && text[0] == ' ') text = text.substr(1);
                    
                    auto colon = ts.find(':');
                    if (colon != std::string::npos) {
                        try {
                            double m = std::stod(ts.substr(0, colon));
                            double s = std::stod(ts.substr(colon + 1));
                            result.lines.push_back({m * 60.0 + s, text});
                        } catch (...) {}
                    }
                }
            }
            }
        }
    } catch (...) {}

    if (result.empty()) {
        result.plain = "Lyrics not found for: " + artist + " - " + title;
    }
    return result;
}

// ── Podcast RSS ───────────────────────────────────────────────────────────────
// Minimal XML parser — extracts <item> blocks and reads title + enclosure url

static std::string xml_extract(const std::string& xml,
                               const std::string& open_tag,
                               const std::string& close_tag,
                               size_t from = 0) {
    auto start = xml.find(open_tag, from);
    if (start == std::string::npos) return "";
    start += open_tag.size();
    auto end = xml.find(close_tag, start);
    if (end == std::string::npos) return "";
    return xml.substr(start, end - start);
}

static std::string xml_attr(const std::string& tag_text, const std::string& attr) {
    auto pos = tag_text.find(attr + "=\"");
    if (pos == std::string::npos) return "";
    pos += attr.size() + 2;
    auto end = tag_text.find('"', pos);
    if (end == std::string::npos) return "";
    return tag_text.substr(pos, end - pos);
}

std::vector<Track> fetch_podcast(const std::string& rss_url, int max_episodes) {
    std::vector<Track> episodes;

    std::string feed;
    CURL* curl = curl_easy_init();
    if (!curl) return episodes;
    curl_easy_setopt(curl, CURLOPT_URL, rss_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, lyrics_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &feed);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "txsyxts/0.1");
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (feed.empty()) return episodes;

    // Extract podcast title for artist field
    std::string podcast_title = xml_extract(feed, "<title>", "</title>");
    // Strip CDATA if present
    if (podcast_title.find("<![CDATA[") == 0)
        podcast_title = podcast_title.substr(9, podcast_title.size() - 12);

    size_t pos = 0;
    int count = 0;
    while (count < max_episodes) {
        auto item_start = feed.find("<item>", pos);
        if (item_start == std::string::npos) break;
        auto item_end = feed.find("</item>", item_start);
        if (item_end == std::string::npos) break;
        std::string item = feed.substr(item_start, item_end - item_start);
        pos = item_end + 7;

        Track t;
        t.source = "podcast";
        t.artist = podcast_title;

        // Title
        t.title = xml_extract(item, "<title>", "</title>");
        if (t.title.find("<![CDATA[") == 0)
            t.title = t.title.substr(9, t.title.size() - 12);

        // Audio URL from <enclosure url="..." .../>
        auto enc_pos = item.find("<enclosure");
        if (enc_pos != std::string::npos) {
            auto enc_end = item.find("/>", enc_pos);
            std::string enc_tag = item.substr(enc_pos, enc_end - enc_pos + 2);
            t.uri = xml_attr(enc_tag, "url");
        }

        if (t.uri.empty() || t.title.empty()) { pos++; continue; }
        episodes.push_back(t);
        ++count;
    }
    return episodes;
}

// ── yt-dlp version check ──────────────────────────────────────────────────────

std::string ytdlp_version() {
    std::string ver = exec_cmd("yt-dlp --version 2>/dev/null");
    while (!ver.empty() && (ver.back() == '\n' || ver.back() == '\r' || ver.back() == ' '))
        ver.pop_back();
    return ver;
}

bool ytdlp_is_recent(const std::string& min_version) {
    std::string ver = ytdlp_version();
    if (ver.empty()) return false;
    // yt-dlp versions are YYYY.MM.DD — lexicographic compare works
    return ver >= min_version;
}

} // namespace txs


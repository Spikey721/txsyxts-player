#include "youtube.hpp"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cctype>
#include <unistd.h>
#include <signal.h>
#include <curl/curl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <cerrno>

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

// ── yt-dlp command builder ─────────────────────────────────────
// Python-based tools (yt-dlp, pip) try IPv6 first and, when the
// machine has an AAAA address but broken IPv6 connectivity, they
// hang forever instead of falling back. Detect that once with a
// short probe, and force yt-dlp onto IPv4 if needed. Every call
// also gets a hard `timeout` so a hung process can never leave
// the UI stuck on "searching...".

static bool ipv6_probe_fails() {
    struct addrinfo hints {};
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo("www.youtube.com", "443", &hints, &res) != 0 || !res)
        return false; // no AAAA records — default IPv4 path is fine

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return false; } // no IPv6 stack — fine

    bool broken = false;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        if (errno != EINPROGRESS) {
            broken = true;
        } else {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            struct timeval tv { 2, 0 }; // 2s probe
            if (select(fd + 1, nullptr, &wset, nullptr, &tv) <= 0) {
                broken = true; // timed out
            } else {
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                broken = (err != 0);
            }
        }
    }
    close(fd);
    freeaddrinfo(res);
    return broken;
}

static std::string ytdlp_cmd(const std::string& args, int timeout_s) {
    static const bool force_v4 = ipv6_probe_fails(); // probed once
    return std::string("timeout ") + std::to_string(timeout_s) +
           " yt-dlp " + (force_v4 ? "--source-address 0.0.0.0 " : "") + args;
}

// ── yt-dlp search ──────────────────────────────────────────────

std::vector<Track> yt_search(const std::string& query, int max_results) {
    std::vector<Track> tracks;

    // use yt-dlp to search and dump JSON
    std::string cmd = ytdlp_cmd("--dump-json --flat-playlist --no-warnings -q --age-limit 99 "
                      "\"ytsearch" + std::to_string(max_results) + ":" + query + "\" "
                      "2>>/tmp/txsyxts-ytdlp.log; echo \"[search] rc=$? q=" + query + "\" >> /tmp/txsyxts-ytdlp.log", 60);
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
    std::string cmd = ytdlp_cmd("--dump-json --flat-playlist --playlist-end " + std::to_string(max_results) + 
                      " --no-warnings -q --age-limit 99 \"" + mix_url + "\" 2>/dev/null", 60);
                      
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
    
    std::string cmd = ytdlp_cmd("--dump-json --flat-playlist " + 
                      (max_results > 0 ? "--playlist-end " + std::to_string(max_results) : "") + 
                      " --no-warnings -q --age-limit 99 \"" + url + "\" 2>/dev/null", 90);
                      
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

// ── stream warm-up ─────────────────────────────────────────────
// A freshly generated googlevideo URL answers HTTP 403 for the first
// few seconds of its life (the edge node has not started serving that
// object yet). yt-dlp hides this in its own download path with a 4s
// wait before starting the transfer; mpv does not retry, so it hits
// the 403, gives up, and the player behaves as if the track instantly
// "ended" (which is what made tracks get skipped). Here we touch the
// URL ourselves, retrying until the server actually answers, so the
// URL handed to mpv is already live.

static size_t warmup_discard_cb(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

static bool warm_up_stream(const std::string& url, int attempts = 12) {
    if (url.rfind("http", 0) != 0) return true; // local file / other scheme

    CURL* curl = curl_easy_init();
    if (!curl) return true; // cannot verify — let mpv try anyway

    long code = 0;
    for (int i = 0; i < attempts; ++i) {
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_RANGE, "0-1023");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, warmup_discard_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36");

        code = 0;
        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (res == CURLE_OK && (code == 200 || code == 206)) {
            curl_easy_cleanup(curl);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    curl_easy_cleanup(curl);
    return false;
}

// ── yt-dlp stream URL extraction ───────────────────────────────
// Tries multiple format selectors and retries with exponential backoff
// to handle YouTube rate-limiting and transient failures. Every URL is
// warmed up before being returned (see above).

std::string yt_stream_url(const std::string& video_url) {
    // Format preference: m4a audio → webm audio → any audio → best
    static const char* fmts[] = {
        "ba[ext=m4a]/ba[ext=webm]/ba",
        "bestaudio/best",
        "b"
    };

    std::string fallback;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) {
            // 2s on 1st retry, 4s on 2nd
            std::this_thread::sleep_for(std::chrono::seconds(2 * attempt));
        }

        for (const char* fmt : fmts) {
            std::string cmd = ytdlp_cmd(std::string("-f \"") + fmt +
                              "\" -g --no-warnings -q --age-limit 99 \"" + video_url + "\" "
                              "2>>/tmp/txsyxts-ytdlp.log; echo \"[stream] rc=$? fmt=" + std::string(fmt) +
                              " uri=" + video_url + "\" >> /tmp/txsyxts-ytdlp.log", 30);
            std::string url = exec_cmd(cmd);
            while (!url.empty() && (url.back() == '\n' || url.back() == '\r' || url.back() == ' '))
                url.pop_back();
            if (url.empty()) continue;

            // The URL only becomes fetchable a moment after generation.
            // Wait until it actually serves data before handing it to mpv.
            if (warm_up_stream(url)) {
                std::ofstream("/tmp/txsyxts-ytdlp.log", std::ios::app)
                    << "[stream] warmed ok\n";
                return url;
            }

            // Still refusing — remember it and try again with a fresh URL.
            if (fallback.empty()) fallback = url;
            std::ofstream("/tmp/txsyxts-ytdlp.log", std::ios::app)
                << "[stream] warmup failed, re-resolving\n";
            break; // format selector worked; don't burn time on the others
        }
    }
    return fallback;
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

    std::string cmd = ytdlp_cmd("-f \"ba[ext=m4a]/ba/b\" --no-warnings -q --age-limit 99 "
                      "--output \"" + dir + "/%(artist)s - %(title)s.%(ext)s\" "
                      "\"" + video_url + "\" 2>/dev/null", 900);
    // Run and return the output path (we derive it from title after download)
    int ret = system(cmd.c_str());
    return ret == 0 ? dir : "";
}

// ── Lyrics ────────────────────────────────────────────────────────────────────
// Provider chain, same idea as the DMS shell: lrclib → KuGou → lyricsplus.
// lrclib is the best source (clean LRC timings, no auth). KuGou covers a lot
// of material lrclib misses and also returns LRC. lyricsplus aggregates Apple
// Music / QQ / Deezer. We keep going until timed lines are found; a plain-only
// result is kept as a fallback so the lyrics pane is never blank when
// something was actually found.

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

static std::string lyr_trim(const std::string& in) {
    size_t a = 0, b = in.size();
    while (a < b && isspace((unsigned char)in[a])) ++a;
    while (b > a && isspace((unsigned char)in[b - 1])) --b;
    return in.substr(a, b - a);
}

// Small HTTP GET helper shared by all providers. Returns "" on any error so
// callers can just move on to the next provider.
static std::string http_get(const std::string& url, long timeout = 15) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, lyrics_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (X11; Linux x86_64) txsyxts/0.1");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || code >= 400) return "";
    return response;
}

// KuGou ships lyric files base64-encoded.
static std::string b64_decode(const std::string& in) {
    static const char* tbl_src =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int8_t tbl[256];
    memset(tbl, -1, sizeof(tbl));
    for (int i = 0; i < 64; ++i) tbl[(unsigned char)tbl_src[i]] = (int8_t)i;

    std::string out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        int8_t d = tbl[c];
        if (d < 0) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((val >> bits) & 0xFF);
        }
    }
    return out;
}

// ── metadata cleaning ─────────────────────────────────────────────────────────
// YouTube titles carry a lot of noise ("(Official Video)", "[HD]", "| Lyrics").
// Every provider matches markedly better once that is stripped.

static bool lyr_ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static bool lyric_junk_segment(const std::string& seg) {
    static const char* junk[] = {
        "official", "video", "audio", "lyric", "visualizer", "remaster",
        "hd", "hq", "4k", "8k", "m/v", "music video", "topic", "from the",
        "soundtrack", "original", "explicit", "clean", "live", "edit",
        "version", "full song", "color coded", "prod."
    };
    std::string l = seg;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    for (auto* j : junk)
        if (l.find(j) != std::string::npos) return true;
    return false;
}

static std::string strip_groups(const std::string& s, bool only_junk) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '(' || c == '[' || c == '{') {
            char close = c == '(' ? ')' : (c == '[' ? ']' : '}');
            size_t rb = s.find(close, i + 1);
            if (rb != std::string::npos) {
                std::string seg = s.substr(i + 1, rb - i - 1);
                if (!only_junk || lyric_junk_segment(seg)) {
                    i = rb + 1;
                    out += ' ';
                    continue;
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

static void clean_meta(std::string& artist, std::string& title) {
    std::string t = lyr_trim(strip_groups(title, true));
    if (t.empty()) t = lyr_trim(strip_groups(title, false));

    // "Song | Some Channel" — the part after the pipe is rarely the song name
    auto bar = t.find(" | ");
    if (bar != std::string::npos) t = lyr_trim(t.substr(0, bar));
    t = lyr_trim(t);
    if (!t.empty()) title = t;

    std::string a = artist;
    for (const char* suf : {" - Topic", "VEVO", " - Official", " Official"})
        if (lyr_ends_with(a, suf)) a = lyr_trim(a.substr(0, a.size() - strlen(suf)));
    for (const char* ft : {" feat. ", " ft. ", " Feat. ", " featuring "}) {
        auto p = a.find(ft);
        if (p != std::string::npos) a = lyr_trim(a.substr(0, p));
    }
    artist = a;
}

// ── LRC parsing ──────────────────────────────────────────────────────────────
// Handles [mm:ss.xx], [mm:ss:xx], several timestamps per line and [offset:..].

static void parse_lrc(const std::string& raw, std::vector<LyricLine>& out) {
    int offset_ms = 0;
    std::istringstream ss(raw);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        size_t i = 0;
        bool any_tag = false;
        std::vector<double> times;

        while (i < line.size() && line[i] == '[') {
            auto rb = line.find(']', i + 1);
            if (rb == std::string::npos) break;
            std::string tag = line.substr(i + 1, rb - i - 1);

            if (tag.rfind("offset:", 0) == 0) {
                offset_ms = atoi(tag.c_str() + 7);
                i = rb + 1;
                any_tag = true;
                continue;
            }

            auto c1 = tag.find(':');
            if (c1 == std::string::npos || c1 == 0 || !isdigit((unsigned char)tag[0])) break;

            bool numeric = true;
            for (size_t k = 0; k < c1; ++k)
                if (!isdigit((unsigned char)tag[k])) { numeric = false; break; }
            if (!numeric) break;

            std::string rest = tag.substr(c1 + 1);
            if (rest.empty()) break;

            double sec;
            auto c2 = rest.find(':');
            if (c2 != std::string::npos) // [mm:ss:centiseconds]
                sec = atof(rest.substr(0, c2).c_str()) +
                      atof(rest.substr(c2 + 1).c_str()) / 100.0;
            else
                sec = atof(rest.c_str());

            times.push_back(atoi(tag.substr(0, c1).c_str()) * 60.0 + sec);
            i = rb + 1;
            any_tag = true;
        }

        if (!any_tag || times.empty()) continue;

        std::string text = lyr_trim(line.substr(i));
        if (text.empty()) continue;

        for (double t : times)
            out.push_back({t + offset_ms / 1000.0, text});
    }

    std::sort(out.begin(), out.end(),
              [](const LyricLine& a, const LyricLine& b) { return a.time_sec < b.time_sec; });
}

// ── provider 1: lrclib ────────────────────────────────────────────────────────

static void lrclib_fill(const json& o, Lyrics& out) {
    out.source = "lrclib";
    out.instrumental = o.value("instrumental", false);

    if (o.contains("syncedLyrics") && !o["syncedLyrics"].is_null())
        parse_lrc(o["syncedLyrics"].get<std::string>(), out.lines);

    if (o.contains("plainLyrics") && !o["plainLyrics"].is_null()) {
        std::string plain = o["plainLyrics"].get<std::string>();
        // some entries ship LRC inside plainLyrics
        if (out.lines.empty() && plain.find("[0") != std::string::npos)
            parse_lrc(plain, out.lines);
        if (out.lines.empty()) out.plain = plain;
    }

    if (out.instrumental && out.empty()) out.plain = "\u266a instrumental";
}

static bool lrclib_exact(const std::string& artist, const std::string& title,
                         int dur, Lyrics& out) {
    std::string u = "https://lrclib.net/api/get?artist_name=" + url_encode_simple(artist) +
                    "&track_name=" + url_encode_simple(title);
    if (dur > 0) u += "&duration=" + std::to_string(dur);

    auto body = http_get(u, 12);
    if (body.empty()) return false;

    json j;
    try { j = json::parse(body); } catch (...) { return false; }
    if (!j.is_object()) return false;

    lrclib_fill(j, out);
    return !out.empty();
}

static bool lrclib_search(const std::string& q, int dur, Lyrics& out) {
    auto body = http_get("https://lrclib.net/api/search?q=" + url_encode_simple(q), 12);
    if (body.empty()) return false;

    json j;
    try { j = json::parse(body); } catch (...) { return false; }
    if (!j.is_array() || j.empty()) return false;

    int best = -1, best_score = -1000;
    for (int i = 0; i < (int)j.size() && i < 12; ++i) {
        const auto& o = j[i];
        int score = 0;

        bool synced = o.contains("syncedLyrics") && !o["syncedLyrics"].is_null() &&
                      o["syncedLyrics"].get<std::string>().size() > 20;
        if (synced) score += 4;
        if (o.value("instrumental", false)) score += 1;

        double d = o.value("duration", 0.0);
        if (dur > 0 && d > 1.0) {
            double diff = std::abs(d - (double)dur);
            if (diff <= 2.0) score += 5;
            else if (diff <= 5.0) score += 3;
            else if (diff > 15.0) score -= 5;
        }

        if (score > best_score) { best_score = score; best = i; }
    }

    if (best < 0) return false;
    lrclib_fill(j[best], out);
    return !out.empty();
}

// ── provider 2: KuGou ────────────────────────────────────────────────────────
// Search the song, look up its lyric document by hash+duration, then download
// the LRC. KuGou's catalogue is huge, which is why the DMS shell uses it.

static bool lyrics_from_kugou(const std::string& artist, const std::string& title,
                              int dur, Lyrics& out) {
    std::string q = artist.empty() ? title : artist + " " + title;
    auto body = http_get("https://mobileservice.kugou.com/api/v3/search/song"
                         "?format=json&page=1&pagesize=6&showtype=1&keyword=" +
                         url_encode_simple(q), 12);
    if (body.empty()) return false;

    json j;
    try { j = json::parse(body); } catch (...) { return false; }
    if (!j.contains("data") || !j["data"].contains("info") ||
        !j["data"]["info"].is_array()) return false;

    int tried = 0;
    for (const auto& s : j["data"]["info"]) {
        if (tried++ >= 4) break;

        std::string hash = s.value("hash", "");
        std::string name = s.value("songname", title);
        int sdur = s.value("duration", 0);
        if (hash.empty()) continue;
        // skip wildly different versions (live/karaoke/remix)
        if (dur > 0 && sdur > 0 && std::abs(sdur / 1000 - dur) > 20) continue;

        auto sb = http_get("https://lyrics.kugou.com/search?ver=1&man=yes&client=pc"
                           "&keyword=" + url_encode_simple(name) +
                           "&duration=" + std::to_string(sdur) + "&hash=" + hash, 12);
        if (sb.empty()) continue;

        json sj;
        try { sj = json::parse(sb); } catch (...) { continue; }
        if (!sj.contains("candidates") || !sj["candidates"].is_array() ||
            sj["candidates"].empty()) continue;

        const auto& c = sj["candidates"][0];
        std::string id = c.value("id", ""), key = c.value("accesskey", "");
        if (id.empty() || key.empty()) continue;

        auto db = http_get("https://lyrics.kugou.com/download?ver=1&client=pc&fmt=lrc"
                           "&charset=utf8&id=" + id + "&accesskey=" + key, 12);
        if (db.empty()) continue;

        json dj;
        try { dj = json::parse(db); } catch (...) { continue; }
        if (!dj.contains("content") || !dj["content"].is_string()) continue;

        std::vector<LyricLine> lines;
        parse_lrc(b64_decode(dj["content"].get<std::string>()), lines);
        if (lines.empty()) continue;

        // KuGou LRC files usually open with a "Title - Artist" credit line
        auto is_credit = [&](const LyricLine& l) {
            if (l.time_sec > 5.0) return false;
            std::string t = l.text, tt = title;
            for (auto& ch : t) ch = (char)tolower((unsigned char)ch);
            for (auto& ch : tt) ch = (char)tolower((unsigned char)ch);
            return !tt.empty() && t.find(tt) != std::string::npos && t.find(" - ") != std::string::npos;
        };
        lines.erase(std::remove_if(lines.begin(), lines.end(), is_credit), lines.end());
        if (lines.empty()) continue;

        // KuGou marks instrumentals with a single placeholder line
        // ("纯音乐，请欣赏" / "instrumental"). Treat that as a real result
        // instead of showing one useless line.
        if (lines.size() <= 1) {
            std::string only = lines.empty() ? "" : lines[0].text;
            std::string lower = only;
            for (auto& ch : lower) ch = (char)tolower((unsigned char)ch);
            if (lower.find("instrumental") != std::string::npos ||
                only.find("\xe7\xba\xaf\xe9\x9f\xb3\xe4\xb9\x90") != std::string::npos) {
                out.lines.clear();
                out.instrumental = true;
                out.plain = "\u266a instrumental";
                out.source = "kugou";
                return true;
            }
            continue;
        }

        out.lines = std::move(lines);
        out.source = "kugou";
        return true;
    }
    return false;
}

// ── provider 3: lyricsplus (Apple Music / QQ / Deezer) ───────────────────────

static bool lyrics_from_lyricsplus(const std::string& artist, const std::string& title,
                                   int dur, Lyrics& out) {
    std::string u = "https://lyricsplus.binimum.org/v2/lyrics/get?title=" +
                    url_encode_simple(title) + "&artist=" + url_encode_simple(artist);
    if (dur > 0) u += "&duration=" + std::to_string(dur);

    auto body = http_get(u, 25);
    if (body.empty()) return false;

    json j;
    try { j = json::parse(body); } catch (...) { return false; }
    if (!j.contains("lyrics") || !j["lyrics"].is_array()) return false;

    std::vector<LyricLine> lines;
    for (const auto& l : j["lyrics"]) {
        std::string txt = lyr_trim(l.value("text", ""));
        if (txt.empty()) continue;
        long long ms = l.value("time", (long long)0);
        lines.push_back({(double)ms / 1000.0, txt});
    }
    if (lines.empty()) return false;

    std::sort(lines.begin(), lines.end(),
              [](const LyricLine& a, const LyricLine& b) { return a.time_sec < b.time_sec; });

    out.lines = std::move(lines);
    out.source = "lyricsplus";
    return true;
}

Lyrics fetch_lyrics(const std::string& artist_in, const std::string& title_in,
                    int duration_sec) {
    Lyrics result;
    if (artist_in.empty() && title_in.empty()) {
        result.plain = "No track info available.";
        return result;
    }

    std::string artist = artist_in, title = title_in;
    clean_meta(artist, title);

    // 1) lrclib — exact lookup (duration makes it precise), then search
    if (lrclib_exact(artist, title, duration_sec, result) && result.synced())
        return result;
    if (lrclib_search(artist + " " + title, duration_sec, result) && result.synced())
        return result;

    // remember a plain/instrumental hit while the other providers are tried
    std::string plain_fallback = result.plain;
    bool instrumental = result.instrumental;
    std::string plain_source = result.source;

    // 2) KuGou
    Lyrics kg;
    if (lyrics_from_kugou(artist, title, duration_sec, kg)) return kg;

    // 3) lyricsplus — slowest, so it goes last
    Lyrics lp;
    if (lyrics_from_lyricsplus(artist, title, duration_sec, lp)) return lp;

    if (!plain_fallback.empty() || instrumental) {
        result.lines.clear();
        result.plain = plain_fallback;
        result.instrumental = instrumental;
        result.source = plain_source;
        return result;
    }

    // 4) last try: title alone — uploaders often disagree about "the artist"
    Lyrics t_only;
    if (lrclib_search(title, duration_sec, t_only) && t_only.synced()) return t_only;

    result.plain = "Lyrics not found for: " + artist + " - " + title;
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


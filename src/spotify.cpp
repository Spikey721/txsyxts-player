#include "spotify.hpp"
#include "config.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <chrono>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace txs {

// ── Track ──────────────────────────────────────────────────────

std::string Track::duration_str() const {
    if (duration_ms <= 0) return "";
    int total_sec = duration_ms / 1000;
    int m = total_sec / 60;
    int s = total_sec % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

std::string Track::search_query() const {
    return artist + " - " + title;
}

std::string Track::display() const {
    if (artist.empty()) return title;
    return artist + " — " + title;
}

// ── curl callback ──────────────────────────────────────────────

static size_t write_cb(void* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// ── Spotify ────────────────────────────────────────────────────

Spotify::Spotify() = default;

bool Spotify::is_configured() const { return !sp_dc_.empty(); }

bool Spotify::is_logged_in() const {
    if (access_token_.empty()) return false;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return now_ms < token_expiry_;
}

void Spotify::set_sp_dc(const std::string& cookie) { sp_dc_ = cookie; }

void Spotify::set_token(const std::string& token, long long expiry_ms) {
    access_token_ = token;
    token_expiry_ = expiry_ms;
}

std::string Spotify::http_get(const std::string& url,
                               const std::string& cookie,
                               const std::string& bearer) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers,
        "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");

    if (!bearer.empty()) {
        std::string auth = "Authorization: Bearer " + bearer;
        headers = curl_slist_append(headers, auth.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (!cookie.empty()) {
        std::string ck = "sp_dc=" + cookie;
        curl_easy_setopt(curl, CURLOPT_COOKIE, ck.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return "";
    return response;
}

std::string Spotify::fetch_token(const std::string& auth_code, const std::string& code_verifier) {
    auto cfg = txs::Config::load();
    
    // If we have an sp_dc cookie, use the Web Player token endpoint!
    if (!sp_dc_.empty() && auth_code.empty()) {
        std::string url = "https://open.spotify.com/get_access_token?reason=transport&productType=web_player";
        
        CURL* curl = curl_easy_init();
        if (!curl) return "";

        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        std::string clean_sp = sp_dc_;
        if (clean_sp.front() == '"') clean_sp = clean_sp.substr(1);
        if (clean_sp.back() == '"') clean_sp.pop_back();

        std::string ck = "sp_dc=" + clean_sp;
        curl_easy_setopt(curl, CURLOPT_COOKIE, ck.c_str());

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "App-Platform: WebPlayer");
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) throw std::runtime_error("sp_dc token fetch failed");

        if (response.empty() || response[0] == '<') {
            throw std::runtime_error("Spotify rejected the cookie! Make sure you copied it exactly right and DID NOT log out of your browser.");
        }

        try {
            auto j = json::parse(response);
            if (j.contains("isAnonymous") && j["isAnonymous"] == true) {
                throw std::runtime_error("sp_dc cookie is expired! Go back to browser, refresh, and get a new one.");
            }

            access_token_ = j.value("accessToken", "");
            token_expiry_ = j.value("accessTokenExpirationTimestampMs", 0LL);
        } catch (const json::parse_error& e) {
            throw std::runtime_error("Spotify returned an invalid response. Your cookie is likely malformed.");
        }
        
        return access_token_;
    }

    // Fallback to OAuth PKCE if using auth code
    std::string url = "https://accounts.spotify.com/api/token";
    std::string body;
    if (!auth_code.empty()) {
        body = "grant_type=authorization_code&client_id=" + cfg.client_id + "&code=" + auth_code + 
               "&redirect_uri=http://localhost:4304/auth/spotify/callback&code_verifier=" + code_verifier;
    } else {
        throw std::runtime_error("No auth code or sp_dc available");
    }

    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) throw std::runtime_error("token request failed");

    auto j = json::parse(response);
    if (j.contains("error")) {
        throw std::runtime_error(j.value("error_description", "unknown error"));
    }

    access_token_ = j.value("access_token", "");
    int expires_in = j.value("expires_in", 3600);
    
    auto now = std::chrono::system_clock::now().time_since_epoch();
    long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    token_expiry_ = now_ms + (expires_in * 1000);

    if (j.contains("refresh_token")) {
        sp_dc_ = j.value("refresh_token", "");
        // save to config automatically
        auto cfg = txs::Config::load();
        cfg.sp_dc = sp_dc_;
        cfg.save();
    }

    return access_token_;
}

bool Spotify::ensure_token() {
    if (is_logged_in()) return true;
    if (!is_configured()) return false;
    
    try {
        fetch_token("", "");
        return is_logged_in();
    } catch (...) {
        return false;
    }
}

Spotify::LoginResult Spotify::login() {
    if (!is_configured()) {
        return {false, "run :login to open spotify in browser"};
    }

    if (!is_logged_in()) {
        if (!ensure_token()) {
            return {false, "failed to fetch token. maybe config is invalid?"};
        }
    }

    return login_with_token();
}

Spotify::LoginResult Spotify::login_with_token() {
    if (access_token_.empty()) {
        return {false, "no token set"};
    }
    try {
        std::string resp = api_get("/me");
        if (!resp.empty()) {
            auto j = json::parse(resp);
            std::string name = j.value("display_name", j.value("id", "unknown"));
            return {true, "logged in as " + name};
        }
        return {true, "logged in"};
    } catch (const std::exception& e) {
        std::string raw_resp = api_get("/me");
        // Limit raw_resp size to prevent UI overflow
        if (raw_resp.size() > 200) raw_resp = raw_resp.substr(0, 200) + "...";
        return {false, std::string("token verify failed: ") + e.what() + " | RAW: " + raw_resp};
    }
}

std::string Spotify::api_get(const std::string& endpoint) {
    if (!is_logged_in()) return "";
    std::string url = "https://api.spotify.com/v1" + endpoint;
    std::string resp = http_get(url, "", access_token_);
    
    // DEBUG: Dump to log file
    FILE* f = fopen("/tmp/txs_api.log", "a");
    if (f) {
        fprintf(f, "GET %s\nRESP: %s\n\n", url.c_str(), resp.c_str());
        fclose(f);
    }

    // safety: if response looks like HTML, return empty
    if (!resp.empty() && (resp[0] == '<' || resp.find("<!DOCTYPE") != std::string::npos)) {
        return "";
    }
    return resp;
}

std::vector<Track> Spotify::get_liked_songs(int limit) {
    std::vector<Track> tracks;
    if (!ensure_token()) return tracks;

    int offset = 0;
    int batch = std::min(limit, 50);

    while ((int)tracks.size() < limit) {
        std::string url = "/me/tracks?limit=" + std::to_string(batch)
                        + "&offset=" + std::to_string(offset);
        std::string resp = api_get(url);
        if (resp.empty()) break;

        try {
            auto j = json::parse(resp);
            auto items = j.value("items", json::array());
            if (items.empty()) break;

            for (auto& item : items) {
                auto t = item.value("track", json::object());
                if (t.is_null()) continue;

                Track track;
                track.title = t.value("name", "Unknown");

                // artists
                std::string artists;
                for (auto& a : t.value("artists", json::array())) {
                    if (!artists.empty()) artists += ", ";
                    artists += a.value("name", "");
                }
                track.artist = artists;

                auto album = t.value("album", json::object());
                track.album = album.value("name", "");
                
                if (album.contains("images") && album["images"].is_array() && !album["images"].empty()) {
                    track.thumbnail_url = album["images"][0].value("url", "");
                }

                track.duration_ms = t.value("duration_ms", 0);
                track.source = "spotify";
                track.uri = t.value("uri", "");
                tracks.push_back(track);
            }

            if (j.value("next", "").empty() || items.empty()) break;
            offset += batch;
        } catch (...) { break; }
    }

    if ((int)tracks.size() > limit) tracks.resize(limit);
    return tracks;
}

std::vector<std::pair<std::string,std::string>> Spotify::get_playlists() {
    std::vector<std::pair<std::string,std::string>> result;
    if (!ensure_token()) return result;

    std::string resp = api_get("/me/playlists?limit=50");
    if (resp.empty()) return result;

    try {
        auto j = json::parse(resp);
        for (auto& item : j.value("items", json::array())) {
            result.emplace_back(item.value("name", ""), item.value("id", ""));
        }
    } catch (...) {}
    return result;
}

std::vector<Track> Spotify::get_playlist_tracks(const std::string& id, int limit) {
    std::vector<Track> tracks;
    if (!ensure_token()) return tracks;

    int offset = 0;
    int batch = std::min(limit, 100);

    while ((int)tracks.size() < limit) {
        std::string url = "/playlists/" + id + "/tracks?limit="
                        + std::to_string(batch) + "&offset=" + std::to_string(offset);
        std::string resp = api_get(url);
        if (resp.empty()) break;

        try {
            auto j = json::parse(resp);
            auto items = j.value("items", json::array());
            if (items.empty()) break;

            for (auto& item : items) {
                auto t = item.value("track", json::object());
                if (t.is_null()) continue;

                Track track;
                track.title = t.value("name", "Unknown");
                std::string artists;
                for (auto& a : t.value("artists", json::array())) {
                    if (!artists.empty()) artists += ", ";
                    artists += a.value("name", "");
                }
                track.artist = artists;

                auto album = t.value("album", json::object());
                if (album.contains("images") && album["images"].is_array() && !album["images"].empty()) {
                    track.thumbnail_url = album["images"][0].value("url", "");
                }

                track.duration_ms = t.value("duration_ms", 0);
                track.source = "spotify";
                track.uri = t.value("uri", "");
                tracks.push_back(track);
            }

            if (j.value("next", "").empty()) break;
            offset += batch;
        } catch (...) { break; }
    }

    if ((int)tracks.size() > limit) tracks.resize(limit);
    return tracks;
}

} // namespace txs

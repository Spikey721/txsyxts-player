#include "scrobbler.hpp"
#include <curl/curl.h>
#include <openssl/md5.h>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <ctime>
#include <vector>
#include <algorithm>
#include <map>

namespace txs {

static size_t scrobble_write_cb(void* contents, size_t size, size_t nmemb, void*) {
    return size * nmemb; // discard response body
}

// MD5 hex digest
static std::string md5hex(const std::string& s) {
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char*>(s.c_str()), s.size(), digest);
    std::ostringstream oss;
    for (int i = 0; i < MD5_DIGEST_LENGTH; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return oss.str();
}

// URL-encode a string (simple)
static std::string url_encode(const std::string& s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            oss << c;
        } else {
            oss << '%' << std::hex << std::uppercase << std::setw(2)
                << std::setfill('0') << (int)c;
        }
    }
    return oss.str();
}

void Scrobbler::configure(const std::string& api_key,
                          const std::string& api_secret,
                          const std::string& session_key) {
    api_key_     = api_key;
    api_secret_  = api_secret;
    session_key_ = session_key;
}

bool Scrobbler::is_configured() const {
    return !api_key_.empty() && !api_secret_.empty() && !session_key_.empty();
}

// Build Last.fm API signature:
// sort params alphabetically, concatenate key+value pairs, append secret, md5
std::string Scrobbler::sign(const std::string& params_str) const {
    return md5hex(params_str + api_secret_);
}

bool Scrobbler::post(const std::string& body) const {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, "https://ws.audioscrobbler.com/2.0/");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, scrobble_write_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return res == CURLE_OK;
}

void Scrobbler::now_playing(const Track& track) {
    if (!is_configured()) return;

    // Build sorted param map for signature
    std::map<std::string, std::string> params = {
        {"api_key",  api_key_},
        {"artist",   track.artist},
        {"method",   "track.updateNowPlaying"},
        {"sk",       session_key_},
        {"track",    track.title},
    };
    if (!track.album.empty()) params["album"] = track.album;

    // Build signature string (sorted, no & between pairs for signing)
    std::string sig_str;
    for (auto& [k, v] : params) sig_str += k + v;
    std::string api_sig = sign(sig_str);

    // Build POST body
    std::string body;
    for (auto& [k, v] : params)
        body += url_encode(k) + "=" + url_encode(v) + "&";
    body += "api_sig=" + api_sig + "&format=json";

    post(body);
}

void Scrobbler::scrobble(const Track& track, std::time_t started_at) {
    if (!is_configured()) return;

    std::map<std::string, std::string> params = {
        {"api_key",   api_key_},
        {"artist",    track.artist},
        {"method",    "track.scrobble"},
        {"sk",        session_key_},
        {"timestamp", std::to_string(started_at)},
        {"track",     track.title},
    };
    if (!track.album.empty()) params["album"] = track.album;

    std::string sig_str;
    for (auto& [k, v] : params) sig_str += k + v;
    std::string api_sig = sign(sig_str);

    std::string body;
    for (auto& [k, v] : params)
        body += url_encode(k) + "=" + url_encode(v) + "&";
    body += "api_sig=" + api_sig + "&format=json";

    post(body);
}

} // namespace txs

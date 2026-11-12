#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace txs {

struct Config {
    // spotify
    std::string sp_dc;
    std::string client_id;      // Spotify API app client ID (free from developer.spotify.com)
    std::string client_secret;  // Spotify API app client secret

    // player
    int volume = 70;
    std::string audio_quality = "bestaudio";

    // last.fm scrobbling (optional)
    std::string lastfm_api_key;
    std::string lastfm_api_secret;
    std::string lastfm_session_key;

    // features
    bool sponsorblock_enabled = true;
    bool notify_enabled       = true;   // desktop notifications
    bool scrobble_enabled     = false;  // last.fm scrobbling

    // paths
    static std::string config_dir();
    static std::string config_file();
    static std::string token_cache_file();

    // load / save
    static Config load();
    void save() const;
};

} // namespace txs

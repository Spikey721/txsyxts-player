#pragma once
// Last.fm scrobbling — records tracks you listen to on Last.fm
// Config: set lastfm_session_key in :config, get it from https://www.last.fm/api/auth/
#include "spotify.hpp"
#include <string>
#include <ctime>

namespace txs {

class Scrobbler {
public:
    // api_key and api_secret from https://www.last.fm/api/account/create
    // session_key from Last.fm web auth flow (stored in config)
    Scrobbler() = default;

    void configure(const std::string& api_key,
                   const std::string& api_secret,
                   const std::string& session_key);

    bool is_configured() const;

    // Call when a track starts playing
    void now_playing(const Track& track);

    // Call after 30s or half the track duration (whichever is less)
    void scrobble(const Track& track, std::time_t started_at);

private:
    std::string api_key_;
    std::string api_secret_;
    std::string session_key_;

    std::string sign(const std::string& params_str) const;
    bool post(const std::string& body) const;
};

} // namespace txs

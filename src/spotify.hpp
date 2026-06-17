#pragma once

#include <string>
#include <vector>
#include <functional>

namespace txs {

struct Track {
    std::string title;
    std::string artist;
    std::string album;
    int duration_ms = 0;
    std::string source; // "spotify", "youtube", "local"
    std::string uri;    // spotify URI, youtube URL, or file path
    std::string thumbnail_url;

    std::string duration_str() const;
    std::string search_query() const;
    std::string display() const;
};

class Spotify {
public:
    Spotify();

    bool is_configured() const;
    bool is_logged_in() const;

    // set sp_dc cookie
    void set_sp_dc(const std::string& cookie);

    // set a pre-fetched access token (from browser login)
    void set_token(const std::string& token, long long expiry_ms);

    // login — exchange sp_dc for access token
    struct LoginResult { bool ok; std::string message; };
    LoginResult login();
    LoginResult login_with_token(); // verify existing token via /me

    std::string fetch_token(const std::string& auth_code, const std::string& code_verifier);

    // fetch library
    std::vector<Track> get_liked_songs(int limit = 50);
    std::vector<std::pair<std::string,std::string>> get_playlists(); // name, id
    std::vector<Track> get_playlist_tracks(const std::string& id, int limit = 100);

private:
    std::string sp_dc_;
    std::string access_token_;
    long long token_expiry_ = 0; // unix timestamp ms

    bool ensure_token();
    std::string api_get(const std::string& endpoint);
    static std::string http_get(const std::string& url,
                                const std::string& cookie = "",
                                const std::string& bearer = "");
};

} // namespace txs

#include "config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace txs {

std::string Config::config_dir() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.config/txsyxts";
}

std::string Config::config_file()       { return config_dir() + "/config.json"; }
std::string Config::token_cache_file()  { return config_dir() + "/sp_token_cache.json"; }

Config Config::load() {
    Config cfg;
    fs::create_directories(config_dir());

    std::ifstream f(config_file());
    if (f.is_open()) {
        try {
            nlohmann::json j;
            f >> j;
            if (j.contains("sp_dc"))             cfg.sp_dc               = j["sp_dc"].get<std::string>();
            if (j.contains("volume"))             cfg.volume              = j["volume"].get<int>();
            if (j.contains("audio_quality"))      cfg.audio_quality       = j["audio_quality"].get<std::string>();
            if (j.contains("client_id"))          cfg.client_id           = j["client_id"].get<std::string>();
            if (j.contains("lastfm_api_key"))     cfg.lastfm_api_key      = j["lastfm_api_key"].get<std::string>();
            if (j.contains("lastfm_api_secret"))  cfg.lastfm_api_secret   = j["lastfm_api_secret"].get<std::string>();
            if (j.contains("lastfm_session_key")) cfg.lastfm_session_key  = j["lastfm_session_key"].get<std::string>();
            if (j.contains("sponsorblock"))       cfg.sponsorblock_enabled= j["sponsorblock"].get<bool>();
            if (j.contains("notify"))             cfg.notify_enabled      = j["notify"].get<bool>();
            if (j.contains("scrobble"))           cfg.scrobble_enabled    = j["scrobble"].get<bool>();
        } catch (...) {}
    }
    return cfg;
}

void Config::save() const {
    fs::create_directories(config_dir());

    nlohmann::json j;
    j["sp_dc"]              = sp_dc;
    j["volume"]             = volume;
    j["audio_quality"]      = audio_quality;
    if (!client_id.empty())          j["client_id"]           = client_id;
    if (!lastfm_api_key.empty())     j["lastfm_api_key"]      = lastfm_api_key;
    if (!lastfm_api_secret.empty())  j["lastfm_api_secret"]   = lastfm_api_secret;
    if (!lastfm_session_key.empty()) j["lastfm_session_key"]  = lastfm_session_key;
    j["sponsorblock"]       = sponsorblock_enabled;
    j["notify"]             = notify_enabled;
    j["scrobble"]           = scrobble_enabled;

    std::ofstream f(config_file());
    if (f.is_open()) f << j.dump(2);
}

} // namespace txs

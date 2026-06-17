#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace txs {

struct Config {
    // spotify
    std::string sp_dc;
    std::string client_id = "9a8d2f0ce77a4e248bf7169b71af4a1f"; // Default to spotube

    // player
    int volume = 70;
    std::string audio_quality = "bestaudio";

    // paths
    static std::string config_dir();
    static std::string config_file();
    static std::string token_cache_file();

    // load / save
    static Config load();
    void save() const;
};

} // namespace txs

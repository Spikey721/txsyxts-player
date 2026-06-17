#include "config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace txs {

std::string Config::config_dir() {
  const char *home = std::getenv("HOME");
  if (!home)
    home = "/tmp";
  return std::string(home) + "/.config/txsyxts";
}

std::string Config::config_file() { return config_dir() + "/config.json"; }

std::string Config::token_cache_file() {
  return config_dir() + "/sp_token_cache.json";
}

Config Config::load() {
  Config cfg;
  fs::create_directories(config_dir());

  std::ifstream f(config_file());
  if (f.is_open()) {
    try {
      nlohmann::json j;
      f >> j;
      if (j.contains("sp_dc"))
        cfg.sp_dc = j["sp_dc"].get<std::string>();
      if (j.contains("volume"))
        cfg.volume = j["volume"].get<int>();
      if (j.contains("audio_quality"))
        cfg.audio_quality = j["audio_quality"].get<std::string>();
      if (j.contains("client_id"))
        cfg.client_id = j.value("client_id", "");
    } catch (...) {
    }
  }
  return cfg;
}

void Config::save() const {
  fs::create_directories(config_dir());

  nlohmann::json j;
  j["sp_dc"] = sp_dc;
  j["volume"] = volume;
  j["audio_quality"] = audio_quality;
  if (!client_id.empty())
    j["client_id"] = client_id;

  std::ofstream f(config_file());
  if (f.is_open()) {
    f << j.dump(2);
  }
}

} // namespace txs

#include "local_playlists.hpp"
#include "config.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace txs {

LocalPlaylists::LocalPlaylists() {
    file_path_ = Config::config_dir() + "/local_playlists.json";
    load();
}

void LocalPlaylists::load() {
    std::ifstream f(file_path_);
    if (!f.is_open()) return;

    try {
        json j;
        f >> j;

        playlists_.clear();
        for (auto& [name, tracks_json] : j.items()) {
            std::vector<Track> tracks;
            if (tracks_json.is_array()) {
                for (auto& t_json : tracks_json) {
                    Track t;
                    t.title = t_json.value("title", "");
                    t.artist = t_json.value("artist", "");
                    t.album = t_json.value("album", "");
                    t.duration_ms = t_json.value("duration_ms", 0);
                    t.source = t_json.value("source", "");
                    t.uri = t_json.value("uri", "");
                    t.thumbnail_url = t_json.value("thumbnail_url", "");
                    tracks.push_back(t);
                }
            }
            playlists_[name] = tracks;
        }
    } catch (...) {
        // failed to parse or read
    }
}

void LocalPlaylists::save() const {
    json j;
    for (const auto& [name, tracks] : playlists_) {
        json tracks_json = json::array();
        for (const auto& t : tracks) {
            tracks_json.push_back({
                {"title", t.title},
                {"artist", t.artist},
                {"album", t.album},
                {"duration_ms", t.duration_ms},
                {"source", t.source},
                {"uri", t.uri},
                {"thumbnail_url", t.thumbnail_url}
            });
        }
        j[name] = tracks_json;
    }

    std::ofstream f(file_path_);
    if (f.is_open()) {
        f << j.dump(4);
    }
}

std::vector<std::string> LocalPlaylists::get_names() const {
    std::vector<std::string> names;
    for (const auto& kv : playlists_) {
        names.push_back(kv.first);
    }
    return names;
}

std::vector<Track> LocalPlaylists::get_tracks(const std::string& name) const {
    auto it = playlists_.find(name);
    if (it != playlists_.end()) {
        return it->second;
    }
    return {};
}

bool LocalPlaylists::create(const std::string& name) {
    if (name.empty()) return false;
    if (playlists_.find(name) != playlists_.end()) return false;
    playlists_[name] = {};
    save();
    return true;
}

bool LocalPlaylists::remove(const std::string& name) {
    auto it = playlists_.find(name);
    if (it != playlists_.end()) {
        playlists_.erase(it);
        save();
        return true;
    }
    return false;
}

bool LocalPlaylists::add_track(const std::string& name, const Track& track) {
    if (name.empty()) return false;
    // auto-create if it doesn't exist
    playlists_[name].push_back(track);
    save();
    return true;
}

bool LocalPlaylists::remove_track(const std::string& name, int index) {
    auto it = playlists_.find(name);
    if (it != playlists_.end()) {
        if (index >= 0 && index < (int)it->second.size()) {
            it->second.erase(it->second.begin() + index);
            save();
            return true;
        }
    }
    return false;
}

} // namespace txs

#include "local_playlists.hpp"
#include "config.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <algorithm>

using json = nlohmann::json;

namespace txs {

LocalPlaylists::LocalPlaylists() {
    file_path_      = Config::config_dir() + "/local_playlists.json";
    tags_file_path_ = Config::config_dir() + "/track_tags.json";
    load();
    load_tags();
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
                    t.title         = t_json.value("title", "");
                    t.artist        = t_json.value("artist", "");
                    t.album         = t_json.value("album", "");
                    t.duration_ms   = t_json.value("duration_ms", 0);
                    t.source        = t_json.value("source", "");
                    t.uri           = t_json.value("uri", "");
                    t.thumbnail_url = t_json.value("thumbnail_url", "");
                    tracks.push_back(t);
                }
            }
            playlists_[name] = tracks;
        }
    } catch (...) {}
}

void LocalPlaylists::save() const {
    json j;
    for (const auto& [name, tracks] : playlists_) {
        json tracks_json = json::array();
        for (const auto& t : tracks) {
            tracks_json.push_back({
                {"title",         t.title},
                {"artist",        t.artist},
                {"album",         t.album},
                {"duration_ms",   t.duration_ms},
                {"source",        t.source},
                {"uri",           t.uri},
                {"thumbnail_url", t.thumbnail_url}
            });
        }
        j[name] = tracks_json;
    }

    std::ofstream f(file_path_);
    if (f.is_open()) f << j.dump(4);
}

void LocalPlaylists::load_tags() {
    std::ifstream f(tags_file_path_);
    if (!f.is_open()) return;
    try {
        json j;
        f >> j;
        tags_.clear();
        for (auto& [uri, tags_json] : j.items()) {
            std::vector<std::string> tags;
            if (tags_json.is_array())
                for (auto& t : tags_json)
                    tags.push_back(t.get<std::string>());
            tags_[uri] = tags;
        }
    } catch (...) {}
}

void LocalPlaylists::save_tags() const {
    json j;
    for (const auto& [uri, tags] : tags_) {
        json tags_json = json::array();
        for (const auto& t : tags) tags_json.push_back(t);
        j[uri] = tags_json;
    }
    std::ofstream f(tags_file_path_);
    if (f.is_open()) f << j.dump(2);
}

std::vector<std::string> LocalPlaylists::get_names() const {
    std::vector<std::string> names;
    for (const auto& kv : playlists_) names.push_back(kv.first);
    return names;
}

std::vector<Track> LocalPlaylists::get_tracks(const std::string& name) const {
    auto it = playlists_.find(name);
    if (it != playlists_.end()) return it->second;
    return {};
}

bool LocalPlaylists::create(const std::string& name) {
    if (name.empty() || playlists_.count(name)) return false;
    playlists_[name] = {};
    save();
    return true;
}

bool LocalPlaylists::remove(const std::string& name) {
    auto it = playlists_.find(name);
    if (it == playlists_.end()) return false;
    playlists_.erase(it);
    save();
    return true;
}

bool LocalPlaylists::rename(const std::string& old_name, const std::string& new_name) {
    if (old_name.empty() || new_name.empty()) return false;
    if (playlists_.count(new_name)) return false; // new name already exists
    auto it = playlists_.find(old_name);
    if (it == playlists_.end()) return false;
    playlists_[new_name] = std::move(it->second);
    playlists_.erase(it);
    save();
    return true;
}

bool LocalPlaylists::add_track(const std::string& name, const Track& track) {
    if (name.empty()) return false;
    playlists_[name].push_back(track);
    save();
    return true;
}

bool LocalPlaylists::remove_track(const std::string& name, int index) {
    auto it = playlists_.find(name);
    if (it == playlists_.end()) return false;
    if (index < 0 || index >= (int)it->second.size()) return false;
    it->second.erase(it->second.begin() + index);
    save();
    return true;
}

bool LocalPlaylists::export_m3u(const std::string& playlist_name,
                                 const std::string& out_path) const {
    auto it = playlists_.find(playlist_name);
    if (it == playlists_.end()) return false;

    std::ofstream f(out_path);
    if (!f.is_open()) return false;

    f << "#EXTM3U\n";
    for (const auto& t : it->second) {
        int dur_sec = t.duration_ms > 0 ? t.duration_ms / 1000 : -1;
        std::string display = t.artist.empty() ? t.title : t.artist + " - " + t.title;
        f << "#EXTINF:" << dur_sec << "," << display << "\n";
        f << t.uri << "\n";
    }
    return true;
}

// ── Tag management ────────────────────────────────────────────────────────────

void LocalPlaylists::add_tag(const std::string& uri, const std::string& tag) {
    if (uri.empty() || tag.empty()) return;
    auto& tags = tags_[uri];
    if (std::find(tags.begin(), tags.end(), tag) == tags.end())
        tags.push_back(tag);
    save_tags();
}

void LocalPlaylists::remove_tag(const std::string& uri, const std::string& tag) {
    auto it = tags_.find(uri);
    if (it == tags_.end()) return;
    it->second.erase(std::remove(it->second.begin(), it->second.end(), tag),
                     it->second.end());
    if (it->second.empty()) tags_.erase(it);
    save_tags();
}

std::vector<std::string> LocalPlaylists::get_tags(const std::string& uri) const {
    auto it = tags_.find(uri);
    if (it != tags_.end()) return it->second;
    return {};
}

std::vector<Track> LocalPlaylists::get_tagged(const std::string& tag) const {
    std::vector<Track> result;
    for (const auto& [pl_name, tracks] : playlists_) {
        for (const auto& t : tracks) {
            auto it = tags_.find(t.uri);
            if (it != tags_.end()) {
                if (std::find(it->second.begin(), it->second.end(), tag)
                    != it->second.end()) {
                    result.push_back(t);
                }
            }
        }
    }
    return result;
}

std::vector<std::string> LocalPlaylists::all_tags() const {
    std::vector<std::string> result;
    for (const auto& [uri, tags] : tags_)
        for (const auto& t : tags)
            if (std::find(result.begin(), result.end(), t) == result.end())
                result.push_back(t);
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace txs

#include "history.hpp"
#include "config.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>

using json = nlohmann::json;

namespace txs {

History::History() {
    file_path_ = Config::config_dir() + "/history.json";
    load();
}

void History::load() {
    std::ifstream f(file_path_);
    if (!f.is_open()) return;
    try {
        json j;
        f >> j;
        entries_.clear();
        for (auto& e : j) {
            HistoryEntry he;
            he.played_at = e.value("played_at", (std::time_t)0);
            auto& tj = e["track"];
            he.track.title         = tj.value("title", "");
            he.track.artist        = tj.value("artist", "");
            he.track.album         = tj.value("album", "");
            he.track.source        = tj.value("source", "");
            he.track.uri           = tj.value("uri", "");
            he.track.thumbnail_url = tj.value("thumbnail_url", "");
            he.track.duration_ms   = tj.value("duration_ms", 0);
            entries_.push_back(he);
        }
    } catch (...) {}
}

void History::save() const {
    json j = json::array();
    // Save most recent MAX_HISTORY entries
    int start = (int)entries_.size() > MAX_HISTORY
                ? (int)entries_.size() - MAX_HISTORY : 0;
    for (int i = start; i < (int)entries_.size(); ++i) {
        const auto& he = entries_[i];
        j.push_back({
            {"played_at", he.played_at},
            {"track", {
                {"title",         he.track.title},
                {"artist",        he.track.artist},
                {"album",         he.track.album},
                {"source",        he.track.source},
                {"uri",           he.track.uri},
                {"thumbnail_url", he.track.thumbnail_url},
                {"duration_ms",   he.track.duration_ms}
            }}
        });
    }
    std::ofstream f(file_path_);
    if (f.is_open()) f << j.dump(2);
}

void History::record(const Track& track) {
    if (track.uri.empty()) return;
    HistoryEntry he;
    he.track = track;
    he.played_at = std::time(nullptr);
    entries_.push_back(he);
    save();
}

std::vector<HistoryEntry> History::get_recent(int n) const {
    std::vector<HistoryEntry> result;
    int start = std::max(0, (int)entries_.size() - n);
    for (int i = (int)entries_.size() - 1; i >= start; --i)
        result.push_back(entries_[i]);
    return result;
}

std::vector<Track> History::get_recent_tracks(int n) const {
    auto entries = get_recent(n);
    std::vector<Track> tracks;
    for (auto& e : entries) tracks.push_back(e.track);
    return tracks;
}

std::unordered_set<std::string> History::get_recent_uris(int n) const {
    std::unordered_set<std::string> uris;
    int start = std::max(0, (int)entries_.size() - n);
    for (int i = start; i < (int)entries_.size(); ++i)
        if (!entries_[i].track.uri.empty())
            uris.insert(entries_[i].track.uri);
    return uris;
}

} // namespace txs

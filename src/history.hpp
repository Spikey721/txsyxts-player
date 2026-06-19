#pragma once
#include "spotify.hpp"
#include <string>
#include <vector>
#include <ctime>
#include <unordered_set>

namespace txs {

struct HistoryEntry {
    Track track;
    std::time_t played_at = 0;
};

class History {
public:
    History();
    void record(const Track& track);
    std::vector<HistoryEntry> get_recent(int n = 100) const;
    std::vector<Track> get_recent_tracks(int n = 50) const;
    // Returns URIs of recently played tracks (for avoiding repeats in autoplay)
    std::unordered_set<std::string> get_recent_uris(int n = 100) const;

private:
    std::string file_path_;
    std::vector<HistoryEntry> entries_;
    static const int MAX_HISTORY = 500;
    void load();
    void save() const;
};

} // namespace txs

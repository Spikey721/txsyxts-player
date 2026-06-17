#pragma once

#include "spotify.hpp"
#include <string>
#include <vector>
#include <map>

namespace txs {

class LocalPlaylists {
public:
    LocalPlaylists();

    void load();
    void save() const;

    // Get list of local playlists (name)
    std::vector<std::string> get_names() const;

    // Get tracks for a specific playlist
    std::vector<Track> get_tracks(const std::string& name) const;

    // Create a new playlist
    bool create(const std::string& name);

    // Delete a playlist
    bool remove(const std::string& name);

    // Add track to playlist
    bool add_track(const std::string& name, const Track& track);

    // Remove track from playlist at index
    bool remove_track(const std::string& name, int index);

private:
    std::map<std::string, std::vector<Track>> playlists_;
    std::string file_path_;
};

} // namespace txs

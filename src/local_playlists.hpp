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

    // Playlist management
    std::vector<std::string> get_names() const;
    std::vector<Track> get_tracks(const std::string& name) const;
    bool create(const std::string& name);
    bool remove(const std::string& name);
    bool rename(const std::string& old_name, const std::string& new_name);
    bool add_track(const std::string& name, const Track& track);
    bool remove_track(const std::string& name, int index);

    // Export playlist to m3u8 file at given path
    bool export_m3u(const std::string& playlist_name, const std::string& out_path) const;

    // Track tags — tag a track URI with a label (e.g. "chill", "workout")
    void add_tag(const std::string& uri, const std::string& tag);
    void remove_tag(const std::string& uri, const std::string& tag);
    std::vector<std::string> get_tags(const std::string& uri) const;
    // Get all tracks across all playlists that have a given tag
    std::vector<Track> get_tagged(const std::string& tag) const;
    // List all unique tags in use
    std::vector<std::string> all_tags() const;

private:
    std::map<std::string, std::vector<Track>> playlists_;
    // uri -> list of tags
    std::map<std::string, std::vector<std::string>> tags_;
    std::string file_path_;
    std::string tags_file_path_;
    void load_tags();
    void save_tags() const;
};

} // namespace txs

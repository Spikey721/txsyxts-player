#pragma once

#include "spotify.hpp"
#include <vector>
#include <optional>

namespace txs {

enum class LoopMode { Off, Track, All };

class Queue {
public:
    void load(const std::vector<Track>& tracks);
    void append(const Track& track);
    void clear();

    const std::vector<Track>& tracks() const { return tracks_; }
    int length() const { return (int)tracks_.size(); }
    bool empty() const { return tracks_.empty(); }
    int current_index() const;

    std::optional<Track> current() const;
    std::optional<Track> next();
    std::optional<Track> prev();
    std::optional<Track> jump(int index);
    std::optional<Track> play_first();

    bool toggle_shuffle();
    bool shuffle() const { return shuffle_; }

    LoopMode cycle_loop();
    LoopMode loop() const { return loop_; }
    std::string loop_str() const;

private:
    std::vector<Track> tracks_;
    std::vector<int> order_;
    int pos_ = -1;
    std::vector<int> history_;
    bool shuffle_ = false;
    LoopMode loop_ = LoopMode::Off;

    void rebuild_order();
};

} // namespace txs

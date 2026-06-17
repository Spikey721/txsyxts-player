#include "queue.hpp"
#include <algorithm>
#include <random>

namespace txs {

void Queue::load(const std::vector<Track>& tracks) {
    tracks_ = tracks;
    pos_ = -1;
    history_.clear();
    rebuild_order();
}

void Queue::append(const Track& track) {
    int idx = (int)tracks_.size();
    tracks_.push_back(track);
    order_.push_back(idx);
}

void Queue::clear() {
    tracks_.clear();
    order_.clear();
    pos_ = -1;
    history_.clear();
}

int Queue::current_index() const {
    if (pos_ >= 0 && pos_ < (int)order_.size())
        return order_[pos_];
    return -1;
}

std::optional<Track> Queue::current() const {
    int idx = current_index();
    if (idx >= 0 && idx < (int)tracks_.size())
        return tracks_[idx];
    return std::nullopt;
}

std::optional<Track> Queue::next() {
    if (order_.empty()) return std::nullopt;

    if (loop_ == LoopMode::Track) return current();

    if (pos_ >= 0) history_.push_back(pos_);
    pos_++;

    if (pos_ >= (int)order_.size()) {
        if (loop_ == LoopMode::All) {
            pos_ = 0;
            if (shuffle_) rebuild_order();
        } else {
            pos_ = (int)order_.size();
            return std::nullopt;
        }
    }
    return current();
}

std::optional<Track> Queue::prev() {
    if (!history_.empty()) {
        pos_ = history_.back();
        history_.pop_back();
        return current();
    }
    if (pos_ > 0) {
        pos_--;
        return current();
    }
    return current();
}

std::optional<Track> Queue::jump(int index) {
    if (index < 0 || index >= (int)tracks_.size()) return std::nullopt;

    // find in order
    for (int i = 0; i < (int)order_.size(); i++) {
        if (order_[i] == index) {
            pos_ = i;
            break;
        }
    }
    history_.clear();
    return current();
}

std::optional<Track> Queue::play_first() {
    if (order_.empty()) return std::nullopt;
    pos_ = 0;
    history_.clear();
    return current();
}

bool Queue::toggle_shuffle() {
    shuffle_ = !shuffle_;
    int cur = current_index();
    rebuild_order();
    if (cur >= 0) {
        for (int i = 0; i < (int)order_.size(); i++) {
            if (order_[i] == cur) { pos_ = i; break; }
        }
    }
    return shuffle_;
}

LoopMode Queue::cycle_loop() {
    if (loop_ == LoopMode::Off) loop_ = LoopMode::Track;
    else if (loop_ == LoopMode::Track) loop_ = LoopMode::All;
    else loop_ = LoopMode::Off;
    return loop_;
}

std::string Queue::loop_str() const {
    switch (loop_) {
        case LoopMode::Track: return "track";
        case LoopMode::All: return "all";
        default: return "off";
    }
}

void Queue::rebuild_order() {
    order_.resize(tracks_.size());
    for (int i = 0; i < (int)tracks_.size(); i++) order_[i] = i;
    if (shuffle_) {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(order_.begin(), order_.end(), g);
    }
}

} // namespace txs

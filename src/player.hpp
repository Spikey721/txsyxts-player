#pragma once

#include <string>
#include <functional>

namespace txs {

class Player {
public:
    Player();
    ~Player();

    // controls
    void play(const std::string& url, const std::string& title = "",
              const std::string& artist = "");
    void pause();
    void stop();
    void seek(double seconds);
    void seek_absolute(double pos_sec);

    // volume
    int volume() const;
    void set_volume(int vol);

    void set_loudnorm(bool enable);

    // state
    bool is_playing() const;
    bool is_paused() const;
    double position() const;
    double duration() const;
    const std::string& current_title() const;
    const std::string& current_artist() const;

    // process events (call from main loop)
    void poll_events();

    // callbacks
    std::function<void()> on_track_end;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace txs

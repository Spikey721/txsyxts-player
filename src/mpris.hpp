#pragma once

#include <string>
#include <functional>

namespace txs {

// Minimal MPRIS MediaPlayer2 + MediaPlayer2.Player D-Bus service.
// Call start() once after the player is created, update() on every tick,
// and stop() before the process exits.
class MprisServer {
public:
    MprisServer();
    ~MprisServer();

    // Launch the D-Bus service (non-blocking — uses a background thread).
    void start();

    // Push current player state.  Call from any thread.
    void update(const std::string& title,
                const std::string& artist,
                const std::string& art_url,
                double position_sec,
                double duration_sec,
                bool playing,
                bool paused);

    // Stop the D-Bus service.
    void stop();

    // Callbacks invoked from the D-Bus thread when a controller sends a command.
    std::function<void()> on_play_pause;
    std::function<void()> on_next;
    std::function<void()> on_prev;
    std::function<void()> on_stop;
    std::function<void(double)> on_seek; // absolute position in seconds

private:
    void* impl_;
};

} // namespace txs

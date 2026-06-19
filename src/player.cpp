#include "player.hpp"
#include <mpv/client.h>
#include <cstring>

namespace txs {

struct Player::Impl {
    mpv_handle* mpv = nullptr;
    std::string title;
    std::string artist;
    double pos = 0.0;
    double dur = 0.0;
    bool paused = false;
    bool playing = false;
    bool idle = true;
};

Player::Player() : impl_(new Impl) {
    impl_->mpv = mpv_create();
    if (!impl_->mpv) return;

    mpv_set_option_string(impl_->mpv, "vo", "null");
    mpv_set_option_string(impl_->mpv, "video", "no");
    mpv_set_option_string(impl_->mpv, "audio-display", "no");
    mpv_set_option_string(impl_->mpv, "terminal", "no");
    mpv_set_option_string(impl_->mpv, "input-default-bindings", "no");
    mpv_set_option_string(impl_->mpv, "ytdl", "yes");
    mpv_set_option_string(impl_->mpv, "config", "no");

    // Memory optimizations: Increased to 50MB forward / 10MB backward to prevent audio stuttering
    // while still maintaining a much smaller footprint than MPV's default 150MB+ cache.
    mpv_set_option_string(impl_->mpv, "cache", "yes");
    mpv_set_option_string(impl_->mpv, "demuxer-max-bytes", "50000000"); // 50 MB
    mpv_set_option_string(impl_->mpv, "demuxer-max-back-bytes", "10000000"); // 10 MB

    mpv_initialize(impl_->mpv);

    mpv_observe_property(impl_->mpv, 0, "playback-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(impl_->mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(impl_->mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(impl_->mpv, 0, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(impl_->mpv, 0, "media-title", MPV_FORMAT_STRING);
}

Player::~Player() {
    if (impl_->mpv) {
        mpv_terminate_destroy(impl_->mpv);
    }
    delete impl_;
}

void Player::play(const std::string& url, const std::string& title,
                  const std::string& artist) {
    if (!impl_->mpv) return;

    impl_->title = title;
    impl_->artist = artist;
    impl_->pos = 0.0;
    impl_->dur = 0.0;
    impl_->playing = true;
    impl_->idle = false;

    const char* cmd[] = {"loadfile", url.c_str(), nullptr};
    mpv_command(impl_->mpv, cmd);
}

void Player::pause() {
    if (!impl_->mpv) return;
    int flag = impl_->paused ? 0 : 1;
    mpv_set_property(impl_->mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void Player::stop() {
    if (!impl_->mpv) return;
    const char* cmd[] = {"stop", nullptr};
    mpv_command(impl_->mpv, cmd);
    impl_->title.clear();
    impl_->artist.clear();
    impl_->pos = 0.0;
    impl_->dur = 0.0;
    impl_->playing = false;
}

void Player::seek(double seconds) {
    if (!impl_->mpv) return;
    std::string secs = std::to_string(seconds);
    const char* cmd[] = {"seek", secs.c_str(), "relative", nullptr};
    mpv_command(impl_->mpv, cmd);
}

void Player::seek_absolute(double pos_sec) {
    if (!impl_->mpv) return;
    std::string s = std::to_string(pos_sec);
    const char* args[] = {"seek", s.c_str(), "absolute", nullptr};
    mpv_command(impl_->mpv, args);
}

int Player::volume() const {
    if (!impl_->mpv) return 70;
    double vol = 70;
    mpv_get_property(impl_->mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
    return static_cast<int>(vol);
}

void Player::set_volume(int vol) {
    if (!impl_->mpv) return;
    double v = static_cast<double>(std::max(0, std::min(150, vol)));
    mpv_set_property(impl_->mpv, "volume", MPV_FORMAT_DOUBLE, &v);
}

bool Player::is_playing() const { return impl_->playing && !impl_->paused; }
bool Player::is_paused() const { return impl_->paused; }
double Player::position() const { return impl_->pos; }
double Player::duration() const { return impl_->dur; }
const std::string& Player::current_title() const { return impl_->title; }
const std::string& Player::current_artist() const { return impl_->artist; }

void Player::set_loudnorm(bool enable) {
    if (!impl_->mpv) return;
    if (enable) {
        mpv_set_property_string(impl_->mpv, "af", "loudnorm=I=-16:TP=-1.5:LRA=11");
    } else {
        mpv_set_property_string(impl_->mpv, "af", "");
    }
}

void Player::poll_events() {
    if (!impl_->mpv) return;

    while (true) {
        mpv_event* ev = mpv_wait_event(impl_->mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;

        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto* prop = static_cast<mpv_event_property*>(ev->data);

            if (strcmp(prop->name, "playback-time") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                impl_->pos = *static_cast<double*>(prop->data);
            } else if (strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                impl_->dur = *static_cast<double*>(prop->data);
            } else if (strcmp(prop->name, "pause") == 0 && prop->format == MPV_FORMAT_FLAG) {
                impl_->paused = *static_cast<int*>(prop->data) != 0;
            } else if (strcmp(prop->name, "idle-active") == 0 && prop->format == MPV_FORMAT_FLAG) {
                bool idle = *static_cast<int*>(prop->data) != 0;
                if (idle && impl_->playing) {
                    impl_->playing = false;
                    impl_->title.clear();
                    impl_->artist.clear();
                    impl_->pos = 0.0;
                    impl_->dur = 0.0;
                    if (on_track_end) on_track_end();
                }
                impl_->idle = idle;
            } else if (strcmp(prop->name, "media-title") == 0 && prop->format == MPV_FORMAT_STRING) {
                if (prop->data) {
                    char* title_str = *static_cast<char**>(prop->data);
                    if (title_str) {
                        // Radio stream metadata updates via media-title
                        std::string mt = title_str;
                        if (mt != impl_->title && !mt.empty()) {
                            impl_->title = mt;
                        }
                    }
                }
            }
        }
    }
}

} // namespace txs

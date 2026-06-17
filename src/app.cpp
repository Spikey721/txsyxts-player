#include "app.hpp"
#include "config.hpp"
#include "spotify.hpp"
#include "youtube.hpp"
#include "player.hpp"
#include "queue.hpp"
#include "commands.hpp"
#include "local_playlists.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/canvas.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/event.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "ascii_art.hpp"

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <future>
#include <sstream>
#include <csignal>
#include <unistd.h>
#include <cstdlib>

using namespace ftxui;

namespace txs {

// ── format time ────────────────────────────────────────────────

static std::string fmt_time(double secs) {
    if (secs <= 0) return "0:00";
    int total = (int)secs;
    int m = total / 60;
    int s = total % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

// ── view modes ─────────────────────────────────────────────────

enum class ViewMode { Home, Playlists, Tracks, Info };

// ── app state ──────────────────────────────────────────────────

struct AppState {
    Config config;
    Spotify spotify;
    Player player;
    Queue queue;
    LocalPlaylists local_playlists;

    // UI state
    ViewMode view = ViewMode::Playlists;
    std::string current_local_playlist;
    std::string cmd_input;
    std::string log_msg = "type :help  ·  :play <query> to search";
    bool log_error = false;
    std::string info_text;
    int selected = 0;
    int ytfzf_pid = 0;
    bool is_search_queue = false;

    // playlist browser
    std::vector<std::pair<std::string,std::string>> playlists; // name, id
    int pl_selected = 0;

    // autoplay & controls
    bool autoplay = true;
    std::string last_artist;
    std::string last_title;
    std::string loop_btn_text = " 🔁 Loop: Off ";
    std::string autoplay_btn_text = " ∞ Autoplay: On ";

    // UI elements
    std::vector<std::string> pl_names;
    std::vector<std::string> track_names;

    std::mutex mtx;

    // ASCII Art state
    std::string current_art_url;
    ftxui::Element current_art = ftxui::text("");
    bool is_fetching_art = false;

    // Modal state
    bool show_modal = false;
    std::vector<std::string> modal_options;
    int modal_selected = 0;
    std::string modal_title;
    std::string modal_input;
    bool modal_is_input = false;
    std::function<void()> on_modal_submit;

    void set_log(const std::string& msg, bool err = false) {
        log_msg = msg;
        log_error = err;
    }
};

// ── command handlers ───────────────────────────────────────────

static void play_track(AppState& st, const Track& track, ftxui::ScreenInteractive* screen = nullptr);
static void update_ascii_art(AppState& st, const Track& track, ftxui::ScreenInteractive& screen);

static void handle_command(AppState& st, const std::string& raw,
                           ScreenInteractive& screen) {
    auto cmd = parse_command(raw);
    if (!cmd) {
        st.set_log("unknown: " + raw, true);
        return;
    }

    const auto& name = cmd->name;
    const auto& args = cmd->args;

    if (name == "help") {
        st.info_text = HELP_TEXT;
        st.view = ViewMode::Info;

    } else if (name == "quit") {
        screen.Exit();

    } else if (name == "config") {
        if (args.empty()) {
            std::string sp = st.config.sp_dc;
            std::string sp_d = sp.size() > 12 ? sp.substr(0,12)+"..." : (sp.empty() ? "(not set)" : sp);
            st.info_text = "config:\n  sp_dc: " + sp_d
                + "\n  volume: " + std::to_string(st.config.volume)
                + "\n  quality: " + st.config.audio_quality
                + "\n\n  set: :config <key> <value>";
            st.view = ViewMode::Info;
        } else {
            auto sp = args.find(' ');
            if (sp == std::string::npos) {
                st.set_log("usage: :config <key> <value>", true);
                return;
            }
            std::string key = args.substr(0, sp);
            std::string val = args.substr(sp + 1);
            if (key == "sp_dc") {
                st.config.sp_dc = val;
                st.spotify.set_sp_dc(val);
            } else if (key == "volume") {
                st.config.volume = std::stoi(val);
                st.player.set_volume(st.config.volume);
            } else if (key == "quality") {
                st.config.audio_quality = val;
            } else {
                st.set_log("unknown key: " + key, true);
                return;
            }
            st.config.save();
            st.set_log("saved: " + key);
        }

    } else if (name == "login") {
        st.set_log("spotify functionality coming soon!", true);

    } else if (name == "liked") {
        st.set_log("spotify functionality coming soon!", true);

    } else if (name == "playlists") {
        st.set_log("fetching playlists...");
        std::thread([&st, &screen]() {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.playlists.clear();
            st.pl_names.clear();

            // Add local playlists first
            for (const auto& local_name : st.local_playlists.get_names()) {
                st.playlists.push_back({local_name, "local:" + local_name});
                st.pl_names.push_back("[Local] " + local_name);
            }

            st.pl_selected = 0;
            st.view = ViewMode::Playlists;
            st.set_log("found " + std::to_string(st.playlists.size()) + " playlists  [o]pen [j/k]nav");
            screen.Post(Event::Custom);
        }).detach();

    } else if (name == "playlist") {
        st.set_log("spotify functionality coming soon!", true);


    } else if (name == "search") {
        if (args.empty()) { st.set_log("usage: :search <query>", true); return; }
        st.set_log("searching: " + args + "...");
        std::string q = args;
        std::thread([&st, &screen, q]() {
            auto tracks = yt_search(q, 10);
            std::lock_guard<std::mutex> lk(st.mtx);
            if (tracks.empty()) {
                st.set_log("no results", true);
            } else {
                st.queue.load(tracks);
                st.is_search_queue = true;
                st.track_names.clear();
                for (auto& t : tracks) st.track_names.push_back(t.title);
                st.view = ViewMode::Tracks;
                st.info_text.clear();
                st.selected = 0;
                st.set_log("found " + std::to_string(tracks.size()) + " results");
            }
            screen.Post(Event::Custom);
        }).detach();

    } else if (name == "radio") {
        if (args.empty()) { st.set_log("usage: :radio <genre>", true); return; }
        st.set_log("starting radio: " + args + "...");
        std::string q = args + " mix playlist";
        std::thread([&st, &screen, q]() {
            auto tracks = yt_search(q, 50); // Get a lot of tracks
            std::lock_guard<std::mutex> lk(st.mtx);
            if (tracks.empty()) {
                st.set_log("no results for radio", true);
            } else {
                st.queue.load(tracks);
                st.queue.toggle_shuffle(); // Shuffle for randomness
                st.track_names.clear();
                for (auto& t : st.queue.tracks()) st.track_names.push_back(t.title);
                st.view = ViewMode::Tracks;
                st.info_text.clear();
                st.selected = 0;
                st.set_log("radio started: " + std::to_string(tracks.size()) + " tracks");
                
                // Play first automatically
                auto t = st.queue.play_first();
                if (t) play_track(st, *t, &screen);
            }
            screen.Post(Event::Custom);
        }).detach();

    } else if (name == "play") {
        if (args.empty()) {
            if (!st.player.current_title().empty()) {
                st.player.pause();
            } else {
                auto t = st.queue.play_first();
                if (t) play_track(st, *t);
                else st.set_log("nothing to play", true);
            }
        } else {
            // try as number
            bool is_num = true;
            for (char c : args) { if (!isdigit(c)) { is_num = false; break; } }

            if (is_num) {
                int idx = std::stoi(args) - 1;
                auto t = st.queue.jump(idx);
                if (t) play_track(st, *t);
                else st.set_log("invalid track: " + args, true);
            } else {
                // search and play
                st.set_log("searching: " + args + "...");
                std::string q = args;
                std::thread([&st, &screen, q]() {
                    if (q.find("http") == 0 && (q.find("youtube.com") != std::string::npos || q.find("youtu.be") != std::string::npos)) {
                        st.set_log("loading youtube playlist/video...");
                        screen.Post(Event::Custom);
                        auto tracks = yt_fetch_playlist(q, 100);
                        if (!tracks.empty()) {
                            std::lock_guard<std::mutex> lk(st.mtx);
                            st.queue.load(tracks);
                            st.track_names.clear();
                            for (auto& t : tracks) st.track_names.push_back(t.title);
                            st.view = ViewMode::Tracks;
                            st.current_local_playlist.clear();
                            st.selected = 0;
                            st.set_log("loaded " + std::to_string(tracks.size()) + " tracks from yt");
                            auto first = st.queue.play_first();
                            if (first) {
                                play_track(st, *first, &screen);
                                update_ascii_art(st, *first, screen);
                            }
                        } else {
                            std::lock_guard<std::mutex> lk(st.mtx);
                            st.set_log("failed to load yt link", true);
                        }
                    } else {
                        auto url = yt_stream_url("ytsearch:" + q);
                        std::lock_guard<std::mutex> lk(st.mtx);
                        if (!url.empty()) {
                            Track t; t.title = q; t.source = "youtube"; t.uri = url;
                            st.player.play(url, q, "");
                            st.set_log("▶ " + q);
                        } else {
                            st.set_log("not found: " + q, true);
                        }
                    }
                    screen.Post(Event::Custom);
                }).detach();
            }
        }

/*
    } else if (name == "ytfzf") {
        if (args.empty()) { st.set_log("usage: :ytfzf <query>", true); return; }
        st.player.stop();
        if (st.ytfzf_pid > 0) kill(st.ytfzf_pid, SIGTERM);
        st.ytfzf_pid = ytfzf_play(args);
        if (st.ytfzf_pid) st.set_log("ytfzf: " + args);
        else st.set_log("ytfzf not available", true);
*/

    } else if (name == "local") {
        if (args.empty()) { st.set_log("usage: :local <dir>", true); return; }
        auto tracks = scan_local(args);
        if (tracks.empty()) st.set_log("no audio files in " + args, true);
        else {
            st.queue.load(tracks);
            st.is_search_queue = false;
            st.track_names.clear();
            for (auto& t : tracks) st.track_names.push_back(t.title);
            st.info_text.clear();
            st.selected = 0;
            st.set_log("loaded " + std::to_string(tracks.size()) + " files");
        }

    } else if (name == "pause") {
        st.player.pause();

    } else if (name == "next") {
        auto t = st.queue.next();
        if (t) play_track(st, *t);
        else { st.player.stop(); st.set_log("end of queue"); }

    } else if (name == "prev") {
        auto t = st.queue.prev();
        if (t) play_track(st, *t);

    } else if (name == "stop") {
        st.player.stop();
        if (st.ytfzf_pid > 0) { kill(st.ytfzf_pid, SIGTERM); st.ytfzf_pid = 0; }
        st.set_log("stopped");

    } else if (name == "vol") {
        if (args.empty()) {
            st.set_log("volume: " + std::to_string(st.player.volume()));
        } else {
            int v = std::stoi(args);
            st.player.set_volume(v);
            st.config.volume = v;
            st.config.save();
            st.set_log("volume: " + std::to_string(v));
        }

    } else if (name == "seek") {
        if (args.empty()) { st.set_log("usage: :seek <±sec>", true); return; }
        st.player.seek(std::stod(args));

    } else if (name == "shuffle") {
        bool on = st.queue.toggle_shuffle();
        st.set_log(std::string("shuffle: ") + (on ? "on" : "off"));

    } else if (name == "loop") {
        st.queue.cycle_loop();
        st.set_log("loop: " + st.queue.loop_str());

    } else if (name == "queue") {
        st.info_text.clear();

    } else if (name == "pl-create") {
        if (args.empty()) { st.set_log("usage: :pl-create <name>", true); return; }
        if (st.local_playlists.create(args)) st.set_log("created local playlist: " + args);
        else st.set_log("failed to create or already exists", true);

    } else if (name == "pl-add") {
        if (args.empty()) { st.set_log("usage: :pl-add <name>", true); return; }
        if (st.view == ViewMode::Tracks && !st.queue.empty()) {
            auto& t = st.queue.tracks()[st.selected];
            if (st.local_playlists.add_track(args, t)) st.set_log("added to " + args);
            else st.set_log("failed to add track", true);
        } else {
            st.set_log("select a track first", true);
        }

    } else if (name == "pl-remove") {
        if (args.empty()) { st.set_log("usage: :pl-remove <name>", true); return; }
        if (st.view == ViewMode::Tracks && !st.queue.empty()) {
            if (st.local_playlists.remove_track(args, st.selected)) {
                st.set_log("removed from " + args);
                // Refresh view if we're currently looking at it
                st.queue.load(st.local_playlists.get_tracks(args));
                st.track_names.clear();
                for (auto& t : st.queue.tracks()) st.track_names.push_back(t.title);
                if (st.selected >= st.queue.length()) st.selected = std::max(0, st.queue.length() - 1);
            } else {
                st.set_log("failed to remove track", true);
            }
        } else {
            st.set_log("select a track first", true);
        }

    } else if (name == "pl-delete") {
        if (args.empty()) { st.set_log("usage: :pl-delete <name>", true); return; }
        if (st.local_playlists.remove(args)) {
            st.set_log("deleted local playlist: " + args);
            if (st.view == ViewMode::Playlists) {
                // Refresh playlist view
                handle_command(st, "playlists", screen);
            }
        } else {
            st.set_log("failed to delete playlist", true);
        }

    } else if (name == "clear") {
        st.queue.clear();
        st.track_names.clear();
        st.info_text.clear();
        st.set_log("cleared");
    }
}

// ── play a track (resolve spotify→youtube if needed) ──────────

static void play_track(AppState& st, const Track& track, ftxui::ScreenInteractive* screen) {
    if (track.source == "local") {
        st.player.play(track.uri, track.title, track.artist);
        st.set_log("▶ " + track.display());
        return;
    }

    if (track.source == "youtube" && !track.uri.empty()) {
        st.set_log("resolving stream: " + track.title + "...");
        if (screen) screen->Post(Event::Custom);
        Track t_copy = track;
        std::thread([&st, screen, t_copy]() {
            std::string stream_url = yt_stream_url(t_copy.uri);
            std::lock_guard<std::mutex> lk(st.mtx);
            if (!stream_url.empty()) {
                st.player.play(stream_url, t_copy.title, t_copy.artist);
                st.set_log("▶ " + t_copy.display());
            } else {
                st.set_log("failed to extract stream: " + t_copy.display(), true);
            }
            if (screen) screen->Post(Event::Custom);
        }).detach();
        return;
    }

    // spotify → search youtube
    st.set_log("finding: " + track.search_query() + "...");
    if (screen) screen->Post(Event::Custom);
    
    std::string q = track.search_query();
    Track t_copy = track; // capture by value for thread
    
    std::thread([&st, screen, q, t_copy]() {
        auto tracks = yt_search(q, 1);
        if (!tracks.empty() && !tracks[0].uri.empty()) {
            std::string stream_url = yt_stream_url(tracks[0].uri);
            std::lock_guard<std::mutex> lk(st.mtx);
            if (!stream_url.empty()) {
                st.player.play(stream_url, t_copy.title, t_copy.artist);
                st.set_log("▶ " + t_copy.display());
            } else {
                st.set_log("failed to extract stream: " + t_copy.display(), true);
            }
        } else {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.set_log("not found on youtube: " + t_copy.display(), true);
        }
        if (screen) screen->Post(Event::Custom);
    }).detach();
}

static void update_ascii_art(AppState& st, const Track& track, ftxui::ScreenInteractive& screen) {
    if (track.thumbnail_url.empty()) {
        st.set_log("ascii_art: no thumbnail url");
        return;
    }
    if (track.thumbnail_url == st.current_art_url) return;

    st.current_art_url = track.thumbnail_url;
    st.is_fetching_art = true;
    st.set_log("ascii_art: downloading...");
    
    std::string url = track.thumbnail_url;
    std::thread([&st, &screen, url]() {
        auto art = fetch_ascii_art(url, 56, 25);
        std::lock_guard<std::mutex> lk(st.mtx);
        st.current_art = art;
        st.is_fetching_art = false;
        st.set_log("ascii_art: loaded!");
        screen.Post(Event::Custom);
    }).detach();
}

// ── main app ───────────────────────────────────────────────────

int run_app() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    AppState st;
    st.config = Config::load();
    st.spotify.set_sp_dc(st.config.sp_dc);
    st.player.set_volume(st.config.volume);

    // Initial population of local playlists
    st.playlists.clear();
    st.pl_names.clear();
    for (const auto& local_name : st.local_playlists.get_names()) {
        st.playlists.push_back({local_name, "local:" + local_name});
        st.pl_names.push_back("[Local] " + local_name);
    }

    auto screen = ScreenInteractive::Fullscreen();

    // track end callback — autoplay recommendations when queue ends
    st.player.on_track_end = [&st, &screen]() {
        // save what was playing for recommendations
        st.last_artist = st.player.current_artist();
        st.last_title = st.player.current_title();
        
        std::string last_uri;
        if (!st.queue.empty()) {
            last_uri = st.queue.tracks()[st.queue.current_index()].uri;
        }

        auto t = st.queue.next();
        if (t) {
            play_track(st, *t, &screen);
            update_ascii_art(st, *t, screen);
        } else if (st.autoplay && !last_uri.empty()) {
            // queue ended — get recommendations from YouTube Mix
            st.set_log("⟳ finding similar songs...");
            screen.Post(Event::Custom);

            std::thread([&st, &screen, last_uri]() {
                auto recs = yt_get_similar(last_uri, 5);
                if (!recs.empty()) {
                    std::lock_guard<std::mutex> lk(st.mtx);
                    
                    // Filter out the exact song that just played (often index 0 in the mix)
                    std::vector<Track> filtered;
                    for (auto& r : recs) {
                        if (r.uri != last_uri) {
                            filtered.push_back(r);
                        }
                    }
                    
                    if (!filtered.empty()) {
                        st.queue.load(filtered);
                        st.track_names.clear();
                        for (auto& t : filtered) st.track_names.push_back(t.title);
                        st.view = ViewMode::Tracks;
                        auto first = st.queue.play_first();
                        if (first) {
                            play_track(st, *first, &screen);
                            update_ascii_art(st, *first, screen);
                            st.set_log("⟳ autoplay: " + first->display());
                        }
                    } else {
                        st.set_log("autoplay: no new similar songs found", true);
                    }
                } else {
                    std::lock_guard<std::mutex> lk(st.mtx);
                    st.set_log("autoplay: no mix available", true);
                }
                screen.Post(Event::Custom);
            }).detach();
        } else {
            st.set_log("queue finished");
            screen.Post(Event::Custom);
        }
    };

    // command input
    std::string input_str;
    auto input = Input(&input_str, ":");

    auto btn_prev = Button(" ⏮  ", [&]{ auto t = st.queue.prev(); if(t){ play_track(st, *t, &screen); update_ascii_art(st, *t, screen); } }, ButtonOption::Animated(Color::GrayDark, Color::GrayLight, Color::White, Color::White));
    auto btn_play = Button(" ⏯  ", [&]{ st.player.pause(); }, ButtonOption::Animated(Color::RGB(152, 195, 121), Color::RGB(152, 195, 121), Color::White, Color::White));
    auto btn_next = Button(" ⏭  ", [&]{ auto t = st.queue.next(); if(t){ play_track(st, *t, &screen); update_ascii_art(st, *t, screen); } }, ButtonOption::Animated(Color::GrayDark, Color::GrayLight, Color::White, Color::White));
    
    auto btn_loop = Button(&st.loop_btn_text, [&]{ 
        st.queue.cycle_loop(); 
        st.set_log("loop: " + st.queue.loop_str()); 
        st.loop_btn_text = " 🔁 Loop: " + st.queue.loop_str() + " ";
    }, ButtonOption::Animated(Color::GrayDark, Color::GrayLight, Color::White, Color::White));
    
    auto btn_autoplay = Button(&st.autoplay_btn_text, [&]{ 
        st.autoplay = !st.autoplay; 
        st.set_log(std::string("autoplay: ") + (st.autoplay ? "on" : "off")); 
        st.autoplay_btn_text = " ∞ Autoplay: " + std::string(st.autoplay ? "On " : "Off ");
    }, ButtonOption::Animated(Color::GrayDark, Color::GrayLight, Color::White, Color::White));

    auto controls = Container::Horizontal({ btn_prev, btn_play, btn_next, btn_loop, btn_autoplay });

    MenuOption track_opt;
    track_opt.on_enter = [&] {
        if (st.view == ViewMode::Tracks && !st.queue.empty()) {
            auto t = st.queue.jump(st.selected);
            if (t) { play_track(st, *t, &screen); update_ascii_art(st, *t, screen); }
        } else if (st.view == ViewMode::Playlists && !st.playlists.empty()) {
            auto pl = st.playlists[st.pl_selected];
            st.set_log("loading playlist: " + pl.first + "...");
            screen.Post(Event::Custom);
            std::thread([&st, &screen, pl]() {
                std::vector<Track> tracks;
                if (pl.second.find("local:") == 0) {
                    std::lock_guard<std::mutex> lk(st.mtx);
                    tracks = st.local_playlists.get_tracks(pl.first);
                } else if (pl.second.find("spotify:") == 0) {
                    tracks = st.spotify.get_playlist_tracks(pl.second.substr(8));
                } else {
                    tracks = st.spotify.get_playlist_tracks(pl.second); // fallback
                }

                std::lock_guard<std::mutex> lk(st.mtx);
                st.queue.load(tracks);
                st.track_names.clear();
                for (auto& t : tracks) st.track_names.push_back(t.title);
                st.view = ViewMode::Tracks;
                st.selected = 0;
                st.set_log("loaded " + std::to_string(tracks.size()) + " tracks");
                screen.Post(Event::Custom);
            }).detach();
        }
    };
    track_opt.entries_option.transform = [&](const EntryState& state) {
        bool focused = state.focused;
        if (st.view == ViewMode::Playlists) {
            auto& pl = st.playlists[state.index];
            bool active = (state.index == st.pl_selected);
            char idx[16]; snprintf(idx, sizeof(idx), "%3d", state.index + 1);
            std::string prefix = active ? "▶" : " ";
            auto row = hbox({
                text(prefix + " " + std::string(idx) + "  ") | color(Color::GrayDark),
                text(pl.first) | (active ? bold : nothing) | color(active ? Color::RGB(152, 195, 121) : Color::GrayLight)
            });
            if (focused) row = row | inverted;
            else if (active) row = row | bgcolor(Color::RGB(26, 26, 26));
            return row;
        } else {
            auto& t = st.queue.tracks()[state.index];
            bool playing = (state.index == st.queue.current_index());
            bool active = (state.index == st.selected);
            std::string marker = playing ? "▶" : " ";
            char idx[16]; snprintf(idx, sizeof(idx), "%3d", state.index + 1);
            std::string dur = t.duration_str();
            std::string artist_s = t.artist.empty() ? "" : "  " + t.artist;
            auto row = hbox({
                text(marker + " " + std::string(idx) + "  ") | color(Color::GrayDark),
                text(t.title) | (playing ? bold : nothing) | color(playing ? Color::RGB(152, 195, 121) : Color::GrayLight),
                text(artist_s) | color(Color::GrayDark),
                filler(),
                text(dur + " ") | color(Color::GrayDark),
                text(" [+] Add ") | (focused ? color(Color::Black) : color(Color::GrayDark))
            });
            if (focused) row = row | inverted;
            else if (active) row = row | bgcolor(Color::RGB(26, 26, 26));
            return row;
        }
    };
    auto menu_pl = Menu(&st.pl_names, &st.pl_selected, track_opt);
    auto menu_tr = Menu(&st.track_names, &st.selected, track_opt);

    auto main_container = Container::Vertical({
        controls,
        menu_pl,
        menu_tr,
        input
    });

    // ── render function ────────────────────────────────────────

    auto renderer = Renderer(main_container, [&]() {
        // poll mpv events
        st.player.poll_events();

        // ── header ─────────────────────────────────────────────
        std::string status;
        if (st.spotify.is_logged_in()) status += "spotify ✓  ";
        // if (ytfzf_available()) status += "ytfzf ✓  ";
        status += "mpv ✓";

        auto header = hbox({
            text("txsyxts") | bold | color(Color::RGB(152, 195, 121)),
            filler(),
            text(status) | color(Color::GrayDark),
        });

        // ── track list or info ─────────────────────────────────
        Element body;
        if (st.view == ViewMode::Info && !st.info_text.empty()) {
            body = paragraph(st.info_text) | color(Color::GrayLight);
        } else if (st.view == ViewMode::Playlists && !st.playlists.empty()) {
            body = menu_pl->Render() | vscroll_indicator | yframe;
        } else if (st.view == ViewMode::Home || st.queue.empty()) {
            body = vbox({
                text("") | size(HEIGHT, EQUAL, 2),
                text("  :help for commands  ·  :login to connect spotify  ·  :search <query>")
                    | center | color(Color::GrayDark),
            });
        } else {
            body = menu_tr->Render() | vscroll_indicator | yframe;
        }

        // ── now playing bar ────────────────────────────────────
        std::string np_title = st.player.current_title();
        std::string np_artist = st.player.current_artist();
        double pos = st.player.position();
        double dur = st.player.duration();
        int progress = dur > 0 ? (int)(pos / dur * 40) : 0;

        std::string bar_filled;
        for (int i = 0; i < progress; i++) bar_filled += "█";
        std::string bar_empty;
        for (int i = 0; i < 40 - progress; i++) bar_empty += "░";
        std::string state_icon = st.player.is_paused() ? "⏸" :
                                 (!np_title.empty() ? "▶" : " ");

        std::string sh_str = st.queue.shuffle() ? "⤮ on" : "";
        std::string lo_str = st.queue.loop() != LoopMode::Off ?
                             ("↻ " + st.queue.loop_str()) : "";
        std::string vol_str = "vol:" + std::to_string(st.player.volume());

        auto now_playing = vbox({
            hbox({
                controls->Render(),
                text("  "),
                text((np_artist + " - " + np_title).length() > 40 ? 
                     (np_artist + " - " + np_title).substr(0, 37) + "..." : 
                     np_artist + " - " + np_title) | bold | color(Color::White),
                filler(),
            }),
            hbox({
                text(fmt_time(pos) + " ") | color(Color::GrayDark),
                text(bar_filled) | color(Color::RGB(152, 195, 121)),
                text(bar_empty) | color(Color::RGB(51, 51, 51)),
                text(" " + fmt_time(dur)) | color(Color::GrayDark),
                filler(),
                text(sh_str + "  " + lo_str + "  " + vol_str) | color(Color::GrayDark),
            }),
        });

        // ── log bar ────────────────────────────────────────────
        auto log = text("  " + st.log_msg)
            | color(st.log_error ? Color::RGB(224, 108, 117) : Color::GrayDark);

        // ── command input ──────────────────────────────────────
        auto cmd_line = hbox({
            text("  > ") | color(Color::RGB(152, 195, 121)),
            input->Render() | flex,
        });

        // ── compose layout ─────────────────────────────────────
        auto right_panel = vbox({
            header,
            separator() | color(Color::RGB(34, 34, 34)),
            body | flex,
        }) | border;

        auto logo = vbox({
            text(""),
            text("   __                       __      ") | color(Color::RGB(152, 195, 121)) | bold,
            text("  / /__  _________  ___  __/ /______") | color(Color::RGB(152, 195, 121)) | bold,
            text(" / __/ |/_/ ___/ / / / |/_/ __/ ___/") | color(Color::RGB(152, 195, 121)) | bold,
            text("/ /__>  <(__  ) /_/ />  </ /_(__  ) ") | color(Color::RGB(152, 195, 121)) | bold,
            text("\\__/_/|_/____/\\__, /_/|_|\\__/____/  ") | color(Color::RGB(152, 195, 121)) | bold,
            text("             /____/                 ") | color(Color::RGB(152, 195, 121)) | bold,
            filler(),
            text("made by ~tanmaypareek  ") | color(Color::RGB(100, 100, 100)) | align_right
        }) | size(WIDTH, EQUAL, 60) | border;

        Element viz;
        if (st.player.is_playing()) {
            double t_anim = st.player.position() * 5.0; // animation speed
            auto c = Canvas(112, 12); // 56 cols * 2 (braille x), 3 rows * 4 (braille y)
            for (int x = 0; x < 111; ++x) {
                // Generate a smooth composite sine wave
                double y1 = 6.0 + 5.0 * (std::sin(x * 0.15 + t_anim) + 0.4 * std::sin(x * 0.3 - t_anim * 1.5));
                double y2 = 6.0 + 5.0 * (std::sin((x + 1) * 0.15 + t_anim) + 0.4 * std::sin((x + 1) * 0.3 - t_anim * 1.5));
                c.DrawPointLine(x, (int)y1, x + 1, (int)y2, Color::RGB(152, 195, 121));
            }
            viz = canvas(std::move(c));
        } else {
            viz = vbox({
                text(std::string(56, ' ')),
                text(std::string(56, ' ')),
                text(std::string(56, ' '))
            });
        }

        auto main_content = hbox({
            (st.current_art_url.empty() ? logo : vbox({
                st.current_art | border | bgcolor(Color::RGB(15,15,15)),
                filler(),
                viz | center
            }) | size(WIDTH, EQUAL, 60)),
            right_panel | flex
        });

        auto term_size = ftxui::Terminal::Size();
        auto full_layout = vbox({
            main_content | flex,
            separator() | color(Color::RGB(34, 34, 34)),
            now_playing,
            separator() | color(Color::RGB(34, 34, 34)),
            log,
            cmd_line,
        }) | bgcolor(Color::RGB(10, 10, 10)) | size(HEIGHT, LESS_THAN, term_size.dimy - 1);

        if (!st.show_modal) {
            return full_layout;
        }

        // Render modal
        Elements modal_lines;
        modal_lines.push_back(text(st.modal_title) | bold | color(Color::RGB(152, 195, 121)) | center);
        modal_lines.push_back(separator());

        if (st.modal_is_input) {
            modal_lines.push_back(text("Enter name: " + st.modal_input + "█") | color(Color::White));
        } else {
            for (size_t i = 0; i < st.modal_options.size(); ++i) {
                bool active = (int)i == st.modal_selected;
                modal_lines.push_back(text((active ? "▶ " : "  ") + st.modal_options[i]) | 
                                      color(active ? Color::Black : Color::White) | 
                                      (active ? bgcolor(Color::RGB(152, 195, 121)) : nothing));
            }
        }

        auto modal_box = vbox(std::move(modal_lines)) | border | bgcolor(Color::RGB(20, 20, 20)) | clear_under;
        return dbox({ full_layout, modal_box | center });
    });

    // ── event handler ──────────────────────────────────────────

    auto component = CatchEvent(renderer, [&](Event event) -> bool {
        if (st.show_modal) {
            if (event == Event::Escape) {
                st.show_modal = false;
                return true;
            }
            if (st.modal_is_input) {
                if (event == Event::Return) {
                    if (st.on_modal_submit) {
                        auto cb = st.on_modal_submit;
                        cb();
                    }
                    return true;
                }
                if (event == Event::Backspace && !st.modal_input.empty()) {
                    st.modal_input.pop_back();
                    return true;
                }
                if (event.is_character()) {
                    st.modal_input += event.character();
                    return true;
                }
            } else {
                if (event == Event::ArrowUp || event == Event::Character('k')) {
                    if (st.modal_selected > 0) st.modal_selected--;
                    return true;
                }
                if (event == Event::ArrowDown || event == Event::Character('j')) {
                    if (st.modal_selected < (int)st.modal_options.size() - 1) st.modal_selected++;
                    return true;
                }
                if (event == Event::Return) {
                    if (st.on_modal_submit) {
                        auto cb = st.on_modal_submit;
                        cb();
                    }
                    return true;
                }
            }
            return true; // Block other events while modal is open
        }

        // Focus input on ':'
        if (event == Event::Character(':') && input_str.empty()) {
            main_container->SetActiveChild(input);
            // We do NOT return true here, we let the event propagate to the input box!
        }

        // Enter — submit command
        if (event == Event::Return) {
            if (!input_str.empty()) {
                std::string cmd = input_str;
                input_str.clear();
                handle_command(st, cmd, screen);
            } else {
                // Play currently selected track on Enter
                if (st.view == ViewMode::Tracks && !st.queue.empty()) {
                    auto t = st.queue.jump(st.selected);
                    if (t) {
                        if (st.autoplay && st.is_search_queue) {
                            Track selected_t = *t;
                            st.queue.clear();
                            st.queue.append(selected_t);
                            st.track_names.clear();
                            st.track_names.push_back(selected_t.title);
                            st.selected = 0;
                            st.is_search_queue = false;
                            t = st.queue.play_first();
                            
                            st.set_log("fetching radio mix...");
                            screen.Post(Event::Custom);
                            std::string uri = selected_t.uri;
                            std::thread([&st, &screen, uri]() {
                                auto recs = yt_get_similar(uri, 50);
                                std::lock_guard<std::mutex> lk(st.mtx);
                                int added = 0;
                                for (auto& r : recs) {
                                    if (r.uri != uri) {
                                        st.queue.append(r);
                                        st.track_names.push_back(r.title);
                                        added++;
                                    }
                                }
                                if (added > 0) st.set_log("radio mix loaded");
                                else st.set_log("no similar songs found", true);
                                screen.Post(Event::Custom);
                            }).detach();
                        }
                        play_track(st, *t, &screen);
                        update_ascii_art(st, *t, screen);
                    }
                } else if (st.view == ViewMode::Playlists && !st.playlists.empty()) {
                    auto pl = st.playlists[st.pl_selected];
                    st.set_log("loading playlist: " + pl.first + "...");
                    screen.Post(Event::Custom);
                    std::thread([&st, &screen, pl]() {
                        std::vector<Track> tracks;
                        if (pl.second.find("local:") == 0) {
                            std::lock_guard<std::mutex> lk(st.mtx);
                            st.current_local_playlist = pl.first;
                            tracks = st.local_playlists.get_tracks(pl.first);
                        } else if (pl.second.find("spotify:") == 0) {
                            st.current_local_playlist.clear();
                            tracks = st.spotify.get_playlist_tracks(pl.second.substr(8));
                        } else {
                            st.current_local_playlist.clear();
                            tracks = st.spotify.get_playlist_tracks(pl.second); // fallback
                        }

                        std::lock_guard<std::mutex> lk(st.mtx);
                        st.queue.load(tracks);
                        st.is_search_queue = false;
                        st.track_names.clear();
                        for (auto& t : tracks) st.track_names.push_back(t.title);
                        st.view = ViewMode::Tracks;
                        st.selected = 0;
                        st.set_log("loaded " + std::to_string(tracks.size()) + " tracks");
                        screen.Post(Event::Custom);
                    }).detach();
                }
            }
            return true;
        }

        // Ctrl+Q — quit
        if (event == Event::Character('\x11')) {
            screen.Exit();
            return true;
        }

        // List navigation (works even when input is focused, if input is empty)
        if ((event == Event::Character('j') || event == Event::ArrowDown) && input_str.empty()) {
            if (st.view == ViewMode::Playlists) {
                if (st.pl_selected < (int)st.playlists.size() - 1) st.pl_selected++;
            } else {
                if (st.selected < st.queue.length() - 1) st.selected++;
            }
            return true;
        }
        if ((event == Event::Character('k') || event == Event::ArrowUp) && input_str.empty()) {
            if (st.view == ViewMode::Playlists) {
                if (st.pl_selected > 0) st.pl_selected--;
            } else {
                if (st.selected > 0) st.selected--;
            }
            return true;
        }

        // Add to local playlist (open modal)
        if ((event == Event::Character('a') || event == Event::Character('A') || event == Event::Character('+')) && input_str.empty()) {
            if (st.view == ViewMode::Tracks && !st.queue.empty()) {
                st.modal_title = " Add to Playlist ";
                st.modal_options = st.local_playlists.get_names();
                st.modal_options.push_back("[ Create New ]");
                st.modal_selected = 0;
                st.modal_is_input = false;
                st.show_modal = true;
                
                // capture copy
                Track t = st.queue.tracks()[st.selected];
                
                st.on_modal_submit = [&st, &screen, t]() {
                    if (st.modal_selected == (int)st.modal_options.size() - 1) {
                        // Create New
                        st.modal_title = " Create New Playlist ";
                        st.modal_input = "";
                        st.modal_is_input = true;
                        
                        st.on_modal_submit = [&st, &screen, t]() {
                            if (!st.modal_input.empty()) {
                                st.local_playlists.create(st.modal_input);
                                st.local_playlists.add_track(st.modal_input, t);
                                st.set_log("created and added to " + st.modal_input);
                            }
                            st.show_modal = false;
                            screen.Post(Event::Custom);
                        };
                    } else {
                        // Add to existing
                        std::string pl_name = st.modal_options[st.modal_selected];
                        st.local_playlists.add_track(pl_name, t);
                        st.set_log("added to " + pl_name);
                        st.show_modal = false;
                    }
                    screen.Post(Event::Custom);
                };
            }
            return true;
        }

        // Remove from local playlist (instant if viewing one)
        if ((event == Event::Character('d') || event == Event::Character('D') || event == Event::Delete) && input_str.empty()) {
            if (st.view == ViewMode::Tracks && !st.queue.empty()) {
                if (!st.current_local_playlist.empty()) {
                    if (st.local_playlists.remove_track(st.current_local_playlist, st.selected)) {
                        st.set_log("removed from " + st.current_local_playlist);
                        st.queue.load(st.local_playlists.get_tracks(st.current_local_playlist));
                        st.track_names.clear();
                        for (auto& tr : st.queue.tracks()) st.track_names.push_back(tr.title);
                        if (st.selected >= st.queue.length()) st.selected = std::max(0, st.queue.length() - 1);
                        screen.Post(Event::Custom);
                    } else {
                        st.set_log("failed to remove track", true);
                    }
                } else {
                    st.set_log("not viewing a local playlist", true);
                }
            }
            return true;
        }

        // Seek forward/backward
        if (event == Event::ArrowRight && input_str.empty()) {
            st.player.seek(5.0);
            return true;
        }
        if (event == Event::ArrowLeft && input_str.empty()) {
            st.player.seek(-5.0);
            return true;
        }

        // Handle other keybindings that Menu doesn't catch
        if (event == Event::Character('b') && input_str.empty()) {
            if (st.view == ViewMode::Tracks && !st.playlists.empty()) {
                st.view = ViewMode::Playlists;
            } else if (st.view == ViewMode::Playlists && !st.queue.empty()) {
                st.view = ViewMode::Tracks;
            }
            return true;
        }

        // Ctrl+N — next
        if (event == Event::Character('\x0e')) {
            auto t = st.queue.next();
            if (t) play_track(st, *t);
            else st.set_log("end of queue");
            return true;
        }

        // Ctrl+P — prev
        if (event == Event::Character('\x10')) {
            auto t = st.queue.prev();
            if (t) play_track(st, *t);
            return true;
        }

        return false;
    });

    // ── periodic refresh (for visualizer animation) ────────────
    std::atomic<bool> running{true};
    std::thread ticker([&]() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            screen.Post(Event::Custom);
        }
    });

    screen.Loop(component);

    running = false;
    ticker.join();

    curl_global_cleanup();
    return 0;
}

} // namespace txs

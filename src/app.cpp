#include "app.hpp"
#include "config.hpp"
#include "spotify.hpp"
#include "youtube.hpp"
#include "player.hpp"
#include "queue.hpp"
#include "commands.hpp"
#include "local_playlists.hpp"
#include "history.hpp"
#include "scrobbler.hpp"
#include "sponsorblock.hpp"

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
#include <unordered_map>
#include <unordered_set>
#include <ctime>

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
    History history;
    Scrobbler scrobbler;

    // UI state
    ViewMode view = ViewMode::Playlists;
    std::string current_local_playlist;
    std::string cmd_input;
    std::string log_msg = ":play <query>  :search <query>  :help";
    bool log_error = false;
    std::string info_text;
    Lyrics current_lyrics;
    int selected = 0;
    std::unordered_set<int> selected_indices;
    int ytfzf_pid = 0;
    bool is_search_queue = false;

    // playlist browser
    std::vector<std::pair<std::string,std::string>> playlists; // name, id
    int pl_selected = 0;

    // autoplay & controls
    bool autoplay = true;
    bool auto_lyrics = false;
    std::string last_artist;
    std::string last_title;
    std::string loop_btn_text = " \U0001f501 Loop: Off ";
    std::string autoplay_btn_text = " \u221e Autoplay: On ";
    std::string autolyrics_btn_text = " ♫ Lyrics: Off ";

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

    // Stream URL cache: video URI -> direct CDN stream URL
    std::unordered_map<std::string, std::string> stream_cache;
    std::string prefetch_uri; // URI currently being pre-fetched

    // Autoplay: set of URIs played this session to avoid repeats
    std::unordered_set<std::string> played_uris;

    // SponsorBlock segments for currently playing video
    std::vector<SponsorSegment> sponsor_segments;
    std::string sponsor_video_id;

    // Last.fm scrobbling
    std::time_t track_started_at = 0;
    bool scrobbled_current = false;

    // yt-dlp version warning (set at startup)
    std::string ytdlp_warn;

    // Download status line
    std::string download_status;

    // Layout box for progress bar clicks
    ftxui::Box progress_box;

    void set_log(const std::string& msg, bool err = false) {
        log_msg = msg;
        log_error = err;
    }
};


// ── command handlers ───────────────────────────────────────────

static void play_track(AppState& st, const Track& track, ftxui::ScreenInteractive* screen = nullptr, int skip_count = 0);
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
        // Alias for :playlists — show local playlists
        handle_command(st, ":playlists", screen);
        return;

    } else if (name == "home") {
        st.view = ViewMode::Home;
        st.set_log("type :help  ·  :play <query> to search");


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
        st.current_lyrics = Lyrics();
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

    } else if (name == "radio") {
        st.set_log("loading radio paradise flac channels...");
        std::vector<Track> rp_tracks = {
            {"Radio Paradise (Main)", "FLAC Stream", "", 0, "podcast", "http://stream.radioparadise.com/flacm", ""},
            {"Radio Paradise (Mellow)", "FLAC Stream", "", 0, "podcast", "http://stream.radioparadise.com/mellow-flac", ""},
            {"Radio Paradise (Rock)", "FLAC Stream", "", 0, "podcast", "http://stream.radioparadise.com/rock-flac", ""},
            {"Radio Paradise (World)", "FLAC Stream", "", 0, "podcast", "http://stream.radioparadise.com/world-etc-flac", ""}
        };
        st.queue.load(rp_tracks);
        st.is_search_queue = false;
        st.current_local_playlist.clear();
        st.track_names.clear();
        for (auto& e : rp_tracks) st.track_names.push_back(e.title);
        st.view = ViewMode::Tracks;
        st.selected = 0;
        st.set_log("loaded 4 Radio Paradise FLAC channels");

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

    } else if (name == "loudnorm") {
        bool current = false;
        // Note: keeping it simple by just toggling, proper state would be in config
        if (st.log_msg.find("ON") != std::string::npos) current = true;
        if (!current) {
            st.player.set_loudnorm(true);
            st.set_log("audio normalization: ON");
        } else {
            st.player.set_loudnorm(false);
            st.set_log("audio normalization: OFF");
        }

    } else if (name == "pl-create") {
        if (args.empty()) { st.set_log("usage: :pl-create <name>", true); return; }
        if (st.local_playlists.create(args)) st.set_log("created local playlist: " + args);
        else st.set_log("failed to create or already exists", true);

    } else if (name == "pl-add") {
        if (args.empty()) { st.set_log("usage: :pl-add <name>", true); return; }
        if (st.view == ViewMode::Tracks && !st.queue.empty()) {
            if (!st.selected_indices.empty()) {
                int count = 0;
                for (int idx : st.selected_indices) {
                    if (idx >= 0 && idx < st.queue.length()) {
                        if (st.local_playlists.add_track(args, st.queue.tracks()[idx])) count++;
                    }
                }
                st.selected_indices.clear();
                st.set_log("added " + std::to_string(count) + " tracks to " + args);
            } else {
                auto& t = st.queue.tracks()[st.selected];
                if (st.local_playlists.add_track(args, t)) st.set_log("added to " + args);
                else st.set_log("failed to add track", true);
            }
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

    } else if (name == "find") {
        if (args.empty()) { st.set_log("usage: :find <text>", true); return; }
        if (st.view != ViewMode::Tracks && st.view != ViewMode::Playlists) {
            st.set_log("not in a list view", true);
            return;
        }
        auto& list = (st.view == ViewMode::Tracks) ? st.track_names : st.pl_names;
        std::string q = args;
        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
        bool found = false;
        for (size_t i = 0; i < list.size(); ++i) {
            std::string item = list[i];
            std::transform(item.begin(), item.end(), item.begin(), ::tolower);
            if (item.find(q) != std::string::npos) {
                if (st.view == ViewMode::Tracks) st.selected = i;
                else st.pl_selected = i;
                st.set_log("found: " + list[i]);
                found = true;
                break;
            }
        }
        if (!found) st.set_log("no match for: " + args, true);


    } else if (name == "history") {
        auto entries = st.history.get_recent(50);
        if (entries.empty()) { st.info_text = "No play history yet."; }
        else {
            std::string text = "Recent plays:\n\n";
            for (auto& e : entries) {
                char buf[32];
                struct tm* tmi = localtime(&e.played_at);
                strftime(buf, sizeof(buf), "%m/%d %H:%M", tmi);
                text += std::string(buf) + "  " + e.track.display() + "\n";
            }
            st.info_text = text;
        }
        st.view = ViewMode::Info;

    } else if (name == "lyrics") {
        std::string artist = st.player.current_artist();
        std::string title  = st.player.current_title();
        
        // If mpv extracted a garbage URL string or missing artist, fall back to queue
        bool mpv_garbage = title.find("=") != std::string::npos || title.find("http") == 0 || title.find(".mp4") != std::string::npos;
        
        if (mpv_garbage || artist.empty()) {
            if (!st.queue.empty()) {
                int ci = st.queue.current_index();
                if (ci >= 0 && ci < st.queue.length()) {
                    artist = st.queue.tracks()[ci].artist;
                    title = st.queue.tracks()[ci].title;
                }
            }
        }
        
        if (title.empty()) { st.set_log("nothing playing", true); return; }
        st.set_log("fetching lyrics...");
        std::thread([&st, &screen, artist, title]() {
            Lyrics lyr = fetch_lyrics(artist, title);
            std::lock_guard<std::mutex> lk(st.mtx);
            if (lyr.plain.find("Lyrics not found") != std::string::npos || lyr.plain.find("Network error") != std::string::npos) {
                st.set_log(lyr.plain, true);
            } else {
                st.current_lyrics = lyr;
                st.info_text = lyr.plain;
                st.set_log("lyrics loaded");
                if (lyr.lines.empty()) {
                    st.view = ViewMode::Info;
                }
            }
            screen.Post(Event::Custom);
        }).detach();

    } else if (name == "download") {
        std::string url;
        if (!args.empty()) { url = args; }
        else if (!st.queue.empty()) {
            int ci = st.queue.current_index();
            if (ci < 0) ci = st.selected;
            if (ci >= 0 && ci < st.queue.length()) url = st.queue.tracks()[ci].uri;
        }
        if (url.empty()) { st.set_log("usage: :download <url>  or select a youtube track", true); return; }
        st.set_log("downloading to ~/Music/ ...");
        std::thread([&st, &screen, url]() {
            std::string dir = yt_download(url);
            std::lock_guard<std::mutex> lk(st.mtx);
            st.set_log(dir.empty() ? "download failed" : "downloaded to " + dir, dir.empty());
            screen.Post(Event::Custom);
        }).detach();

    } else if (name == "podcast") {
        if (args.empty()) { st.set_log("usage: :podcast <rss-url>", true); return; }
        st.set_log("loading podcast...");
        std::string rss = args;
        std::thread([&st, &screen, rss]() {
            auto eps = fetch_podcast(rss, 30);
            std::lock_guard<std::mutex> lk(st.mtx);
            if (eps.empty()) { st.set_log("no episodes found", true); }
            else {
                st.queue.load(eps);
                st.is_search_queue = false;
                st.current_local_playlist.clear();
                st.track_names.clear();
                for (auto& e : eps) st.track_names.push_back(e.title);
                st.view = ViewMode::Tracks;
                st.selected = 0;
                st.set_log("loaded " + std::to_string(eps.size()) + " episodes");
            }
            screen.Post(Event::Custom);
        }).detach();

    } else if (name == "pl-rename") {
        auto sp = args.find(' ');
        if (sp == std::string::npos) { st.set_log("usage: :pl-rename <old> <new>", true); return; }
        std::string old_name = args.substr(0, sp);
        std::string new_name = args.substr(sp + 1);
        if (st.local_playlists.rename(old_name, new_name)) {
            if (st.current_local_playlist == old_name) st.current_local_playlist = new_name;
            st.set_log("renamed to: " + new_name);
        } else {
            st.set_log("rename failed (not found or name taken)", true);
        }

    } else if (name == "pl-export") {
        if (args.empty()) { st.set_log("usage: :pl-export <playlist-name>", true); return; }
        const char* home = std::getenv("HOME");
        std::string out = (home ? std::string(home) : "/tmp") + "/" + args + ".m3u8";
        if (st.local_playlists.export_m3u(args, out))
            st.set_log("exported to " + out);
        else
            st.set_log("export failed: \"" + args + "\" not found", true);

    } else if (name == "tag") {
        if (args.empty()) { st.set_log("usage: :tag <label>", true); return; }
        std::string uri;
        if (!st.queue.empty()) {
            int ci = st.queue.current_index(); if (ci < 0) ci = st.selected;
            if (ci >= 0 && ci < st.queue.length()) uri = st.queue.tracks()[ci].uri;
        }
        if (uri.empty()) { st.set_log("select a track first", true); return; }
        st.local_playlists.add_tag(uri, args);
        st.set_log("tagged: " + args);

    } else if (name == "untag") {
        if (args.empty()) { st.set_log("usage: :untag <label>", true); return; }
        std::string uri;
        if (!st.queue.empty()) {
            int ci = st.queue.current_index(); if (ci < 0) ci = st.selected;
            if (ci >= 0 && ci < st.queue.length()) uri = st.queue.tracks()[ci].uri;
        }
        if (uri.empty()) { st.set_log("select a track first", true); return; }
        st.local_playlists.remove_tag(uri, args);
        st.set_log("removed tag: " + args);

    } else if (name == "tagged") {
        if (args.empty()) { st.set_log("usage: :tagged <label>", true); return; }
        auto tracks = st.local_playlists.get_tagged(args);
        if (tracks.empty()) { st.set_log("no tracks tagged: " + args, true); return; }
        st.queue.load(tracks);
        st.is_search_queue = false;
        st.current_local_playlist.clear();
        st.track_names.clear();
        for (auto& t : tracks) st.track_names.push_back(t.title);
        st.view = ViewMode::Tracks;
        st.selected = 0;
        st.set_log("tagged \"" + args + "\": " + std::to_string(tracks.size()) + " tracks");

    } else if (name == "tags") {
        auto all = st.local_playlists.all_tags();
        st.info_text = all.empty() ? "No tags yet.\n\nUse :tag <label> on a selected track." :
                       "All tags:\n\n";
        for (auto& t : all) st.info_text += "  " + t + "\n";
        st.view = ViewMode::Info;

    } else if (name == "update") {
        st.set_log("updating yt-dlp...");
        std::thread([&st, &screen]() {
            int ret = system("pip install -U yt-dlp 2>/dev/null || pip3 install -U yt-dlp 2>/dev/null");
            std::lock_guard<std::mutex> lk(st.mtx);
            if (ret == 0) { st.ytdlp_warn.clear(); st.set_log("yt-dlp updated: " + ytdlp_version()); }
            else st.set_log("update failed — try: pip install -U yt-dlp", true);
            screen.Post(Event::Custom);
        }).detach();

    } else if (name == "sponsorblock") {
        if (args == "on" || args == "off") {
            st.config.sponsorblock_enabled = (args == "on");
            st.config.save();
        }
        st.set_log(std::string("sponsorblock: ") + (st.config.sponsorblock_enabled ? "on" : "off"));

    } else if (name == "scrobble") {
        if (args == "on" || args == "off") {
            st.config.scrobble_enabled = (args == "on");
            st.config.save();
        }
        st.set_log(std::string("scrobbling: ") + (st.config.scrobble_enabled ? "on" : "off") +
                   " | configured: " + (st.scrobbler.is_configured() ? "yes" : "no"));
    }
}


// ── Pre-fetch, notify, scrobble, sponsorblock helpers ─────────────────────────

static void prefetch_next(AppState& st, ftxui::ScreenInteractive* screen) {
    int next_idx = st.queue.current_index() + 1;
    if (next_idx < 0 || next_idx >= st.queue.length()) return;
    const Track& next = st.queue.tracks()[next_idx];
    if (next.source != "youtube" || next.uri.empty()) return;
    if (st.stream_cache.count(next.uri)) return;
    if (st.prefetch_uri == next.uri) return;

    std::string uri = next.uri;
    st.prefetch_uri = uri;
    std::thread([&st, screen, uri]() {
        std::string url = yt_stream_url(uri);
        std::lock_guard<std::mutex> lk(st.mtx);
        if (!url.empty()) { st.stream_cache[uri] = url; }
        st.prefetch_uri.clear();
        if (screen) screen->Post(Event::Custom);
    }).detach();
}

static void notify_track(const Track& t) {
    std::string body = t.artist.empty() ? t.title : t.artist + " \xe2\x80\x94 " + t.title;
    for (auto& c : body) if (c == '"') c = '\'';
    system(("notify-send -t 4000 -i audio-x-generic \"txsyxts\" \"" + body + "\" 2>/dev/null &").c_str());
}

static void on_track_started(AppState& st, const Track& t,
                              ftxui::ScreenInteractive* screen,
                              const std::string& stream_url) {
    st.history.record(t);
    if (!t.uri.empty()) st.played_uris.insert(t.uri);
    if (st.config.notify_enabled) notify_track(t);

    st.track_started_at = std::time(nullptr);
    st.scrobbled_current = false;
    if (st.config.scrobble_enabled && st.scrobbler.is_configured())
        st.scrobbler.now_playing(t);

    st.current_lyrics = Lyrics();
    if (st.auto_lyrics) {
        // Automatically invoke the lyrics fetcher
        handle_command(st, "lyrics", *screen);
    }

    // Fetch SponsorBlock segments in background
    if (st.config.sponsorblock_enabled) {
        std::string video_url = t.uri.empty() ? stream_url : t.uri;
        std::string vid = yt_video_id(video_url);
        if (!vid.empty() && vid != st.sponsor_video_id) {
            std::thread([&st, vid]() {
                auto segs = sponsorblock_get(vid);
                std::lock_guard<std::mutex> lk(st.mtx);
                st.sponsor_segments = segs;
                st.sponsor_video_id = vid;
            }).detach();
        }
    }

    prefetch_next(st, screen);
    int ci = st.queue.current_index();
    if (ci >= 0) st.selected = ci;
}

// ── play a track (resolve spotify→youtube if needed) ──────────
// skip_count: number of tracks auto-skipped so far due to failure.
// Capped at 3 to avoid an infinite skip loop on a dead queue.

static void play_track(AppState& st, const Track& track, ftxui::ScreenInteractive* screen, int skip_count) {
    if (track.source == "local" || track.source == "podcast") {
        st.player.play(track.uri, track.title, track.artist);
        st.set_log("\u25b6 " + track.display());
        on_track_started(st, track, screen, track.uri);
        return;
    }

    if (track.source == "youtube" && !track.uri.empty()) {
        // Check stream cache first — instant start if pre-fetched
        auto cached = st.stream_cache.find(track.uri);
        if (cached != st.stream_cache.end() && !cached->second.empty()) {
            st.player.play(cached->second, track.title, track.artist);
            st.set_log("\u25b6 " + track.display() + " (instant)");
            on_track_started(st, track, screen, cached->second);
            if (screen) screen->Post(Event::Custom);
            return;
        }

        st.set_log("resolving stream: " + track.title + "...");
        if (screen) screen->Post(Event::Custom);
        Track t_copy = track;
        std::thread([&st, screen, t_copy, skip_count]() {
            std::string stream_url = yt_stream_url(t_copy.uri);
            std::lock_guard<std::mutex> lk(st.mtx);
            if (!stream_url.empty()) {
                st.stream_cache[t_copy.uri] = stream_url;
                st.player.play(stream_url, t_copy.title, t_copy.artist);
                st.set_log("\u25b6 " + t_copy.display());
                on_track_started(st, t_copy, screen, stream_url);
            } else if (skip_count < 3) {
                st.set_log("skipping (unavailable): " + t_copy.title + "...", true);
                auto next = st.queue.next();
                if (next) {
                    int ci = st.queue.current_index();
                    if (ci >= 0) st.selected = ci;
                    play_track(st, *next, screen, skip_count + 1);
                } else {
                    st.set_log("queue ended");
                }
            } else {
                st.set_log("too many failures in a row \u2014 stopping", true);
            }
            if (screen) screen->Post(Event::Custom);
        }).detach();
        return;
    }

    // spotify → search youtube
    st.set_log("finding: " + track.search_query() + "...");
    if (screen) screen->Post(Event::Custom);

    std::string q = track.search_query();
    Track t_copy = track;

    std::thread([&st, screen, q, t_copy, skip_count]() {
        auto tracks = yt_search(q, 1);
        if (!tracks.empty() && !tracks[0].uri.empty()) {
            std::string stream_url = yt_stream_url(tracks[0].uri);
            std::lock_guard<std::mutex> lk(st.mtx);
            if (!stream_url.empty()) {
                st.stream_cache[t_copy.uri] = stream_url;
                st.player.play(stream_url, t_copy.title, t_copy.artist);
                st.set_log("\u25b6 " + t_copy.display());
                on_track_started(st, t_copy, screen, stream_url);
            } else if (skip_count < 3) {
                st.set_log("skipping (unavailable): " + t_copy.title + "...", true);
                auto next = st.queue.next();
                if (next) {
                    int ci = st.queue.current_index();
                    if (ci >= 0) st.selected = ci;
                    play_track(st, *next, screen, skip_count + 1);
                } else {
                    st.set_log("queue ended");
                }
            } else {
                st.set_log("too many failures in a row \u2014 stopping", true);
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

    // yt-dlp version check background
    std::thread([&st]() {
        if (!ytdlp_is_recent("2024.11.18")) {
            std::lock_guard<std::mutex> lk(st.mtx);
            st.ytdlp_warn = "yt-dlp is outdated! Run :update or some streams may fail.";
        }
    }).detach();

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
            st.set_log("\u27f3 finding similar songs...");
            screen.Post(Event::Custom);

            std::thread([&st, &screen, last_uri]() {
                auto recs = yt_get_similar(last_uri, 5);
                if (!recs.empty()) {
                    std::lock_guard<std::mutex> lk(st.mtx);

                    // Filter out recently played tracks to avoid loops
                    std::vector<Track> filtered;
                    for (auto& r : recs) {
                        if (st.played_uris.count(r.uri) == 0 && r.uri != last_uri) {
                            filtered.push_back(r);
                        }
                    }

                    // Fallback to recent history recommendations if mix is exhausted
                    if (filtered.empty()) {
                        auto hist_uris = st.history.get_recent_uris(5);
                        for (auto& hu : hist_uris) {
                            auto hist_recs = yt_get_similar(hu, 5);
                            for (auto& hr : hist_recs) {
                                if (st.played_uris.count(hr.uri) == 0) {
                                    filtered.push_back(hr);
                                    break;
                                }
                            }
                            if (!filtered.empty()) break;
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
                            st.set_log("\u27f3 autoplay: " + first->display());
                        }
                    } else {
                        st.set_log("autoplay: no new unplayed similar songs found", true);
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

    auto btn_prev = Button(" \u23ee  ", [&]{ auto t = st.queue.prev(); if(t){ play_track(st, *t, &screen); update_ascii_art(st, *t, screen); } }, ButtonOption::Animated(Color::GrayDark, Color::GrayLight, Color::White, Color::White));
    auto btn_play = Button(" \u23ef  ", [&]{ st.player.pause(); }, ButtonOption::Animated(Color::RGB(152, 195, 121), Color::RGB(152, 195, 121), Color::White, Color::White));
    auto btn_next = Button(" \u23ed  ", [&]{ auto t = st.queue.next(); if(t){ play_track(st, *t, &screen); update_ascii_art(st, *t, screen); } }, ButtonOption::Animated(Color::GrayDark, Color::GrayLight, Color::White, Color::White));

    auto btn_loop = Button(&st.loop_btn_text, [&]{
        st.queue.cycle_loop();
        st.set_log("loop: " + st.queue.loop_str());
        st.loop_btn_text = " \U0001f501 Loop: " + st.queue.loop_str() + " ";
    }, ButtonOption::Animated(Color::GrayDark, Color::GrayLight, Color::White, Color::White));

    auto btn_autoplay = Button(&st.autoplay_btn_text, [&]{
        st.autoplay = !st.autoplay;
        st.set_log(std::string("autoplay: ") + (st.autoplay ? "on" : "off"));
        st.autoplay_btn_text = " \u221e Autoplay: " + std::string(st.autoplay ? "On " : "Off ");
    }, ButtonOption::Animated(Color::GrayDark, Color::GrayLight, Color::White, Color::White));

    auto btn_autolyrics = Button(&st.autolyrics_btn_text, [&]{
        st.auto_lyrics = !st.auto_lyrics;
        st.set_log(std::string("auto lyrics: ") + (st.auto_lyrics ? "on" : "off"));
        st.autolyrics_btn_text = " ♫ Lyrics: " + std::string(st.auto_lyrics ? "On " : "Off ");
        if (st.auto_lyrics && st.current_lyrics.lines.empty() && st.info_text.empty() && !st.player.current_title().empty()) {
            handle_command(st, "lyrics", screen);
        }
    }, ButtonOption::Animated(Color::GrayDark, Color::GrayLight, Color::White, Color::White));

    auto controls = Container::Horizontal({ btn_prev, btn_play, btn_next, btn_loop, btn_autoplay, btn_autolyrics });

    MenuOption track_opt;
    track_opt.on_enter = [] {};
    track_opt.entries_option.transform = [&](const EntryState& state) {
        bool focused = state.focused;
        if (st.view == ViewMode::Playlists) {
            auto& pl = st.playlists[state.index];
            bool active = (state.index == st.pl_selected);
            char idx[16]; snprintf(idx, sizeof(idx), "%3d", state.index + 1);
            std::string prefix = active ? "\u25b6" : " ";
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
            std::string marker = playing ? "\u25b6" : " ";
            if (st.selected_indices.count(state.index)) marker = "[✓]";
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

        // Check SponsorBlock
        if (st.config.sponsorblock_enabled && st.player.is_playing() && !st.sponsor_segments.empty()) {
            double pos = st.player.position();
            double skip_to = sponsorblock_should_skip(pos, st.sponsor_segments);
            if (skip_to > 0) {
                st.player.seek_absolute(skip_to);
                st.set_log("SponsorBlock: skipped to " + fmt_time(skip_to));
            }
        }

        // Check Last.fm Scrobble (scrobble after 50% or 4 minutes)
        if (st.config.scrobble_enabled && st.player.is_playing() && !st.scrobbled_current && st.track_started_at > 0) {
            double pos = st.player.position();
            double dur = st.player.duration();
            int elapsed = std::time(nullptr) - st.track_started_at;
            if ((dur > 0 && pos > dur * 0.5) || elapsed > 240) {
                int ci = st.queue.current_index();
                if (ci >= 0 && ci < st.queue.length()) {
                    st.scrobbler.scrobble(st.queue.tracks()[ci], st.track_started_at);
                    st.scrobbled_current = true;
                }
            }
        }

        // ── header ─────────────────────────────────────────────
        std::string status;
        if (st.spotify.is_logged_in()) status += "spotify \u2713  ";
        status += "mpv \u2713";

        auto header = hbox({
            text("txsyxts") | bold | color(Color::RGB(152, 195, 121)),
            filler(),
            text(status) | color(Color::GrayDark),
        });

        // ── track list or info ─────────────────────────────────
        Element body;
        if (st.view == ViewMode::Info && !st.info_text.empty()) {
            body = paragraph(st.info_text) | color(Color::GrayLight) | vscroll_indicator | yframe;
        } else if (st.view == ViewMode::Playlists && !st.playlists.empty()) {
            body = menu_pl->Render() | vscroll_indicator | yframe;
        } else if (st.view == ViewMode::Home || st.queue.empty()) {
            body = vbox({
                text("") | size(HEIGHT, EQUAL, 2),
                text("  j/k=navigate  Enter=play  a=add to playlist  :search <query>  :help")
                    | center | color(Color::GrayDark),
            });
        } else {
            body = menu_tr->Render() | vscroll_indicator | yframe;
        }

        // ── now playing bar ────────────────────────────────────
        std::string np_title = st.player.current_title();
        std::string np_artist = st.player.current_artist();
        
        bool np_garbage = np_title.find("=") != std::string::npos || np_title.find("http") == 0 || np_title.find(".mp4") != std::string::npos;
        if (np_garbage || np_artist.empty()) {
            if (!st.queue.empty()) {
                int ci = st.queue.current_index();
                if (ci >= 0 && ci < st.queue.length()) {
                    np_artist = st.queue.tracks()[ci].artist;
                    np_title = st.queue.tracks()[ci].title;
                }
            }
        }
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
        
        int vol_val = st.player.volume();
        int v_bars = vol_val / 10;
        if (v_bars > 15) v_bars = 15;
        std::string v_filled; for(int i=0; i<v_bars; i++) v_filled += "█";
        std::string v_empty; for(int i=0; i<15-v_bars; i++) v_empty += "░";
        std::string vol_str = "vol: " + v_filled + v_empty + " " + std::to_string(vol_val);

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
            }) | reflect(st.progress_box),
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

        Element lyrics_el = filler();
        if (!st.current_lyrics.lines.empty()) {
            Elements l_els;
            double pos = st.player.position();
            int active_idx = -1;
            for (int i = 0; i < (int)st.current_lyrics.lines.size(); ++i) {
                if (pos >= st.current_lyrics.lines[i].time_sec) active_idx = i;
            }
            int start = std::max(0, active_idx - 3);
            int end = std::min((int)st.current_lyrics.lines.size(), active_idx + 4);
            for (int i = start; i < end; ++i) {
                bool active = (i == active_idx);
                l_els.push_back(text("  " + st.current_lyrics.lines[i].text) 
                    | (active ? color(Color::RGB(152, 195, 121)) : color(Color::GrayDark))
                    | (active ? bold : nothing));
            }
            lyrics_el = vbox(std::move(l_els)) | center;
        }

        auto left_panel = vbox({
            (st.current_art_url.empty() ? logo : (st.current_art | border | bgcolor(Color::RGB(15,15,15)))),
            filler(),
            lyrics_el,
            filler(),
            viz | center
        }) | size(WIDTH, EQUAL, 60);

        auto main_content = hbox({
            left_panel,
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
        if (event == Event::Return && !st.show_modal) {
            if (!input_str.empty()) {
                std::string cmd = input_str;
                input_str.clear();
                handle_command(st, cmd, screen);
                return true;
            } else if (st.view == ViewMode::Tracks && !st.queue.empty()) {
                if (!st.selected_indices.empty()) {
                    std::vector<Track> selected_tracks;
                    std::vector<int> sorted_idx(st.selected_indices.begin(), st.selected_indices.end());
                    std::sort(sorted_idx.begin(), sorted_idx.end());
                    for (int i : sorted_idx) {
                        if (i >= 0 && i < st.queue.length()) selected_tracks.push_back(st.queue.tracks()[i]);
                    }
                    st.queue.load(selected_tracks);
                    st.track_names.clear();
                    for (auto& t : selected_tracks) st.track_names.push_back(t.title);
                    st.selected_indices.clear();
                    st.selected = 0;
                    st.queue.jump(0);
                    play_track(st, st.queue.tracks()[0], &screen);
                    update_ascii_art(st, st.queue.tracks()[0], screen);
                    st.set_log("playing selected batch");
                } else {
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
                }
                return true;
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

        // Shift+- (underscore) — remove selected song from current local playlist
        if (event == Event::Character('_') && input_str.empty()) {
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
                    st.set_log("not in a local playlist (use Shift+- only in playlist view)", true);
                }
            }
            return true;
        }

        // Space to pause
        if (event == Event::Character(' ') && input_str.empty()) {
            st.player.pause();
            return true;
        }

        // Search-as-you-type ( / )
        if (event == Event::Character('/') && input_str.empty()) {
            input_str = "search ";
            return true;
        }

        // Visual select mode
        if (event == Event::Character('v') && input_str.empty()) {
            if (st.view == ViewMode::Tracks && !st.queue.empty()) {
                if (st.selected_indices.count(st.selected)) {
                    st.selected_indices.erase(st.selected);
                } else {
                    st.selected_indices.insert(st.selected);
                }
                if (st.selected < st.queue.length() - 1) st.selected++;
                return true;
            }
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

        // Volume controls
        if ((event == Event::Character('-') || event == Event::Character('=') || event == Event::Character('+')) && input_str.empty()) {
            int step = event == Event::Character('-') ? -5 : 5;
            int new_vol = std::max(0, std::min(150, st.player.volume() + step));
            st.player.set_volume(new_vol);
            st.config.volume = new_vol;
            st.config.save();
            return true;
        }

        // Handle other keybindings that Menu doesn't catch
        if (event == Event::Character('f') && input_str.empty()) {
            input_str = "find ";
            return true;
        }

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

        // Progress bar seeking
        if (event.is_mouse() && event.mouse().button == Mouse::Left && event.mouse().motion == Mouse::Pressed) {
            auto& box = st.progress_box;
            if (event.mouse().x >= box.x_min && event.mouse().x <= box.x_max &&
                event.mouse().y >= box.y_min && event.mouse().y <= box.y_max) {
                int width = box.x_max - box.x_min;
                if (width > 0 && st.player.duration() > 0) {
                    // Approximate click pos to progress bar (ignoring the timestamps on sides)
                    // The bar string is 40 chars long, but we just use the raw x percentage within the box
                    double pct = (double)(event.mouse().x - box.x_min) / width;
                    if (pct < 0) pct = 0;
                    if (pct > 1) pct = 1;
                    st.player.seek_absolute(pct * st.player.duration());
                }
                return true;
            }
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

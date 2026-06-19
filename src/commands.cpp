#include "commands.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

namespace txs {

static const std::unordered_map<std::string, std::string> ALIASES = {
    {"p", "pause"}, {"n", "next"}, {"s", "stop"}, {"q", "queue"},
    {"v", "vol"}, {"sh", "shuffle"}, {"lo", "loop"}, {"h", "help"},
    {"exit", "quit"}, {"f", "find"},
};

static const std::unordered_set<std::string> COMMANDS = {
    "login", "liked", "play", "pause", "next", "prev", "stop",
    "queue", "vol", "search", "shuffle", "loop", "local", "config",
    "playlists", "playlist", "ytfzf", "seek", "clear", "help", "quit", "home",
    "radio", "find",
    // playlist management
    "pl-create", "pl-add", "pl-remove", "pl-delete", "pl-rename", "pl-export",
    // new features
    "history", "lyrics", "download", "podcast",
    "tag", "untag", "tagged", "tags",
    "update", "sponsorblock", "scrobble",
};

std::optional<Command> parse_command(const std::string& input) {
    std::string text = input;

    // trim
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    while (!text.empty() && text.back() == ' ') text.pop_back();

    // strip leading ':'
    if (!text.empty() && text[0] == ':') text = text.substr(1);
    if (text.empty()) return std::nullopt;

    // split name and args
    std::string name, args;
    auto sp = text.find(' ');
    if (sp != std::string::npos) {
        name = text.substr(0, sp);
        args = text.substr(sp + 1);
        while (!args.empty() && args.front() == ' ') args.erase(args.begin());
    } else {
        name = text;
    }

    // lowercase
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    // resolve alias
    auto it = ALIASES.find(name);
    if (it != ALIASES.end()) name = it->second;

    if (COMMANDS.find(name) == COMMANDS.end()) return std::nullopt;

    return Command{name, args};
}

const char* HELP_TEXT =
    "━━ playback ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
    "  :play <url>          stream a youtube video/playlist\n"
    "  :play <n>            play track #n from list\n"
    "  :search <query>      search youtube, show results\n"
    "  :find <text> / :f    jump to matching track in current list\n"
    "  :radio <genre>       start a genre radio station\n"
    "  :local <path>        load local audio files\n"
    "  :podcast <rss-url>   load podcast episodes from RSS\n"
    "  :pause / :p          toggle pause\n"
    "  :next / :n           next track\n"
    "  :prev                previous track\n"
    "  :stop / :s           stop playback\n"
    "  :seek <±sec>         seek forward/back\n"
    "  :vol <0-150>         set volume\n"
    "  :shuffle / :sh       toggle shuffle\n"
    "  :loop / :lo          cycle loop (off→track→all)\n"
    "  :clear               clear track list\n"
    "\n"
    "━━ playlist management ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
    "  :playlists           list local playlists\n"
    "  :pl-create <name>    create local playlist\n"
    "  :pl-add <name>       add selected track to playlist\n"
    "  :pl-remove <name>    remove selected track from playlist\n"
    "  :pl-delete <name>    delete a local playlist\n"
    "  :pl-rename <old> <new>  rename a playlist\n"
    "  :pl-export <name>    export playlist to ~/name.m3u8\n"
    "\n"
    "━━ track tagging ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
    "  :tag <label>         tag selected track (e.g. chill)\n"
    "  :untag <label>       remove tag from selected track\n"
    "  :tagged <label>      show all tracks with this tag\n"
    "  :tags                list all tags in use\n"
    "\n"
    "━━ features & info ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
    "  :lyrics              show lyrics for current track\n"
    "  :history             show recent play history\n"
    "  :download [url]      download to ~/Music/ via yt-dlp\n"
    "  :sponsorblock on/off toggle SponsorBlock (auto-skip ads)\n"
    "  :scrobble on/off     toggle Last.fm scrobbling\n"
    "  :update              update yt-dlp to latest version\n"
    "  :config              show/edit config\n"
    "  :home                go to home screen\n"
    "  :help / :h           this help\n"
    "  :quit / exit         quit\n"
    "\n"
    "━━ keys ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
    "  j/k or ↑↓           navigate\n"
    "  Enter                play selected track\n"
    "  d / Del              remove from current playlist\n"
    "  _  (Shift+-)         remove song from playlist\n"
    "  Ctrl+N / Ctrl+P      next / previous track\n"
    "  Ctrl+Q               quit\n"
    "  b                    back to playlists view";

} // namespace txs

#include "commands.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

namespace txs {

static const std::unordered_map<std::string, std::string> ALIASES = {
    {"p", "pause"}, {"n", "next"}, {"s", "stop"}, {"q", "queue"},
    {"v", "vol"}, {"sh", "shuffle"}, {"lo", "loop"}, {"h", "help"},
    {"exit", "quit"},
};

static const std::unordered_set<std::string> COMMANDS = {
    "login", "liked", "play", "pause", "next", "prev", "stop",
    "queue", "vol", "search", "shuffle", "loop", "local", "config",
    "playlists", "playlist", "ytfzf", "seek", "clear", "help", "quit", "home",
    "pl-create", "pl-add", "pl-remove", "pl-delete",
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
        // trim args
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
    "commands:\n"
    "  :play <url>         stream a youtube video or playlist directly\n"
    "  :play <n>           play track #n from list\n"
    "  :search <query>     search youtube, show results\n"
    "  :local <path>       load local audio files\n"
    "  :pl-create <name>   create local playlist\n"
    "  :pl-add <name>      add selected track to playlist\n"
    "  :pl-remove <name>   remove selected track from playlist\n"
    "  :pl-delete <name>   delete a local playlist\n"
    "  :playlists          list local playlists\n"
    "  :playlist           alias for :playlists\n"
    "  :home               go to home screen\n"
    "  :ytfzf <query>      stream via ytfzf directly\n"
    "  :pause / :p         toggle pause\n"
    "  :next / :n          next track\n"
    "  :prev               previous track\n"
    "  :stop / :s          stop playback\n"
    "  :seek <±sec>        seek forward/back\n"
    "  :vol <0-100>        set volume\n"
    "  :queue / :q         show queue\n"
    "  :shuffle / :sh      toggle shuffle\n"
    "  :loop / :lo         cycle loop (off>track>all)\n"
    "  :clear              clear track list\n"
    "  :config             show/edit config\n"
    "  :help / :h          this help\n"
    "  :quit / :exit       exit\n"
    "\n"
    "  keys: j/k=navigate  Enter=play  Space=pause\n"
    "        a/+=add to playlist  d/Del=remove from playlist\n"
    "        Shift+- (_)=remove song from current playlist\n"
    "        b=back to playlists  Ctrl+N=next  Ctrl+P=prev  Ctrl+Q=quit";

} // namespace txs

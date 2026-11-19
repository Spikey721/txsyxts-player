#include "app.hpp"
#include <fstream>

int main() {
    // Startup marker: makes it obvious from the log which build is running.
    std::ofstream("/tmp/txsyxts-ytdlp.log", std::ios::app)
        << "[app] started build=" TXSYXTS_BUILD_TAG "\n";
    return txs::run_app();
}

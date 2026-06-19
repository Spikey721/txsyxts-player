#include "sponsorblock.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace txs {

static size_t sb_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), realsize);
    return realsize;
}

std::string yt_video_id(const std::string& url) {
    // Handle youtu.be/ID
    auto pos2 = url.find("youtu.be/");
    if (pos2 != std::string::npos) {
        std::string id = url.substr(pos2 + 9);
        auto q = id.find('?');
        if (q != std::string::npos) id = id.substr(0, q);
        return id;
    }
    // Handle youtube.com/watch?v=ID
    auto pos = url.find("v=");
    if (pos != std::string::npos) {
        std::string id = url.substr(pos + 2);
        auto amp = id.find('&');
        if (amp != std::string::npos) id = id.substr(0, amp);
        return id;
    }
    return "";
}

std::vector<SponsorSegment> sponsorblock_get(const std::string& video_id) {
    std::vector<SponsorSegment> segs;
    if (video_id.empty()) return segs;

    // Categories to skip (can be expanded)
    std::string api_url =
        "https://sponsor.ajay.app/api/skipSegments?videoID=" + video_id +
        "&categories=[\"sponsor\",\"intro\",\"outro\",\"selfpromo\",\"interaction\"]";

    std::string response;
    CURL* curl = curl_easy_init();
    if (!curl) return segs;

    curl_easy_setopt(curl, CURLOPT_URL, api_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sb_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "txsyxts/0.1");
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (response.empty()) return segs;

    try {
        auto j = json::parse(response);
        for (auto& seg : j) {
            SponsorSegment s;
            auto segment = seg["segment"];
            s.start_sec = segment[0].get<double>();
            s.end_sec   = segment[1].get<double>();
            s.category  = seg.value("category", "sponsor");
            segs.push_back(s);
        }
    } catch (...) {}

    return segs;
}

double sponsorblock_should_skip(double pos_sec,
                                const std::vector<SponsorSegment>& segments) {
    for (const auto& seg : segments) {
        // Within 1 second of start counts as "in the segment"
        if (pos_sec >= seg.start_sec - 0.5 && pos_sec < seg.end_sec) {
            return seg.end_sec;
        }
    }
    return -1.0;
}

} // namespace txs

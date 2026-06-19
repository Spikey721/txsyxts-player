#pragma once
// SponsorBlock — skip sponsored segments in YouTube videos
// Free public API: https://sponsor.ajay.app
#include <string>
#include <vector>

namespace txs {

struct SponsorSegment {
    double start_sec;
    double end_sec;
    std::string category; // "sponsor", "intro", "outro", "selfpromo", etc.
};

// Fetch sponsor segments for a YouTube video ID.
// Returns empty vector if none found or API unreachable.
std::vector<SponsorSegment> sponsorblock_get(const std::string& video_id);

// Given current position and segments, return end of segment to skip to,
// or -1 if not currently in a segment.
double sponsorblock_should_skip(double pos_sec,
                                const std::vector<SponsorSegment>& segments);

// Extract video ID from a YouTube URL
std::string yt_video_id(const std::string& url);

} // namespace txs

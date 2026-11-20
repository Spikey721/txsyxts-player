#pragma once

#include <string>
#include <ftxui/dom/elements.hpp>

namespace txs {

// Fetches an image from a URL and converts it to a colored ASCII art Element
// Target width and height specifies the character dimensions of the output
ftxui::Element fetch_ascii_art(const std::string& url, int target_width, int target_height);

// Removes all kitty-protocol images from the terminal and forces the next
// art render to upload again (used when the cover changes position).
void invalidate_art_images();

} // namespace txs

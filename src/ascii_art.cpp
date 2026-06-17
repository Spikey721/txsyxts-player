#include "ascii_art.hpp"
#include <curl/curl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace ftxui;

namespace txs {

static size_t write_memory_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    auto* mem = static_cast<std::vector<uint8_t>*>(userp);
    mem->insert(mem->end(), static_cast<uint8_t*>(contents), static_cast<uint8_t*>(contents) + realsize);
    return realsize;
}

// We use Kitty's t=f mode to transmit the file directly. 
// Base64 of "/tmp/txsart.jpg" is "L3RtcC90eHNhcnQuanBn"
class KittyImageNode : public ftxui::Node {
public:
    KittyImageNode(int width, int height)
        : width_(width), height_(height) {
    }

    void ComputeRequirement() override {
        requirement_.min_x = width_;
        requirement_.min_y = height_;
    }

    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        if (!uploaded_ || last_x_ != box_.x_min || last_y_ != box_.y_min) {
            last_x_ = box_.x_min;
            last_y_ = box_.y_min;
            seq_.clear();

            // Use kitty +kitten icat directly to bypass any manual protocol logic!
            // We tell it to place the image exactly inside our box dimensions.
            std::string cmd = "kitty +kitten icat --transfer-mode=file --scale-up --place " + 
                              std::to_string(width_) + "x" + std::to_string(height_) + "@" + 
                              std::to_string(box_.x_min) + "x" + std::to_string(box_.y_min) + 
                              " /tmp/txsart.jpg 2>/dev/null";
            
            FILE* pipe = popen(cmd.c_str(), "r");
            if (pipe) {
                char buffer[128];
                while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    seq_ += buffer;
                }
                pclose(pipe);
            }
            uploaded_ = true;
        }

        // Put the sequence in the top-left pixel.
        screen.PixelAt(box_.x_min, box_.y_min).character = seq_;
    }

private:
    int width_, height_;
    int last_x_ = -1;
    int last_y_ = -1;
    bool uploaded_ = false;
    std::string seq_;
};

Element fetch_ascii_art(const std::string& url, int target_width, int target_height) {
    if (url.empty()) return text("");

    // 1. Download image
    std::vector<uint8_t> buffer;
    CURL* curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }

    if (buffer.empty()) {
        return text("");
    }

    // 2. Determine aspect ratio and calculate dynamic height
    int w = 0, h = 0, comp = 0;
    stbi_info_from_memory(buffer.data(), buffer.size(), &w, &h, &comp);
    
    if (w > 0 && h > 0) {
        // A terminal cell is roughly twice as tall as it is wide.
        target_height = (target_width * h) / (w * 2);
    }

    // Save as is to /tmp/txsart.jpg
    FILE* fp = fopen("/tmp/txsart.jpg", "wb");
    if (fp) {
        fwrite(buffer.data(), 1, buffer.size(), fp);
        fclose(fp);
    }

    // 2. Return Kitty graphics node
    return std::make_shared<KittyImageNode>(target_width, target_height);
}

} // namespace txs

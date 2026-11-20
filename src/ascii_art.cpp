#include "ascii_art.hpp"
#include <curl/curl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace ftxui;

namespace txs {

static std::atomic<uint64_t> g_art_generation{0};

void invalidate_art_images() {
    ++g_art_generation;
    // Delete all kitty placements directly on /dev/tty
    int tty = open("/dev/tty", O_WRONLY);
    if (tty >= 0) {
        const char* del = "\033_Ga=d\033\\";
        write(tty, del, strlen(del));
        close(tty);
    }
}

static size_t write_memory_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    auto* mem = static_cast<std::vector<uint8_t>*>(userp);
    mem->insert(mem->end(), static_cast<uint8_t*>(contents), static_cast<uint8_t*>(contents) + realsize);
    return realsize;
}

static std::string base64_encode(const std::vector<uint8_t>& in) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned int b = (unsigned int)in[i] << 16;
        if (i + 1 < in.size()) b |= (unsigned int)in[i + 1] << 8;
        if (i + 2 < in.size()) b |= (unsigned int)in[i + 2];
        out += tbl[(b >> 18) & 0x3f];
        out += tbl[(b >> 12) & 0x3f];
        out += (i + 1 < in.size()) ? tbl[(b >> 6) & 0x3f] : '=';
        out += (i + 2 < in.size()) ? tbl[b & 0x3f] : '=';
    }
    return out;
}

// Send the Kitty Graphics Protocol image directly to /dev/tty.
// We write chunked APC sequences (max 4096 bytes of base64 per chunk).
// a=T  = transmit + display
// t=d  = raw pixel data (RGBA)
// f=32 = RGBA format
// s=W,v=H = pixel dimensions
// c=cols,r=rows = terminal cell size
// m=1/0 = more chunks / last chunk
static void kitty_send_image(int tty_fd,
                              const uint8_t* rgba, int px_w, int px_h,
                              int col, int row,
                              int w_cells, int h_cells) {
    // Delete previous placements first
    std::string del = "\033_Ga=d\033\\";
    write(tty_fd, del.data(), del.size());

    // Move cursor to the target position using ANSI escape before transmitting.
    // Kitty places the image at the current cursor position.
    std::string move = "\033[" + std::to_string(row + 1) + ";" +
                                 std::to_string(col + 1) + "H";
    write(tty_fd, move.data(), move.size());

    // Base64-encode all RGBA pixels
    std::vector<uint8_t> raw(rgba, rgba + px_w * px_h * 4);
    std::string b64 = base64_encode(raw);

    const size_t CHUNK = 4096;
    size_t offset = 0;
    bool first = true;
    while (offset < b64.size()) {
        size_t end = std::min(offset + CHUNK, b64.size());
        bool last = (end >= b64.size());
        std::string chunk = b64.substr(offset, end - offset);
        offset = end;

        std::string seq;
        if (first) {
            // First chunk: include all parameters
            seq = "\033_Ga=T,t=d,f=32"
                  ",s=" + std::to_string(px_w) +
                  ",v=" + std::to_string(px_h) +
                  ",c=" + std::to_string(w_cells) +
                  ",r=" + std::to_string(h_cells) +
                  ",m=" + (last ? "0" : "1") +
                  ";" + chunk + "\033\\";
            first = false;
        } else {
            // Continuation chunks: only m= key
            seq = "\033_Gm=" + std::string(last ? "0" : "1") +
                  ";" + chunk + "\033\\";
        }
        write(tty_fd, seq.data(), seq.size());
    }
}

// Stored decoded image for rendering
struct DecodedImage {
    std::vector<uint8_t> rgba;
    int px_w = 0, px_h = 0;
    int cell_w = 0, cell_h = 0;
};

static std::shared_ptr<DecodedImage> g_decoded_image;

class KittyImageNode : public ftxui::Node {
public:
    KittyImageNode(int width, int height, std::shared_ptr<DecodedImage> img)
        : width_(width), height_(height), img_(std::move(img)) {}

    void ComputeRequirement() override {
        requirement_.min_x = width_;
        requirement_.min_y = height_;
    }

    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        uint64_t gen = g_art_generation.load();
        bool pos_changed = (last_x_ != box_.x_min || last_y_ != box_.y_min);
        bool gen_changed = (gen_ != gen);

        if (!uploaded_ || pos_changed || gen_changed) {
            gen_ = gen;
            last_x_ = box_.x_min;
            last_y_ = box_.y_min;
            uploaded_ = true;

            if (img_ && !img_->rgba.empty()) {
                int tty_fd = open("/dev/tty", O_WRONLY);
                if (tty_fd >= 0) {
                    kitty_send_image(tty_fd,
                                     img_->rgba.data(),
                                     img_->px_w, img_->px_h,
                                     box_.x_min, box_.y_min,
                                     width_, height_);
                    close(tty_fd);
                }
            }
        }

        // Reserve the space in the FTXUI layout (blank cells — image is an overlay)
        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            for (int x = box_.x_min; x <= box_.x_max; ++x) {
                screen.PixelAt(x, y).character = " ";
            }
        }
    }

private:
    int width_, height_;
    std::shared_ptr<DecodedImage> img_;
    int last_x_ = -1, last_y_ = -1;
    bool uploaded_ = false;
    uint64_t gen_ = 0;
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

    if (buffer.empty()) return text("");

    // 2. Decode image to RGBA pixels
    int px_w = 0, px_h = 0, comp = 0;
    uint8_t* pixels = stbi_load_from_memory(buffer.data(), (int)buffer.size(),
                                             &px_w, &px_h, &comp, 4);
    if (!pixels || px_w <= 0 || px_h <= 0) return text("");

    // Adjust cell height to preserve aspect ratio (terminal cell ~= 2:1 h/w)
    target_height = (target_width * px_h) / (px_w * 2);
    if (target_height < 1) target_height = 1;

    auto img = std::make_shared<DecodedImage>();
    img->rgba.assign(pixels, pixels + px_w * px_h * 4);
    img->px_w = px_w;
    img->px_h = px_h;
    img->cell_w = target_width;
    img->cell_h = target_height;
    stbi_image_free(pixels);

    g_decoded_image = img;
    return std::make_shared<KittyImageNode>(target_width, target_height, img);
}

} // namespace txs

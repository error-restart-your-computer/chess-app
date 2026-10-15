#pragma once
// text.h -- minimal FreeType-backed text rendering for SDL2 (no SDL_ttf needed)
#include <SDL.h>
#include <string>
#include <unordered_map>

enum class FontStyle { REGULAR = 0, BOLD = 1, MONO = 2 };

class TextRenderer {
public:
    bool init(SDL_Renderer* renderer);
    void shutdown();

    // align: 0 = left, 1 = centre on x, 2 = right edge at x
    void draw(const std::string& text, int px, SDL_Color color, int x, int y,
              int align = 0, FontStyle style = FontStyle::REGULAR);

    int width(const std::string& text, int px, FontStyle style = FontStyle::REGULAR);
    int lineHeight(int px, FontStyle style = FontStyle::REGULAR);

private:
    struct Entry {
        SDL_Texture* tex = nullptr;
        int w = 0;
        int h = 0;
    };

    Entry* lookup(const std::string& text, int px, SDL_Color color, FontStyle style);

    SDL_Renderer* renderer_ = nullptr;
    void* library_ = nullptr;   // FT_Library
    void* faces_[3] = {nullptr, nullptr, nullptr};
    std::unordered_map<std::string, Entry> cache_;
};

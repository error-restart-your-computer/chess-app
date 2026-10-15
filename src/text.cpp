#include "text.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

const char* kRegularCandidates[] = {
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    nullptr};

const char* kBoldCandidates[] = {
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    nullptr};

const char* kMonoCandidates[] = {
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    nullptr};

FT_Face loadFace(FT_Library lib, const char* const* candidates) {
    for (int i = 0; candidates[i]; ++i) {
        FT_Face face = nullptr;
        if (FT_New_Face(lib, candidates[i], 0, &face) == 0) return face;
    }
    // Last resort: ask FreeType for whatever it can find.
    FT_Face face = nullptr;
    for (int i = 0; candidates[i]; ++i) {
        if (FT_New_Face(lib, candidates[i], 0, &face) == 0) return face;
    }
    return nullptr;
}

// Decode one UTF-8 sequence starting at i and advance past it. Anything that is
// not valid UTF-8 turns into '?' so a stray byte can never index a wild glyph.
size_t nextCp(const std::string& s, size_t i, uint32_t& cp) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
        cp = c;
        return i + 1;
    }
    int extra;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; }
    else { cp = '?'; return i + 1; }
    size_t j = i + 1;
    for (int k = 0; k < extra; ++k) {
        if (j >= s.size()) break;
        const unsigned char cc = (unsigned char)s[j];
        if ((cc & 0xC0) != 0x80) break;
        cp = (cp << 6) | (cc & 0x3Fu);
        ++j;
    }
    return j;
}

}  // namespace

bool TextRenderer::init(SDL_Renderer* renderer) {
    renderer_ = renderer;
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) {
        std::fprintf(stderr, "chess: could not initialise FreeType\n");
        return false;
    }
    library_ = lib;
    faces_[0] = loadFace(lib, kRegularCandidates);
    faces_[1] = loadFace(lib, kBoldCandidates);
    faces_[2] = loadFace(lib, kMonoCandidates);
    if (!faces_[0]) faces_[0] = faces_[1];
    if (!faces_[0]) {
        std::fprintf(stderr, "chess: no usable font found\n");
        return false;
    }
    if (!faces_[1]) faces_[1] = faces_[0];
    if (!faces_[2]) faces_[2] = faces_[0];
    return true;
}

void TextRenderer::shutdown() {
    for (auto& kv : cache_)
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    cache_.clear();

    FT_Library lib = (FT_Library)library_;
    if (lib) {
        // Faces may alias each other when fallbacks kicked in; free each unique pointer once.
        for (int i = 0; i < 3; ++i) {
            bool dup = false;
            for (int j = 0; j < i; ++j)
                if (faces_[j] == faces_[i]) dup = true;
            if (faces_[i] && !dup) FT_Done_Face((FT_Face)faces_[i]);
            faces_[i] = nullptr;
        }
        FT_Done_FreeType(lib);
        library_ = nullptr;
    }
    renderer_ = nullptr;
}

TextRenderer::Entry* TextRenderer::lookup(const std::string& text, int px, SDL_Color color, FontStyle style) {
    char key[64];
    std::snprintf(key, sizeof(key), "%d|%d|%02x%02x%02x%02x|", px, int(style), color.r, color.g, color.b, color.a);
    std::string full = std::string(key) + text;

    auto it = cache_.find(full);
    if (it != cache_.end()) return &it->second;
    if (cache_.size() > 4000) {
        for (auto& kv : cache_)
            if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
        cache_.clear();
    }

    FT_Face face = (FT_Face)faces_[int(style)];
    if (!face) face = (FT_Face)faces_[0];
    if (FT_Set_Pixel_Sizes(face, 0, px) != 0) return nullptr;

    const int ascent = face->size->metrics.ascender >> 6;
    const int descent = -((face->size->metrics.descender) >> 6);
    const int height = std::max(1, ascent + descent);

    int pen = 0;
    FT_UInt prev = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        i = nextCp(text, i, cp);
        FT_UInt idx = FT_Get_Char_Index(face, cp);
        if (prev && idx) {
            FT_Vector kern;
            FT_Get_Kerning(face, prev, idx, FT_KERNING_DEFAULT, &kern);
            pen += kern.x >> 6;
        }
        if (FT_Load_Glyph(face, idx, FT_LOAD_DEFAULT) == 0) pen += face->glyph->advance.x >> 6;
        prev = idx;
    }
    const int w = std::max(1, pen + 1);

    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return nullptr;
    SDL_memset(surf->pixels, 0, (size_t)surf->h * surf->pitch);

    SDL_LockSurface(surf);
    uint32_t* pixels = (uint32_t*)surf->pixels;
    const int pitch = surf->pitch / 4;

    pen = 0;
    prev = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = 0;
        i = nextCp(text, i, cp);
        FT_UInt idx = FT_Get_Char_Index(face, cp);
        if (prev && idx) {
            FT_Vector kern;
            FT_Get_Kerning(face, prev, idx, FT_KERNING_DEFAULT, &kern);
            pen += kern.x >> 6;
        }
        if (FT_Load_Glyph(face, idx, FT_LOAD_RENDER) != 0) {
            prev = idx;
            continue;
        }
        FT_GlyphSlot g = face->glyph;
        const FT_Bitmap& bm = g->bitmap;
        const int left = pen + g->bitmap_left;
        const int top = ascent - g->bitmap_top;

        for (unsigned int row = 0; row < bm.rows; ++row) {
            int dy = top + (int)row;
            if (dy < 0 || dy >= height) continue;
            for (unsigned int col = 0; col < bm.width; ++col) {
                int dx = left + (int)col;
                if (dx < 0 || dx >= w) continue;
                unsigned char cov = bm.buffer[row * bm.pitch + col];
                if (!cov) continue;
                unsigned int a = (unsigned int)cov * color.a / 255u;
                pixels[dy * pitch + dx] = ((uint32_t)a << 24) | ((uint32_t)color.r << 16) |
                                          ((uint32_t)color.g << 8) | (uint32_t)color.b;
            }
        }
        pen += g->advance.x >> 6;
        prev = idx;
    }
    SDL_UnlockSurface(surf);

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer_, surf);
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    Entry& e = cache_[full];
    e.tex = tex;
    e.w = w;
    e.h = height;
    return &e;
}

void TextRenderer::draw(const std::string& text, int px, SDL_Color color, int x, int y, int align, FontStyle style) {
    if (text.empty()) return;
    Entry* e = lookup(text, px, color, style);
    if (!e || !e->tex) return;
    SDL_Rect dst{x, y, e->w, e->h};
    if (align == 1) dst.x = x - e->w / 2;
    else if (align == 2) dst.x = x - e->w;
    SDL_RenderCopy(renderer_, e->tex, nullptr, &dst);
}

int TextRenderer::width(const std::string& text, int px, FontStyle style) {
    if (text.empty()) return 0;
    Entry* e = lookup(text, px, SDL_Color{255, 255, 255, 255}, style);
    return e ? e->w : 0;
}

int TextRenderer::lineHeight(int px, FontStyle style) {
    FT_Face face = (FT_Face)faces_[int(style)];
    if (!face) face = (FT_Face)faces_[0];
    if (!face || FT_Set_Pixel_Sizes(face, 0, px) != 0) return px + 2;
    return (face->size->metrics.height >> 6) + 1;
}

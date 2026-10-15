#pragma once
// xoverlay.h -- a shaped, click-through X11 window drawn over the board
//
// SDL runs on the Wayland driver here, where window positions are guesses, so
// the overlay is placed through Xlib instead: an override-redirect window at
// real desktop coordinates, cut to the alpha of what we draw (so nothing
// undrawn covers the board) with an empty input region (so clicks pass by).
#include <SDL.h>

#include <string>

class XOverlay {
public:
    // Looks for an X display we can draw on; `reason` says why not.
    bool open(std::string* reason);
    bool isOpen() const { return dpy != nullptr; }

    // Creates the window on first use, then keeps it at `r` in desktop
    // coordinates, mapped and on top.
    bool place(const SDL_Rect& r);

    // Shows `src` (ARGB8888, exactly the window's size): its alpha becomes the
    // window's shape, and the pixels go straight onto it.
    void present(SDL_Surface* src);

    void hide();
    void close();

    const SDL_Rect& rect() const { return winRect; }
    const std::string& why() const { return note; }

private:
    void* dpy = nullptr;       // Display*
    void* visual = nullptr;    // Visual*
    unsigned long window = 0;  // Window
    unsigned long cmap = 0;    // Colormap, when the window is deeper than its parent
    void* gc = nullptr;        // GC
    SDL_Rect winRect{0, 0, 0, 0};
    int depth = 24;
    bool hasAlpha = false;
    bool mapped = false;
    bool openFailed = false;   // tried once and was refused: do not hammer X
    std::string note;
};

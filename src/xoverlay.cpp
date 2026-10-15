// xoverlay.cpp -- the shaped X11 window the best-move advice is drawn on
//
// The window is override-redirect (no decorations, no window manager to fight
// with), sits at real desktop coordinates, is cut out with the alpha of what we
// draw so the board underneath stays visible, and carries an empty input
// region so the player's clicks keep going to the board.
#include "xoverlay.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#endif

#ifdef HAVE_X11

namespace {

// Alpha over 8 belongs to the window; everything else is cut away. The image
// is a depth-1 pixmap, so the alpha channel of every pixel becomes one bit.
void fillMask(XImage* m, SDL_Surface* src) {
    const int w = m->width, h = m->height;
    const int bpl = m->bytes_per_line;
    const bool msb = (m->bitmap_bit_order == MSBFirst);
    for (int y = 0; y < h; ++y) {
        const uint32_t* px =
            (const uint32_t*)((const uint8_t*)src->pixels + (size_t)y * src->pitch);
        unsigned char* row = (unsigned char*)m->data + (size_t)y * bpl;
        for (int xb = 0; xb < bpl; ++xb) {
            unsigned char byte = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const int x = xb * 8 + bit;
                if (x >= w) break;
                if (((px[x] >> 24) & 255) <= 8) continue;
                if (msb)
                    byte |= (unsigned char)(0x80 >> bit);
                else
                    byte |= (unsigned char)(0x01 << bit);
            }
            row[xb] = byte;
        }
    }
}

// Said to the terminal once per distinct line: the overlay runs for hours, and
// a complaint repeated every frame is no complaint at all. Either thread's X
// calls can raise it, so the memory of what has already been said is guarded.
std::mutex gSaidMutex;

void once(const std::string& m) {
    static std::string said;
    const std::lock_guard<std::mutex> lk(gSaidMutex);
    if (m != said) {
        std::fprintf(stderr, "chess: overlay: %s\n", m.c_str());
        said = m;
    }
}

// Xlib's own error handler ends the process over the first protocol error, and
// this window is built out of exactly the calls servers like to refuse: one
// deeper than its parent, a shape of its own, an image at its own depth. Ours
// remembers the first refusal so the call can be skipped and reported instead
// of taking the app down with it -- and says what was refused, since that is
// the whole story when the overlay does not appear. Whichever thread the
// server answers on, the code lands here.
std::atomic<int> gXErr{0};

int quietError(Display*, XErrorEvent* e) {
    if (!gXErr) gXErr = e->error_code;
    once("X error " + std::to_string(e->error_code) + " from request " +
         std::to_string(e->request_code) + " (ignored)");
    return 0;
}

struct Quiet {
    Display* d;
    // The handler is installed and left up for good. Putting the previous one
    // back would be putting Xlib's fatal one back, and the capture thread runs
    // its own X calls on the same process: whoever restores last would hand the
    // gun to the other. Both sides simply install their own recorder before
    // their calls, so a refusal can never end the program.
    explicit Quiet(Display* disp) : d(disp) {
        XSetErrorHandler(quietError);
        gXErr = 0;
    }
    // Errors come back asynchronously, so nothing is known until the server
    // has been asked to catch up. 0 means the calls above went through.
    int sync() {
        if (d) XSync(d, False);
        return gXErr;
    }
    void reset() { gXErr = 0; }
    // After this the destructor only catches up on the display; it is no
    // longer ours (used once it has been closed).
    void detach() { d = nullptr; }
    ~Quiet() {
        if (d) XSync(d, False);
    }
};

}  // namespace

bool XOverlay::open(std::string* reason) {
    if (dpy) return true;
    if (openFailed) {  // already refused once: opening again would only churn X
        if (reason) *reason = note;
        return false;
    }
    Display* d = XOpenDisplay(nullptr);
    if (!d) {
        note = "no X display (on Wayland that means Xwayland is missing)";
        openFailed = true;
        if (reason) *reason = note;
        return false;
    }
    int ev = 0, err = 0;
    if (!XShapeQueryExtension(d, &ev, &err)) {
        note = "this X server has no shape extension";
        openFailed = true;
        XCloseDisplay(d);
        if (reason) *reason = note;
        return false;
    }
    XVisualInfo vinfo;
    if (XMatchVisualInfo(d, DefaultScreen(d), 32, TrueColor, &vinfo)) {
        visual = vinfo.visual;
        depth = 32;
        hasAlpha = true;
    } else {
        visual = DefaultVisual(d, DefaultScreen(d));
        depth = DefaultDepth(d, DefaultScreen(d));
        hasAlpha = false;
        note = "no 32-bit visual here, so the arrow is drawn solid";
    }
    dpy = d;
    return true;
}

bool XOverlay::place(const SDL_Rect& r) {
    if (!dpy || r.w <= 0 || r.h <= 0) return false;
    Display* d = (Display*)dpy;
    const int scr = DefaultScreen(d);
    if (!window) {
        const Window root = RootWindow(d, scr);
        XSetWindowAttributes wa;
        std::memset(&wa, 0, sizeof wa);
        wa.override_redirect = True;
        wa.background_pixel = 0;
        wa.border_pixel = 0;
        unsigned long mask = CWOverrideRedirect | CWBackPixel | CWBorderPixel;
        Quiet q(d);  // every X call from here down is watched, none may end us
        bool plain = false;  // the parent's own visual had to be used instead
        // A window deeper than its parent belongs to a visual of its own, and
        // the server wants that visual's colormap named alongside it: leaving
        // it out is a BadMatch, and the process would not have survived it.
        if (depth != DefaultDepth(d, scr)) {
            cmap = XCreateColormap(d, root, (Visual*)visual, AllocNone);
            if (cmap) {
                wa.colormap = cmap;
                mask |= CWColormap;
            }
        }
        Window made = XCreateWindow(d, root, r.x, r.y, (unsigned)r.w, (unsigned)r.h, 0, depth,
                                    InputOutput, (Visual*)visual, mask, &wa);
        if (q.sync()) {
            // Whatever the server objected to, the parent's own visual cannot
            // be among them. The shape still cuts the window to the pixels we
            // draw, so only per-pixel fading is given up.
            made = 0;
            if (cmap) {
                XFreeColormap(d, cmap);
                cmap = 0;
            }
            visual = DefaultVisual(d, scr);
            depth = DefaultDepth(d, scr);
            hasAlpha = false;
            mask = CWOverrideRedirect | CWBackPixel | CWBorderPixel;
            q.reset();
            made = XCreateWindow(d, root, r.x, r.y, (unsigned)r.w, (unsigned)r.h, 0, depth,
                                 InputOutput, (Visual*)visual, mask, &wa);
            if (q.sync())
                made = 0;
            else
                plain = true;
        }
        if (!made) {
            note = "the X server will not give us a window here";
            once(note);
            if (cmap) {
                XFreeColormap(d, cmap);
                cmap = 0;
            }
            return false;
        }
        window = made;
        gc = XCreateGC(d, window, 0, nullptr);
        // An empty input region: nothing that happens over the board reaches
        // this window, so the player's clicks carry on underneath it. (0 is
        // `Unsorted`, which is what any ordering means with no rectangles.)
        XShapeCombineRectangles(d, window, ShapeInput, 0, 0, nullptr, 0, ShapeSet, 0);
        winRect = r;
        XMapRaised(d, window);
        mapped = true;
        XFlush(d);
        if (q.sync()) {  // something after the window itself was refused
            note = "the X server will not show the overlay window here";
            once(note);
            XDestroyWindow(d, window);
            if (gc) {
                XFreeGC(d, (GC)gc);
                gc = nullptr;
            }
            if (cmap) {
                XFreeColormap(d, cmap);
                cmap = 0;
            }
            window = 0;
            mapped = false;
            return false;
        }
        // A window that ended up without alpha says so once, here: which of
        // the two reasons applied tells whether it is this machine or this
        // server that would not have it.
        if (hasAlpha)
            note.clear();
        else if (plain)
            note = "this X server refuses a 32-bit window, so the arrow is drawn solid";
        else
            note = "no 32-bit window here, so the arrow is drawn solid";
        // And the one thing worth knowing when it all worked: that the window
        // exists, where it is, and which kind of window it turned out to be.
        once("window " + std::to_string(r.w) + "x" + std::to_string(r.h) + " at " +
             std::to_string(r.x) + "," + std::to_string(r.y) + " " +
             (hasAlpha ? "32-bit" : "solid") + " over X");
        return true;
    }
    {
        Quiet q(d);  // a refused move or map must not end the process
        if (r.w != winRect.w || r.h != winRect.h)
            XResizeWindow(d, window, (unsigned)r.w, (unsigned)r.h);
        if (r.x != winRect.x || r.y != winRect.y) XMoveWindow(d, window, r.x, r.y);
        winRect = r;
        if (!mapped) {
            XMapRaised(d, window);
            mapped = true;
        }
        XRaiseWindow(d, window);
        XFlush(d);
        if (q.sync()) {
            note = "the X server will not place the overlay window here";
            once(note);
        } else if (note == "the X server will not place the overlay window here") {
            note.clear();  // it was refused once and has been fine ever since
        }
    }
    return true;
}

void XOverlay::present(SDL_Surface* src) {
    if (!dpy || !window || !src || !mapped) return;
    if (src->w != winRect.w || src->h != winRect.h) return;
    if (src->format->format != SDL_PIXELFORMAT_ARGB8888) return;

    Display* d = (Display*)dpy;
    Quiet q(d);  // a refused image or shape is a skipped frame, not a crash
    const int w = src->w, h = src->h;
    if (SDL_MUSTLOCK(src)) SDL_LockSurface(src);

    // Shape first: the window is cut to exactly the pixels we are about to
    // show, so no invisible box is left lying over the board.
    XImage* mi = XCreateImage(d, (Visual*)visual, 1, ZPixmap, 0, nullptr, w, h, 32, 0);
    if (mi) {
        if (mi->data) {
            XFree(mi->data);
            mi->data = nullptr;
        }
        mi->data = (char*)std::calloc(1, (size_t)mi->bytes_per_line * h);
        char* md = mi->data;
        if (md) {
            fillMask(mi, src);
            const Pixmap pix = XCreatePixmap(d, window, w, h, 1);
            GC mgc = XCreateGC(d, pix, 0, nullptr);
            XPutImage(d, pix, mgc, mi, 0, 0, 0, 0, w, h);
            XShapeCombineMask(d, window, ShapeBounding, 0, 0, pix, ShapeSet);
            XFreeGC(d, mgc);
            XFreePixmap(d, pix);
        }
        mi->data = nullptr;
        XDestroyImage(mi);
        std::free(md);
    }

    // Then the pixels themselves.
    XImage* xi = XCreateImage(d, (Visual*)visual, depth, ZPixmap, 0, nullptr, w, h, 32, 0);
    if (xi) {
        if (xi->data) {
            XFree(xi->data);
            xi->data = nullptr;
        }
        if (xi->bits_per_pixel == 32) {
            std::vector<uint8_t> buf((size_t)w * h * 4);
            const bool msb = (xi->byte_order == MSBFirst);
            for (int y = 0; y < h; ++y) {
                const uint8_t* s = (const uint8_t*)src->pixels + (size_t)y * src->pitch;
                uint8_t* o = buf.data() + (size_t)y * w * 4;
                if (!msb) {
                    std::memcpy(o, s, (size_t)w * 4);
                } else {
                    // Byte-reverse into the server's order; a 24-bit window
                    // has no alpha channel to write into.
                    for (int x = 0; x < w; ++x) {
                        const uint32_t v = ((const uint32_t*)s)[x];
                        const uint32_t p = (depth == 24) ? (v & 0x00FFFFFFu) : v;
                        o[0] = (uint8_t)(p >> 24);
                        o[1] = (uint8_t)(p >> 16);
                        o[2] = (uint8_t)(p >> 8);
                        o[3] = (uint8_t)p;
                    }
                }
            }
            xi->data = (char*)buf.data();
            xi->bytes_per_line = w * 4;
            XPutImage(d, window, (GC)gc, xi, 0, 0, 0, 0, w, h);
            xi->data = nullptr;
            if (hasAlpha) note.clear();
        } else {
            note = "this X server does not use 32-bit pixels";
        }
        XDestroyImage(xi);
    }

    if (SDL_MUSTLOCK(src)) SDL_UnlockSurface(src);
    XRaiseWindow(d, window);
    XFlush(d);
    if (q.sync()) {
        note = "the X server refused the overlay frame";
        once(note);
    } else if (note == "the X server refused the overlay frame") {
        note.clear();
    }
}

void XOverlay::hide() {
    if (!dpy || !window || !mapped) return;
    Display* d = (Display*)dpy;
    Quiet q(d);
    XUnmapWindow(d, window);
    XFlush(d);
    q.sync();
    mapped = false;
}

void XOverlay::close() {
    if (!dpy) return;
    Display* d = (Display*)dpy;
    {
        Quiet q(d);
        if (window) {
            XDestroyWindow(d, window);
            window = 0;
        }
        if (gc) {
            XFreeGC(d, (GC)gc);
            gc = nullptr;
        }
        if (cmap) {
            XFreeColormap(d, cmap);
            cmap = 0;
        }
        q.sync();    // everything is settled while the display is still ours
        XCloseDisplay(d);  // closed under the quiet handler, not the default one
        q.detach();  // the display is gone: the destructor must not touch it
    }
    dpy = nullptr;
    visual = nullptr;
    mapped = false;
    winRect = SDL_Rect{0, 0, 0, 0};
}

#else  // !HAVE_X11

bool XOverlay::open(std::string* reason) {
    note = "this build has no X11 support";
    if (reason) *reason = note;
    return false;
}
bool XOverlay::place(const SDL_Rect&) { return false; }
void XOverlay::present(SDL_Surface*) {}
void XOverlay::hide() {}
void XOverlay::close() {}

#endif  // HAVE_X11

// screen.cpp -- capture the desktop, find the chess board on it, read its pieces
//
// Three steps, each usable on its own (the tests drive the last two with
// rendered boards):
//   captureScreen()  a frame of the desktop (X11 root window, or a screenshot
//                    tool where the root window cannot be read, as under
//                    XWayland)
//   findBoard()      the 8x8 two-colour grid anywhere on that frame
//   readBoard()      the 64 squares turned back into a FEN
#include "screen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pieces.h"

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

namespace {

// ------------------------------------------------------------- colour -----

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline int dist2i(const int a[3], const int b[3]) {
    const int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    return dr * dr + dg * dg + db * db;
}

// A pixel off the screen against the colour its square is supposed to be: what
// turns a square into a mask of the piece standing on it.
inline int dist2p(const uint8_t* a, const int* b) {
    const int dr = (int)a[0] - b[0], dg = (int)a[1] - b[1], db = (int)a[2] - b[2];
    return dr * dr + dg * dg + db * db;
}

// Average colour of a k x k box centred on (cx, cy), clamped to the image.
void sampleBox(const ScreenImage& im, int cx, int cy, int k, int out[3]) {
    const int half = k / 2;
    const int x0 = clampi(cx - half, 0, im.w - 1);
    const int x1 = clampi(cx + half, 0, im.w - 1);
    const int y0 = clampi(cy - half, 0, im.h - 1);
    const int y1 = clampi(cy + half, 0, im.h - 1);
    int r = 0, g = 0, b = 0, n = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const uint8_t* p = im.px(x, y);
            r += p[0];
            g += p[1];
            b += p[2];
            ++n;
        }
    out[0] = n ? r / n : 0;
    out[1] = n ? g / n : 0;
    out[2] = n ? b / n : 0;
}

// Median of several samples, per channel (the average of the middle two):
// corner glyphs and pieces that reach the edge can take any couple of the
// samples and still leave the middle of the sorted values where the square's
// own colour sits. Six points buy tolerance for two at once -- a rank label
// and a file letter, or a wide base and a glyph on top of it.
void medianN(const int s[][3], int n, int out[3]) {
    int v[8];
    if (n < 2 || n > 8) return;
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < n; ++i) v[i] = s[i][c];
        std::sort(v, v + n);
        out[c] = (v[n / 2 - 1] + v[n / 2]) / 2;
    }
}

// ------------------------------------------------------------- capture ----

struct X11State {
    Display* dpy = nullptr;
    bool failed = false;
    std::string err;
};

X11State& x11state() {
    static X11State s;
    return s;
}

struct CapState {
    int kind = 0;  // 0 = undecided, 1 = x11, 2 = tool, 3 = no x11
    int tool = -1;
    std::string name = "auto";
};

CapState& capState() {
    static CapState s;
    return s;
}

// Where the frames are parked between captures.
std::string tempPath(const char* suffix) {
    const char* t = std::getenv("TMPDIR");
    const std::string dir = (t && *t) ? t : "/tmp";
    return dir + "/chess_cap_" + std::to_string((long)getpid()) + suffix;
}

std::string which(const std::string& exe) {
    const char* env = std::getenv("PATH");
    if (!env) return "";
    std::string paths(env);
    size_t i = 0;
    for (;;) {
        size_t j = paths.find(':', i);
        if (j == std::string::npos) j = paths.size();
        std::string dir = paths.substr(i, j - i);
        if (dir.empty()) dir = ".";
        const std::string full = dir + "/" + exe;
        if (::access(full.c_str(), X_OK) == 0) return full;
        if (j == paths.size()) break;
        i = j + 1;
    }
    return "";
}

// What a tool had to say about itself, trimmed to what a status line can hold.
// A screenshot tool that decides not to write anything gives no other clue.
std::string logLine(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    char buf[256];
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    std::string s;
    for (size_t i = 0; i < n && s.size() < 150; ++i) {
        if (buf[i] == '\n' || buf[i] == '\r') break;
        s += buf[i];
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

// A BMP header alone is 54 bytes: below that the file is still being written
// or never will be. -1 means it is not there at all.
long fileSize(const std::string& path) {
    struct stat sb;
    if (::stat(path.c_str(), &sb) != 0) return -1;
    return (long)sb.st_size;
}

// The message the status line shows: who tried, what went wrong, and the
// tool's own words if it had any.
std::string blame(const char* name, const std::string& what, const std::string& log) {
    const std::string said = logLine(log);
    if (said.empty()) return std::string(name) + ": " + what;
    return std::string(name) + ": " + what + " (" + said + ")";
}

// Runs a screenshot tool with a deadline: a wedged tool must not pin the
// overlay worker, nor hold the app open at exit. With `logPath` the tool's
// output is kept instead of thrown away with /dev/null.
bool runTool(const std::vector<std::string>& argv, int timeoutMs,
             const char* logPath = nullptr) {
    if (argv.empty()) return false;
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int sink = -1;
        if (logPath && *logPath) sink = ::open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (sink < 0) sink = ::open("/dev/null", O_WRONLY);
        if (sink >= 0) {
            ::dup2(sink, 1);
            ::dup2(sink, 2);
            ::close(sink);
        }
        char* args[16];
        int n = 0;
        for (const std::string& s : argv)
            if (n < 15) args[n++] = const_cast<char*>(s.c_str());
        args[n] = nullptr;
        ::execvp(args[0], args);
        ::_exit(127);
    }
    int status = 0;
    for (int waited = 0;; waited += 20) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (r < 0) return false;
        if (waited >= timeoutMs) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return false;
        }
        ::usleep(20000);
    }
}

struct ToolCmd {
    const char* name;
    const char* args;  // %o is replaced by the output path
};

// Tried in order when the root window cannot be read (XWayland): each writes a
// BMP, which is cheap to decode and cheap to write at this size.
const ToolCmd CAP_TOOLS[] = {
    {"spectacle", "-b -n -f -o %o"},
    {"gnome-screenshot", "-f %o"},
    {"import", "-window root %o"},
};
const int CAP_NTOOLS = 3;

uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t rd16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

// 24/32-bit uncompressed BMP, bottom-up or top-down.
bool loadBmp(const std::string& path, ScreenImage& out, std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return fail("screenshot file is missing");
    uint8_t head[54];
    if (std::fread(head, 1, sizeof head, f) != sizeof head) {
        std::fclose(f);
        return fail("truncated screenshot");
    }
    if (head[0] != 'B' || head[1] != 'M') {
        std::fclose(f);
        return fail("screenshot is not a BMP file");
    }
    const uint32_t offset = rd32(head + 10);
    int32_t w = (int32_t)rd32(head + 18);
    int32_t h = (int32_t)rd32(head + 22);
    const uint16_t bpp = rd16(head + 28);
    const uint32_t comp = rd32(head + 30);
    const bool topDown = h < 0;
    if (topDown) h = -h;
    if (w <= 0 || h <= 0 || (bpp != 24 && bpp != 32) || comp != 0) {
        std::fclose(f);
        return fail("unsupported screenshot (need an uncompressed BMP)");
    }
    const size_t bytes = bpp / 8;
    const size_t rowBytes = ((size_t)w * bytes + 3) & ~(size_t)3;
    std::vector<uint8_t> row(rowBytes);
    out.reset(w, h);
    if (std::fseek(f, (long)offset, SEEK_SET) != 0) {
        std::fclose(f);
        return fail("bad screenshot header");
    }
    for (int i = 0; i < h; ++i) {
        if (std::fread(row.data(), 1, rowBytes, f) != rowBytes) {
            std::fclose(f);
            return fail("truncated screenshot");
        }
        const int y = topDown ? i : (h - 1 - i);
        uint8_t* dst = out.px(0, y);
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row.data() + (size_t)x * bytes;
            dst[0] = p[2];
            dst[1] = p[1];
            dst[2] = p[0];
            dst += 3;
        }
    }
    std::fclose(f);
    return true;
}

// The other direction: a 24-bit BMP, bottom-up, rows padded to four bytes --
// the same shape loadBmp reads, so what the eye was given can be looked at.
bool saveBmp(const ScreenImage& im, const std::string& path) {
    if (im.empty()) return false;
    const int w = im.w, h = im.h;
    const size_t rowBytes = ((size_t)w * 3 + 3) & ~(size_t)3;
    uint8_t head[54] = {};
    head[0] = 'B';
    head[1] = 'M';
    auto w16 = [&](int at, unsigned v) {
        head[at] = (uint8_t)(v & 255);
        head[at + 1] = (uint8_t)((v >> 8) & 255);
    };
    auto w32 = [&](int at, unsigned long v) {
        for (int k = 0; k < 4; ++k) head[at + k] = (uint8_t)((v >> (8 * k)) & 255);
    };
    w32(2, 54UL + rowBytes * (size_t)h);  // whole file
    w32(10, 54);                          // where the pixels start
    w32(14, 40);                          // DIB header size
    w32(18, (unsigned long)w);
    w32(22, (unsigned long)h);  // positive: the first row in the file is the last on screen
    w16(26, 1);                 // planes
    w16(28, 24);                // bits per pixel
    w32(34, rowBytes * (size_t)h);
    w32(38, 2835);  // pixels per metre, so viewers get the aspect right
    w32(42, 2835);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(head, 1, sizeof head, f) == sizeof head;
    std::vector<uint8_t> row(rowBytes, 0);
    for (int y = h - 1; ok && y >= 0; --y) {
        const uint8_t* src = im.px(0, y);
        for (int x = 0; x < w; ++x) {
            row[(size_t)x * 3 + 0] = src[(size_t)x * 3 + 2];  // the file wants BGR
            row[(size_t)x * 3 + 1] = src[(size_t)x * 3 + 1];
            row[(size_t)x * 3 + 2] = src[(size_t)x * 3 + 0];
        }
        ok = std::fwrite(row.data(), 1, rowBytes, f) == rowBytes;
    }
    std::fclose(f);
    return ok;
}

// And a PNG, so a frame can be looked at rather than parsed: a BMP opens in a
// viewer but not in every tool worth arguing with, and this one is written out
// by hand because the library that does it properly is not linked. The part of
// the format that could go wrong -- the compression -- is the part this does
// not do: the zlib stream inside is a run of stored blocks, the bytes exactly
// as they are, with the two header bytes and the Adler-32 checksum the format
// asks for around them. Chunks are signed with a CRC-32, which is the only
// table worth having.
static const std::array<uint32_t, 256> PNG_CRC = [] {
    std::array<uint32_t, 256> v{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        v[i] = c;
    }
    return v;
}();

static uint32_t pngCrc(uint32_t crc, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) crc = PNG_CRC[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

bool savePng(const ScreenImage& im, const std::string& path) {
    if (im.empty()) return false;
    const int w = im.w, h = im.h;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto be32 = [](uint8_t* d, uint32_t v) {
        d[0] = (uint8_t)(v >> 24);
        d[1] = (uint8_t)(v >> 16);
        d[2] = (uint8_t)(v >> 8);
        d[3] = (uint8_t)v;
    };
    bool ok = true;
    // One chunk: the payload's length, its four letter type, the payload, and
    // a checksum over type and payload together.
    auto chunk = [&](const char* type, const uint8_t* data, size_t n) {
        if (!ok) return;
        uint8_t head[8];
        be32(head, (uint32_t)n);
        std::memcpy(head + 4, type, 4);
        uint32_t crc = pngCrc(0xFFFFFFFFu, head + 4, 4);
        if (n > 0) crc = pngCrc(crc, data, n);
        uint8_t tail[4];
        be32(tail, crc ^ 0xFFFFFFFFu);
        ok = std::fwrite(head, 1, sizeof head, f) == sizeof head &&
             (n == 0 || std::fwrite(data, 1, n, f) == n) &&
             std::fwrite(tail, 1, sizeof tail, f) == sizeof tail;
    };

    // The pixels as scanlines: one filter byte (none) and then the row.
    const size_t rowLen = 1 + (size_t)w * 3;
    std::vector<uint8_t> raw(rowLen * (size_t)h);
    for (int y = 0; y < h; ++y) {
        uint8_t* d = &raw[(size_t)y * rowLen];
        d[0] = 0;
        std::memcpy(d + 1, im.px(0, y), (size_t)w * 3);
    }

    // The same bytes wrapped for zlib: two header bytes, then blocks of at
    // most 65535 -- a flag byte, the length, its complement, the data -- and
    // the Adler-32 of the whole run at the end.
    std::vector<uint8_t> z;
    z.reserve(raw.size() + 64 + raw.size() / 65535 * 5);
    z.push_back(0x78);
    z.push_back(0x01);
    for (size_t off = 0; off < raw.size(); off += 65535) {
        const size_t n = std::min((size_t)65535, raw.size() - off);
        z.push_back((uint8_t)(off + n >= raw.size() ? 1 : 0));
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)((n >> 8) & 0xFF));
        const uint16_t inv = (uint16_t)~(uint16_t)n;
        z.push_back((uint8_t)(inv & 0xFF));
        z.push_back((uint8_t)((inv >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    // The Adler-32 the format wants: two running sums, reduced only when they
    // have grown to the point where 5552 more bytes could not carry either
    // past 2^32 -- one division per block of the row instead of per byte.
    uint32_t a = 1, b = 0;
    size_t at = 0;
    while (at < raw.size()) {
        const size_t end = std::min(at + 5552, raw.size());
        for (; at < end; ++at) {
            a += raw[at];
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    uint8_t adler[4];
    be32(adler, (b << 16) | a);
    z.insert(z.end(), adler, adler + 4);

    uint8_t sig[8];
    sig[0] = 0x89;
    sig[1] = 'P';
    sig[2] = 'N';
    sig[3] = 'G';
    sig[4] = 13;
    sig[5] = 10;
    sig[6] = 26;
    sig[7] = 10;
    if (std::fwrite(sig, 1, sizeof sig, f) != sizeof sig) ok = false;
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;   // bits per channel
    ihdr[9] = 2;   // colour type: truecolour
    ihdr[10] = 0;  // deflate
    ihdr[11] = 0;  // filtering
    ihdr[12] = 0;  // one pass
    chunk("IHDR", ihdr, sizeof ihdr);
    chunk("IDAT", z.data(), z.size());
    chunk("IEND", nullptr, 0);
    std::fclose(f);
    return ok;
}

#ifdef HAVE_X11

int gXErr = 0;

int xTrap(Display*, XErrorEvent* e) {
    gXErr = e->error_code;
    return 0;
}

// Where the red, green and blue bits of a pixel sit: everything about reading
// the root window depends on it.
void maskInfo(unsigned long mask, int& shift, int& bits) {
    shift = 0;
    bits = 0;
    if (!mask) return;
    while (!((mask >> shift) & 1uL)) ++shift;
    unsigned long m = mask >> shift;
    while (m & 1uL) {
        ++bits;
        m >>= 1;
    }
}

inline uint8_t chanOf(unsigned long v, unsigned long mask, int shift, int bits) {
    const unsigned long x = (v & mask) >> shift;
    if (bits >= 8) return (uint8_t)(x >> (bits - 8));
    if (bits == 0) return 0;
    return (uint8_t)((x * 255) / ((1u << bits) - 1));
}

bool ximageToRgb(XImage* im, ScreenImage& out) {
    if (!im || !im->data || im->width <= 0 || im->height <= 0) return false;
    out.reset(im->width, im->height);
    int rs, rb, gs, gb, bs, bb;
    maskInfo(im->red_mask, rs, rb);
    maskInfo(im->green_mask, gs, gb);
    maskInfo(im->blue_mask, bs, bb);
    const int bpp = im->bits_per_pixel;
    const bool msb = (im->byte_order == MSBFirst);
    const bool fast = (bpp == 32 || bpp == 24);
    for (int y = 0; y < im->height; ++y) {
        const uint8_t* row = (const uint8_t*)im->data + (size_t)y * im->bytes_per_line;
        uint8_t* dst = out.px(0, y);
        for (int x = 0; x < im->width; ++x) {
            unsigned long v;
            if (!fast) {
                v = XGetPixel(im, x, y);
            } else {
                const uint8_t* p = row + (size_t)x * (bpp / 8);
                if (bpp == 32) {
                    v = msb ? (((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
                               ((unsigned long)p[2] << 8) | (unsigned long)p[3])
                            : (((unsigned long)p[3] << 24) | ((unsigned long)p[2] << 16) |
                               ((unsigned long)p[1] << 8) | (unsigned long)p[0]);
                } else {
                    v = msb ? (((unsigned long)p[0] << 16) | ((unsigned long)p[1] << 8) |
                               (unsigned long)p[2])
                            : (((unsigned long)p[2] << 16) | ((unsigned long)p[1] << 8) |
                               (unsigned long)p[0]);
                }
            }
            dst[0] = chanOf(v, im->red_mask, rs, rb);
            dst[1] = chanOf(v, im->green_mask, gs, gb);
            dst[2] = chanOf(v, im->blue_mask, bs, bb);
            dst += 3;
        }
    }
    return true;
}

// X errors arrive asynchronously, so every call that can fail is wrapped in a
// trap: a root window that cannot be read (XWayland) reports BadMatch rather
// than ending the process.
bool captureX11(ScreenImage& out, std::string* err) {
    X11State& st = x11state();
    if (st.failed) {
        if (err) *err = st.err;
        return false;
    }
    if (!st.dpy) {
        st.dpy = XOpenDisplay(NULL);
        if (!st.dpy) {
            st.failed = true;
            st.err = "cannot open the X display";
            if (err) *err = st.err;
            return false;
        }
    }
    const Window root = DefaultRootWindow(st.dpy);
    XWindowAttributes attrs;
    attrs.width = attrs.height = 0;
    // The trap stays up once raised: putting Xlib's default handler back would
    // hand a fatal one to the overlay window, whose own X calls run on another
    // thread and install their own recorder over this one.
    XSetErrorHandler(xTrap);
    gXErr = 0;
    XGetWindowAttributes(st.dpy, root, &attrs);
    XImage* im = nullptr;
    if (!gXErr && attrs.width > 0 && attrs.height > 0)
        im = XGetImage(st.dpy, root, 0, 0, (unsigned)attrs.width, (unsigned)attrs.height, AllPlanes,
                       ZPixmap);
    XSync(st.dpy, False);
    if (im && !gXErr && ximageToRgb(im, out)) {
        XDestroyImage(im);
        return true;
    }
    if (im) XDestroyImage(im);
    st.failed = true;
    st.err = "the X root window cannot be read";
    XCloseDisplay(st.dpy);
    st.dpy = nullptr;
    if (err) *err = st.err;
    return false;
}

#endif  // HAVE_X11

}  // namespace

void x11InitThreads() {
#ifdef HAVE_X11
    XInitThreads();
#endif
}

const char* captureBackend() { return capState().name.c_str(); }

std::string captureProblem() {
    if (capState().kind == 1 || capState().kind == 2) return "";
#ifdef HAVE_X11
    // Before the first frame a display implies a capture path; once X11 has
    // been tried and refused (Xwayland), only a screenshot tool can save us.
    if (capState().kind != 3) {
        const char* disp = std::getenv("DISPLAY");
        if (disp && *disp) return "";
    }
#endif
    for (int i = 0; i < CAP_NTOOLS; ++i)
        if (!which(CAP_TOOLS[i].name).empty()) return "";
    return "no X display and no screenshot tool, so the screen cannot be captured";
}

// The frame the watcher is working from, written out so the log can be checked
// against the picture it came from. The format follows the name: a .png for
// anything that wants to look at it, and the BMP SDL writes without help for
// everything else.
bool saveScreenImage(const ScreenImage& im, const char* path) {
    if (path == nullptr || path[0] == '\0') return false;
    const std::string p(path);
    const bool png = p.size() >= 4 && p.compare(p.size() - 4, 4, ".png") == 0;
    return png ? savePng(im, p) : saveBmp(im, p);
}

bool captureScreen(ScreenImage& out, std::string* err) {
    CapState& st = capState();
#ifdef HAVE_X11
    // A Wayland session's X11 root window reads just fine and still shows
    // only Xwayland's own clients: the window being watched is a native
    // Wayland one and never appears in it, so letting "x11 works" win here
    // would mean catching the screen half-blind. The session decides -- the
    // screenshot tool sees all of it, and on Wayland it goes first.
    static const bool wayland = [] {
        const char* d = std::getenv("WAYLAND_DISPLAY");
        const char* t = std::getenv("XDG_SESSION_TYPE");
        return (d && *d) || (t && std::strcmp(t, "wayland") == 0);
    }();
    if (wayland) st.kind = 3;
    if (st.kind == 0 || st.kind == 1) {
        std::string e;
        if (captureX11(out, &e)) {
            st.kind = 1;
            st.name = "x11";
            return true;
        }
        st.kind = 3;  // the root window is unreadable: never try it twice
    }
#else
    st.kind = 3;
#endif

    if (st.tool < 0)
        for (int i = 0; i < CAP_NTOOLS; ++i)
            if (!which(CAP_TOOLS[i].name).empty()) {
                st.tool = i;
                st.name = CAP_TOOLS[i].name;
                break;
            }
    if (st.tool < 0) {
        if (err) *err = "no screenshot tool (install spectacle or gnome-screenshot)";
        return false;
    }

    const ToolCmd& tool = CAP_TOOLS[st.tool];
    const std::string exe = which(tool.name);
    if (exe.empty()) {
        if (err) *err = std::string(tool.name) + " is gone from $PATH";
        st.tool = -1;
        return false;
    }
    const std::string path = tempPath(".bmp");
    const std::string log = tempPath(".log");
    ::unlink(path.c_str());
    ::unlink(log.c_str());
    std::vector<std::string> argv;
    argv.push_back(exe);
    {
        const std::string args = tool.args;
        size_t i = 0;
        for (;;) {
            size_t j = args.find(' ', i);
            if (j == std::string::npos) j = args.size();
            const std::string tok = args.substr(i, j - i);
            if (!tok.empty()) argv.push_back(tok == "%o" ? path : tok);
            if (j == args.size()) break;
            i = j + 1;
        }
    }

    // Whatever goes wrong here, the terminal the app was started from hears
    // about it -- once per distinct complaint, not once per frame.
    auto fail = [&](const std::string& m) -> bool {
        if (err) *err = m;
        static std::string said;
        if (m != said) {
            std::fprintf(stderr, "chess: capture: %s\n", m.c_str());
            said = m;
        }
        ::unlink(path.c_str());
        ::unlink(log.c_str());
        return false;
    };

    if (!runTool(argv, 15000, log.c_str()))
        return fail(blame(tool.name, "failed to take a screenshot", log));

    // A tool that hands the shot to an instance already running returns before
    // that instance has written anything. Give the file a moment to arrive and
    // to stop growing before calling it missing.
    for (int w = 0; w < 50 && fileSize(path) < 54; ++w) ::usleep(20000);

    std::string e;
    if (!loadBmp(path, out, &e)) {
        // A file that exists but is not a BMP: some tools ignore the extension
        // and write a PNG instead, and ImageMagick is the one converter we can
        // count on being installed. With no file at all there is nothing to
        // convert -- and the tool's own words say more than it would.
        const bool haveFile = fileSize(path) >= 0;
        std::string magick = haveFile ? which("magick") : std::string();
        if (magick.empty() && haveFile) magick = which("convert");
        const std::string conv = tempPath("_c.bmp");
        if (magick.empty() || !runTool({magick, path, conv}, 15000) ||
            !loadBmp(conv, out, &e))
            return fail(blame(tool.name, e, log));
        ::unlink(conv.c_str());
    }
    ::unlink(path.c_str());
    ::unlink(log.c_str());
    st.kind = 2;
    return true;
}

// -------------------------------------------------------------- marker ----

bool findSelfWindow(const ScreenImage& img, const SDL_Rect& marker, int winW, int winH,
                    SDL_Rect& out) {
    // Four colour blocks drawn in our own window's bottom margin while the
    // overlay runs: window positions are guesses under Wayland, this is not.
    static const uint8_t COLS[4][3] = {{255, 0, 128}, {0, 255, 64}, {64, 128, 255}, {255, 255, 0}};
    if (img.empty() || marker.w < 8 || marker.h < 2) return false;
    const int bw = marker.w / 4;
    if (bw < 2) return false;
    for (int y = 0; y + marker.h <= img.h; ++y) {
        const uint8_t* row = img.px(0, y);
        for (int x = 0; x + marker.w <= img.w; ++x) {
            const uint8_t* p = row + 3 * (size_t)x;
            // A screenshot can carry the colours a shade off, so the first
            // block is compared with the same slack as the rest.
            if (std::abs((int)p[0] - (int)COLS[0][0]) > 6 ||
                std::abs((int)p[1] - (int)COLS[0][1]) > 6 ||
                std::abs((int)p[2] - (int)COLS[0][2]) > 6)
                continue;
            bool hit = true;
            for (int b = 0; b < 4 && hit; ++b) {
                const uint8_t* q = img.px(x + b * bw + bw / 2, y + marker.h / 2);
                if (std::abs((int)q[0] - (int)COLS[b][0]) > 6 ||
                    std::abs((int)q[1] - (int)COLS[b][1]) > 6 ||
                    std::abs((int)q[2] - (int)COLS[b][2]) > 6)
                    hit = false;
            }
            if (!hit) continue;
            out.x = x - marker.x;
            out.y = y - marker.y;
            out.w = winW;
            out.h = winH;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------ detection ----

namespace {

struct Cand {
    float sc;
    int x, y, s;
};

inline bool overSkip(const std::vector<SDL_Rect>& skip, int px, int py) {
    for (const SDL_Rect& r : skip)
        if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) return true;
    return false;
}

// One sample per square -- cheap enough to try every scale and phase over the
// whole screen. Squares behind our own window are left out of the counts
// entirely rather than dragging them toward the window's colour.
float coarseScore(const ScreenImage& im, int x, int y, int s, int stride,
                  const std::vector<SDL_Rect>& skip) {
    int smp[64][3];
    bool have[64];
    int cnt[2] = {0, 0};
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) {
            const int px = clampi((x + (c * 2 + 1) * s / 2) * stride, 0, im.w - 1);
            const int py = clampi((y + (r * 2 + 1) * s / 2) * stride, 0, im.h - 1);
            const int i = r * 8 + c;
            if (!skip.empty() && overSkip(skip, px, py)) {
                have[i] = false;
                continue;
            }
            const uint8_t* q = im.px(px, py);
            smp[i][0] = q[0];
            smp[i][1] = q[1];
            smp[i][2] = q[2];
            have[i] = true;
            ++cnt[((i >> 3) + (i & 7)) & 1];
        }
    if (cnt[0] < 8 || cnt[1] < 8) return 0.f;

    // Most of the screen is not two colours at all -- a gradient, a photograph,
    // a page of text -- and the mean of each half says so in a single pass, so
    // the medians below (six small sorts) are only paid for what survives it.
    double sum[2][3] = {{0, 0, 0}, {0, 0, 0}};
    for (int i = 0; i < 64; ++i) {
        if (!have[i]) continue;
        const int g = ((i >> 3) + (i & 7)) & 1;
        sum[g][0] += smp[i][0];
        sum[g][1] += smp[i][1];
        sum[g][2] += smp[i][2];
    }
    int mean[2][3];
    for (int g = 0; g < 2; ++g)
        for (int c = 0; c < 3; ++c) mean[g][c] = (int)(sum[g][c] / cnt[g]);
    // 25 is under half of what the finest board's two colours are apart (55,
    // the cut-off further down), so no real board is lost here even when a
    // piece sits on half its squares and pulls both halves of the mean.
    if (std::sqrt((double)dist2i(mean[0], mean[1])) < 25.0) return 0.f;

    // The median of each half, not its mean. These samples sit at the centre
    // of each square, which is where a piece stands: half the squares on a
    // real board put a white or a black piece into the count, and a mean of
    // those three crowds lands nowhere near either square colour -- where
    // nothing on the board is an inlier any more and the board is scored as
    // background. The median of a third pieces, two thirds squares is square.
    int med[2][3];
    for (int g = 0; g < 2; ++g)
        for (int c = 0; c < 3; ++c) {
            int v[64], k = 0;
            for (int i = 0; i < 64; ++i)
                if (have[i] && (((i >> 3) + (i & 7)) & 1) == g) v[k++] = smp[i][c];
            std::sort(v, v + k);
            med[g][c] = k ? v[k / 2] : 0;
        }

    double var = 0.0;
    int n = 0;
    for (int i = 0; i < 64; ++i) {
        if (!have[i]) continue;
        const int g = ((i >> 3) + (i & 7)) & 1;
        const int d2 = dist2i(smp[i], med[g]);
        if (d2 <= 100 * 100) {
            var += d2;
            ++n;
        }
    }
    const int vis = cnt[0] + cnt[1];
    if (n < 8) return 0.f;
    const double rms = std::sqrt(var / n);
    const double dist = std::sqrt((double)dist2i(med[0], med[1]));
    if (dist < 45.0) return 0.f;
    // Samples that fit neither colour are how a grid that wanders off the
    // board into the background shows itself: it is two colours there, but
    // not these two.
    return (float)(dist / (dist + rms + 1.0) * ((double)n / vis));
}

// Four corner samples per square give that square's colour; the board's two
// colours are then the median of each half. Squares behind `skip` are counted
// rather than read: a board we can only half see is worse than no board.
float evalGrid(const ScreenImage& im, int x, int y, int cell, const std::vector<SDL_Rect>& skip,
               int colA[3], int colB[3], int* covered, int minPer = 24) {
    if (covered) *covered = 0;
    if (cell < 16) return 0.f;
    if (x < 0 || y < 0 || x + 8 * cell > im.w || y + 8 * cell > im.h) return 0.f;

    // Sample close to the square's edge: a grid even a couple of pixels off
    // then samples across the colour boundary and scores badly, which is what
    // lets the walk below settle on the exact alignment.
    const int q = std::max(2, std::min(cell * 45 / 100, cell / 2 - 2));
    int est[64][3];
    bool have[64];
    int cov = 0;
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) {
            const int i = r * 8 + c;
            const int cx = x + (c * 2 + 1) * cell / 2;
            const int cy = y + (r * 2 + 1) * cell / 2;
            if (!skip.empty() && overSkip(skip, cx, cy)) {
                have[i] = false;
                ++cov;
                continue;
            }
            const int off[4][2] = {{-q, -q}, {q, -q}, {-q, q}, {q, q}};
            int s[4][3];
            for (int k = 0; k < 4; ++k) sampleBox(im, cx + off[k][0], cy + off[k][1], 3, s[k]);
            medianN(s, 4, est[i]);
            have[i] = true;
        }
    if (covered) *covered = cov;

    int vals[2][3][64];
    int cnt[2] = {0, 0};
    for (int i = 0; i < 64; ++i) {
        if (!have[i]) continue;
        const int g = ((i >> 3) + (i & 7)) & 1;
        const int k = cnt[g]++;
        vals[g][0][k] = est[i][0];
        vals[g][1][k] = est[i][1];
        vals[g][2][k] = est[i][2];
    }
    if (cnt[0] < minPer || cnt[1] < minPer) return 0.f;
    for (int g = 0; g < 2; ++g)
        for (int c = 0; c < 3; ++c) {
            int* v = vals[g][c];
            std::sort(v, v + cnt[g]);
            (g ? colB : colA)[c] = v[cnt[g] / 2];
        }

    double var = 0.0;
    int n = 0, vis = 0;
    for (int i = 0; i < 64; ++i) {
        if (!have[i]) continue;
        ++vis;
        const int g = ((i >> 3) + (i & 7)) & 1;
        const int d2 = dist2i(est[i], g ? colB : colA);
        if (d2 <= 100 * 100) {
            var += d2;
            ++n;
        }
    }
    const double rms = n ? std::sqrt(var / n) : 0.0;
    const double dist = std::sqrt((double)dist2i(colA, colB));
    if (dist < 55.0 || vis == 0) return 0.f;
    // A grid lined up with the board but shifted a few squares to the side
    // also reads as two colours -- its extra columns are background. Counting
    // the squares that fit neither colour is what tells the two apart.
    return (float)(dist / (dist + rms + 1.0) * ((double)n / vis));
}

// Our own board is the best-looking checkerboard on the screen, so colour is
// what tells it apart from a real one: no site uses exactly these two.
const int OURS_A[3] = {238, 230, 206};
const int OURS_B[3] = {118, 150, 98};

bool looksLikeOurs(const int a[3], const int b[3]) {
    return (dist2i(a, OURS_A) <= 8 * 8 && dist2i(b, OURS_B) <= 8 * 8) ||
           (dist2i(a, OURS_B) <= 8 * 8 && dist2i(b, OURS_A) <= 8 * 8);
}

}  // namespace

bool findBoard(const ScreenImage& im, const std::vector<SDL_Rect>& skip, BoardRect& out) {
    out = BoardRect();
    if (im.empty()) {
        out.why = "no picture came back from the screen";
        return false;
    }
    // Every refusal below says this much at least: whether our own window's
    // marker stripe was even in the picture.
    const std::string mark = skip.empty() ? "no marker seen" : "marker found";

    // Stage one: every scale and phase, sampled at half resolution, keeping
    // only the handful of grids that already look like a checkerboard.
    const int stride = 2;
    const int rw = (im.w + stride - 1) / stride;
    const int rh = (im.h + stride - 1) / stride;
    // The coarse pass samples square centres, and on a full board half those
    // centres are pieces: the true grid then scores *below* a grid shifted
    // half a square (whose centres dodge the pieces), so ranking by the
    // coarse score alone drops boards that the walk would have locked onto
    // instantly -- one real capture sat at rank 43 and never got there. The
    // cut only has to discard the background; the walk decides the rest.
    const int TOPK = 64;
    std::vector<Cand> top;
    top.reserve(TOPK);
    int nAdmit = 0;  // grids that passed the coarse cut, before TOPK trimmed them
    // OV_FIND also keeps every grid the coarse cut admitted, so the asked one
    // can be ranked against them: a board whose grid passes the cut and still
    // never reaches the walk was evicted, and the eviction is the finding.
    const char* fdump = std::getenv("OV_FIND");
    std::vector<Cand> admitted;

    for (int s = 14; s <= 104; s += 2) {
        const int step = std::max(4, s / 2);
        for (int y = 0; y + 8 * s <= rh; y += step)
            for (int x = 0; x + 8 * s <= rw; x += step) {
                const float sc = coarseScore(im, x, y, s, stride, skip);
                if (sc < 0.45f) continue;
                ++nAdmit;
                if (fdump) admitted.push_back(Cand{sc, x, y, s});
                if ((int)top.size() < TOPK) {
                    top.push_back(Cand{sc, x, y, s});
                    continue;
                }
                int wi = 0;
                for (int i = 1; i < (int)top.size(); ++i)
                    if (top[i].sc < top[wi].sc) wi = i;
                if (sc > top[wi].sc) top[wi] = Cand{sc, x, y, s};
            }
    }
    const int nCand = (int)top.size();

    // Stage two: the coarse pass only pins the board down to half a square, so
    // each candidate is walked onto the exact pixel -- per axis, because the
    // score peaks sharply once all 64 corner samples line up again.
    Cand best{0.f, 0, 0, 0};
    int bestA[3] = {0, 0, 0}, bestB[3] = {0, 0, 0};
    float hiddenSc = 0.f;
    int hiddenCells = 0;
    int hiddenX = 0, hiddenY = 0, hiddenCell = 0;
    int nHidden = 0;  // candidates whose squares our own window stood on
    // Called with the candidate just walked into place, so a board we end up
    // refusing for being under our own window can still say which one it was.
    auto rememberHidden = [&](float sc, int cov, int hx, int hy, int hc) {
        if (sc > hiddenSc) {
            hiddenSc = sc;
            hiddenCells = cov;
            hiddenX = hx;
            hiddenY = hy;
            hiddenCell = hc;
        }
    };

    // Walk a candidate onto the exact pixel. The two axes are swept together
    // first and only then polished one at a time: a grid out by a few pixels
    // in both x and y scores the same whichever axis is moved on its own --
    // the four corner samples land in the wrong squares either way -- so a
    // step at a time never leaves that spot, and it is the pair that has to
    // get off it. `mindSkip` keeps the walk clear of the squares our own
    // window covers; a board being measured *through* the window walks over
    // them regardless and is scored on what still shows.
    auto refine = [&](int span, int& gx, int& gy, int& gcell, float& gsc, int ca[3],
                      int cb[3], bool mindSkip, int minPer) {
        int px = gx, py = gy;
        float psc = gsc;
        int pa[3] = {ca[0], ca[1], ca[2]}, pb[3] = {cb[0], cb[1], cb[2]};
        for (int dy = -span; dy <= span; dy += 4) {
            for (int dx = -span; dx <= span; dx += 4) {
                if (dx == 0 && dy == 0) continue;
                int aa[3] = {0, 0, 0}, bb[3] = {0, 0, 0}, cv = 0;
                const float s2 =
                    evalGrid(im, gx + dx, gy + dy, gcell, skip, aa, bb, &cv, minPer);
                if (mindSkip && cv != 0) continue;
                if (s2 > psc) {
                    psc = s2;
                    px = gx + dx;
                    py = gy + dy;
                    std::memcpy(pa, aa, sizeof pa);
                    std::memcpy(pb, bb, sizeof pb);
                }
            }
        }
        gx = px;
        gy = py;
        gsc = psc;
        std::memcpy(ca, pa, sizeof pa);
        std::memcpy(cb, pb, sizeof pb);

        for (int pass = 0; pass < 3; ++pass) {
            for (int axis = 0; axis < 3; ++axis) {
                const int reach = (axis == 2) ? 8 : span;
                int& v = (axis == 0) ? gx : (axis == 1) ? gy : gcell;
                const int here = v;
                int bestV = here;
                float bestSc = gsc;
                int ba[3] = {ca[0], ca[1], ca[2]}, bc[3] = {cb[0], cb[1], cb[2]};
                for (int d = -reach; d <= reach; ++d) {
                    if (d == 0) continue;
                    v = here + d;
                    int aa[3] = {0, 0, 0}, bb[3] = {0, 0, 0}, cv = 0;
                    const float s2 = evalGrid(im, gx, gy, gcell, skip, aa, bb, &cv, minPer);
                    if (mindSkip && cv != 0) continue;
                    if (s2 > bestSc) {
                        bestSc = s2;
                        bestV = here + d;
                        std::memcpy(ba, aa, sizeof ba);
                        std::memcpy(bc, bb, sizeof bc);
                    }
                }
                v = bestV;
                gsc = bestSc;
                std::memcpy(ca, ba, sizeof ba);
                std::memcpy(cb, bc, sizeof bc);
            }
        }
    };

    // Where the walk left every candidate, for the dump below: the refusal
    // line only ever reports the winner, and the winner is not always the
    // candidate that should have won.
    std::vector<Cand> walked;

    for (const Cand& c : top) {
        const int bs = c.s * stride;
        const int span = (std::max(4, c.s / 2) * stride) / 2 + 2;
        int x = c.x * stride, y = c.y * stride, cell = bs;
        int a[3] = {0, 0, 0}, b[3] = {0, 0, 0}, cov = 0;
        float sc = evalGrid(im, x, y, cell, skip, a, b, &cov, 24);
        if (cov > 0) {
            ++nHidden;
            // Our own window stands over part of it: walk the grid into place
            // over what still shows, then say how good the board underneath
            // looks -- that is what `covered` is measured from.
            refine(span, x, y, cell, sc, a, b, false, 8);
            int cov2 = 0;
            const float hs = evalGrid(im, x, y, cell, skip, a, b, &cov2, 8);
            if (cov2 > 0 && hs > 0.f) rememberHidden(hs, cov2, x, y, cell);
            continue;
        }
        refine(span, x, y, cell, sc, a, b, true, 24);
        walked.push_back(Cand{sc, x, y, cell});
        if (sc > best.sc) {
            best = Cand{sc, x, y, cell};
            std::memcpy(bestA, a, sizeof bestA);
            std::memcpy(bestB, b, sizeof bestB);
        }
    }

    // The pair's last step: a size cannot be judged from where the winner
    // stands, and the position cannot reach where the better size needs it.
    // A cell-63 board under a cell-64 grid scores exactly that way -- at
    // (32,32) neither size wins (the odd one only pays once the grid is at
    // 35), and from (32,32) every step toward 35 carries the too-long grid
    // further past the board's far edge -- so the walk settles a pixel out,
    // the corner samples then read the page or their neighbour, and the
    // outer files come back as one wrong colour (a live board read itself
    // as seventeen kings this way, both a- and h-files phantom pieces).
    // Each neighbouring size therefore gets its own joint sweep around the
    // settled position before it is given up: (35,35,63) is within reach
    // of (32,32), and one look at it ends the argument.
    for (int ds = -1; ds <= 1; ds += 2) {
        const int tc = best.s + ds;
        if (tc < 14 || tc > 104) continue;
        const int sx = best.x, sy = best.y;
        int tx = sx, ty = sy;
        int ta[3] = {0, 0, 0}, tb[3] = {0, 0, 0};
        float tsc = -1.f;
        for (int dy = -8; dy <= 8; ++dy)
            for (int dx = -8; dx <= 8; ++dx) {
                int aa[3] = {0, 0, 0}, bb[3] = {0, 0, 0}, cv = 0;
                const float s2 = evalGrid(im, sx + dx, sy + dy, tc, skip, aa, bb, &cv, 24);
                if (cv != 0) continue;
                if (s2 > tsc) {
                    tsc = s2;
                    tx = sx + dx;
                    ty = sy + dy;
                    std::memcpy(ta, aa, sizeof ta);
                    std::memcpy(tb, bb, sizeof tb);
                }
            }
        if (tsc > best.sc) {
            best = Cand{tsc, tx, ty, tc};
            std::memcpy(bestA, ta, sizeof bestA);
            std::memcpy(bestB, tb, sizeof bestB);
        }
    }

    // OV_FIND=1 prints where the walk left every candidate and how each one
    // scored; OV_FIND=x,y,cell scores that exact grid on top of it. "the best
    // grid scored 0.54" says the search failed -- this says what it found in
    // the board's place, or what the board itself is worth when its position
    // is already known, which is the difference between a board that is not
    // there and one the walk cannot settle on.
    if (fdump) {
        std::fprintf(stderr, "find: admitted %d grids, %d kept by TOPK\n", nAdmit, nCand);
        int fx = 0, fy = 0, fc = 0;
        if (std::sscanf(fdump, "%d,%d,%d", &fx, &fy, &fc) == 3) {
            int aa[3] = {0, 0, 0}, bb[3] = {0, 0, 0}, cv = 0;
            const float gsc = evalGrid(im, fx, fy, fc, skip, aa, bb, &cv, 24);
            std::fprintf(stderr,
                         "find: asked %d,%d cell %d -> %.3f (covered %d) a %d,%d,%d b %d,%d,%d\n",
                         fx, fy, fc, gsc, cv, aa[0], aa[1], aa[2], bb[0], bb[1], bb[2]);
            // The coarse pass only ever looks at grids the search space holds
            // -- half-resolution, so all three numbers must be even -- and
            // where the asked grid ranks among everything the cut admitted
            // says whether the walk was ever going to see it.
            if (((fx | fy | fc) & 1) == 0) {
                const float csc =
                    coarseScore(im, fx / stride, fy / stride, fc / stride, stride, skip);
                std::sort(admitted.begin(), admitted.end(),
                          [](const Cand& a, const Cand& b) { return a.sc > b.sc; });
                int rank = -1;
                for (size_t i = 0; i < admitted.size(); ++i)
                    if (admitted[i].x == fx / stride && admitted[i].y == fy / stride &&
                        admitted[i].s == fc / stride) {
                        rank = (int)i;
                        break;
                    }
                std::fprintf(stderr,
                             "find: coarse there -> %.3f, rank %d of %d admitted%s\n", csc, rank,
                             (int)admitted.size(),
                             rank >= TOPK ? " (evicted before the walk)" : "");
                for (int i = 0; i < 8 && i < (int)admitted.size(); ++i)
                    std::fprintf(stderr, "find: coarse top %d: %d,%d cell %d -> %.3f\n", i,
                                 admitted[i].x * stride, admitted[i].y * stride,
                                 admitted[i].s * stride, admitted[i].sc);
            }
        }
        std::sort(walked.begin(), walked.end(),
                  [](const Cand& a, const Cand& b) { return a.sc > b.sc; });
        for (size_t i = 0; i < walked.size(); ++i)
            std::fprintf(stderr, "find: cand %2d %d,%d cell %d -> %.3f\n", (int)i, walked[i].x,
                         walked[i].y, walked[i].s, walked[i].sc);
    }

    if (best.sc < 0.62f) {
        // Worth saying out loud when a good board is simply behind our window.
        char why[224];
        if (hiddenSc >= 0.62f) {
            out.covered = hiddenCells;
            // `ok` stays false, so there is nothing here to read; these say
            // which board was refused -- the one standing under our window.
            out.x = hiddenX;
            out.y = hiddenY;
            out.cell = hiddenCell;
            std::snprintf(why, sizeof why,
                          "a board at %d,%d cell %d scored %.2f but %d of its squares are under "
                          "our window (%s)",
                          hiddenX, hiddenY, hiddenCell, hiddenSc, hiddenCells, mark.c_str());
        } else {
            out.covered = 0;
            std::snprintf(why, sizeof why,
                          "best grid scored %.2f, 0.62 needed - %d candidates, %d of them under "
                          "our window (%s)",
                          best.sc, nCand, nHidden, mark.c_str());
        }
        out.why = why;
        return false;
    }
    // Our own board is painted in a real site's palette as well -- chess.com's
    // default green is exactly it -- so matching colours only prove a board is
    // ours when we cannot tell any other way. The marker stripe has already
    // told us where our window is whenever `skip` is filled, and our board
    // lives inside that rectangle; with no stripe we are blind to our own
    // window, and the colours are all we have left to go on.
    bool overOurs = false;
    for (size_t i = 0; i < skip.size() && !overOurs; ++i) {
        const SDL_Rect& w = skip[i];
        const int x1 = best.x + best.s * 8, y1 = best.y + best.s * 8;
        overOurs = best.x < w.x + w.w && x1 > w.x && best.y < w.y + w.h && y1 > w.y;
    }
    if (overOurs || (skip.empty() && looksLikeOurs(bestA, bestB))) {
        char why[224];
        if (hiddenSc >= 0.62f) {
            out.covered = hiddenCells;
            // `ok` stays false, so there is nothing here to read; these say
            // which board was refused -- the one standing under our window.
            out.x = hiddenX;
            out.y = hiddenY;
            out.cell = hiddenCell;
            std::snprintf(why, sizeof why,
                          "the winner at %d,%d cell %d scored %.2f but it is our own window's "
                          "board, with a real one under it (%s)",
                          best.x, best.y, best.s, best.sc, mark.c_str());
        } else {
            out.covered = 0;
            std::snprintf(why, sizeof why,
                          "the best grid at %d,%d cell %d scored %.2f but it is our own window's "
                          "board (%s)",
                          best.x, best.y, best.s, best.sc, mark.c_str());
        }
        out.why = why;
        return false;
    }
    out.ok = true;
    out.x = best.x;
    out.y = best.y;
    out.cell = best.s;
    std::memcpy(out.colA, bestA, sizeof bestA);
    std::memcpy(out.colB, bestB, sizeof bestB);
    out.score = best.sc;
    char found[224];
    std::snprintf(found, sizeof found, "grid %.2f at %d,%d cell %d (%s)", best.sc, best.x, best.y,
                  best.s, mark.c_str());
    out.why = found;
    return true;
}

// The finder's own checkerboard score for a grid at exactly this position
// and square size, exposed so a caller holding two opinions about the same
// board -- the lattice's rectangle and the model's -- can score both and
// take the better one. A grid our own window covers scores zero: that is
// not a board anyone can read.
float scoreGridAt(const ScreenImage& img, int x, int y, int cell,
                  const std::vector<SDL_Rect>& skip) {
    int colA[3], colB[3];
    int cov = 0;
    const float sc = evalGrid(img, x, y, cell, skip, colA, colB, &cov);
    return cov > 0 ? 0.f : sc;
}

// ------------------------------------------------------------- reading ----

namespace {

// A point inside a convex polygon: the arrow is a quad and a triangle.
bool inConvex(const SDL_Point* p, int n, int x, int y) {
    int sign = 0;
    for (int i = 0; i < n; ++i) {
        const SDL_Point& a = p[i];
        const SDL_Point& b = p[(i + 1) % n];
        const int cr = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
        if (cr == 0) continue;
        const int s = cr > 0 ? 1 : -1;
        if (sign == 0)
            sign = s;
        else if (s != sign)
            return false;
    }
    return true;
}

// One number for how light a pixel looks. A piece pixel on somebody else's
// board is either clearly brighter than the square it stands on (a white
// piece) or far darker (a black one); what falls in between is the shadow the
// piece stands in, or the wash a site paints under a piece, and that belongs
// to the board.
int lumaOf(int r, int g, int b) { return (r * 299 + g * 587 + b * 114) / 1000; }
int lumaOf(const int rgb[3]) { return lumaOf(rgb[0], rgb[1], rgb[2]); }

// Whether a pixel is arrow ink rather than art: its channels spread apart
// further than a third of the brightest of them. A site's arrow paints at
// 0.40 to 0.57 this way (the green band crossing g2 on board104: 0.40, 0.43,
// 0.45, and its solid core at 0.57); every piece set measured off a real
// board stays under 0.23 and the squares themselves under 0.12, so the two
// never touch and 0.30 stands in the gap. Both masks drop these pixels
// before anything is measured, so templates and screen squares keep
// describing the same thing.
static bool tooSaturated(const uint8_t* p) {
    const int hi = std::max({(int)p[0], (int)p[1], (int)p[2]});
    const int lo = std::min({(int)p[0], (int)p[1], (int)p[2]});
    return hi > 0 && 10 * (hi - lo) > 3 * hi;
}
static bool tooSaturated(const int rgb[3]) {
    const int hi = std::max({rgb[0], rgb[1], rgb[2]});
    const int lo = std::min({rgb[0], rgb[1], rgb[2]});
    return hi > 0 && 10 * (hi - lo) > 3 * hi;
}

// How far from the square colour a pixel has to sit before it counts as piece:
// the worst case between a white and a black piece, so both ends work.
int maskThreshold(const int bg[3]) {
    static const int WHITE_P[3] = {250, 250, 246};
    static const int BLACK_P[3] = {44, 46, 53};
    const int d = std::min(dist2i(bg, WHITE_P), dist2i(bg, BLACK_P));
    return clampi((int)std::sqrt((double)d) / 3, 14, 60);
}

// Whether a pixel's brightness says "piece" rather than "square" -- the
// second test a candidate pixel has to pass, next to the colour distance
// above. Distance alone cannot do this job: over a bright square the ramp
// from square colour to a black piece runs three hundred units of colour
// and its whole first half is still the square's own AA, while a white
// piece sits only forty-six units away, so no threshold separates ring
// from piece at both ends. Brightness does: a piece pixel lies in the
// piece's own half of the range this square can hold -- dragged down
// toward black's 47, or pushed up toward white's 248 -- and only the last
// half of each ramp is kept. The templates are put through the same test,
// so screen and template masks drop exactly the same ring, which is what
// keeps the pawn's box a pawn's box on every square colour: on a cream
// square an unfiltered ring grew the box three pixels and met the bishop's
// proportions instead, tying bishop and pawn at one point.
//
// The guards at both call sites matter as much as the test: it runs only on
// bright squares (bgLuma >= 190) and only below cell 80. On a green square
// the colour threshold already cuts the ring, and the filter instead eats the
// piece's own mid-tones -- half a knight's mask went, and every candidate on
// that square scored below 60. And the ring is a fixed couple of pixels
// whatever the square size: at cell 92-104 it flips no decision (the
// fixtures read correctly without this), while stripping antialiasing and
// shadow there only costs margin.
static bool lumaIsPiece(int pLuma, int bgLuma) {
    const int down = bgLuma - pLuma, rangeD = std::max(1, bgLuma - 47);
    const int up = pLuma - bgLuma, rangeU = std::max(1, 248 - bgLuma);
    return 2 * down >= rangeD || 2 * up >= rangeU;
}

// What a silhouette says about itself, in pixels of the square it sits on.
// None of it depends on the art a piece set was drawn in: how tall the piece
// stands, how wide, how far it leans to one side, how thin the very top is.
// A piece read off somebody else's board can be told apart by these when the
// pixel overlap against our own drawing alone is not sure -- a king's cross
// is a narrow top where a queen's crown is broad, and only a knight leans.
struct ShapeStat {
    int minX = 0, minY = 0;  // where the box sits in the square
    int bw = 0, bh = 0;      // bounding box of the mask
    int area = 0;            // pixels in it
    int top = 0;             // widest row in the top fifth of that box
    int mirror = 0;          // pixels whose mirror across the square is not set
};

// From a finished mask and its per-row widths; x0..x1 / y0..y1 is the box the
// caller walked out while masking the square.
ShapeStat shapeStat(const std::vector<uint8_t>& mask, const std::vector<int>& rows, int cell,
                    int x0, int y0, int x1, int y1) {
    ShapeStat s;
    if (x1 < x0 || y1 < y0 || cell <= 0) return s;
    s.minX = x0;
    s.minY = y0;
    s.bw = x1 - x0 + 1;
    s.bh = y1 - y0 + 1;
    const int topY = std::min(cell - 1, y0 + std::max(1, s.bh / 5));
    for (int y = y0; y <= topY; ++y) s.top = std::max(s.top, rows[y]);
    for (int y = 0; y < cell; ++y)
        for (int x = 0; x < cell; ++x) {
            const size_t k = (size_t)y * cell + x;
            if (mask[k]) ++s.area;
            if (x < cell / 2 && mask[k] != mask[(size_t)y * cell + (cell - 1 - x)]) ++s.mirror;
        }
    return s;
}

// Both sides of a comparison are redrawn over their *own* bounding box onto
// one fixed grid, and every measurement afterwards happens there. A piece set
// drawn larger than ours, or a board of a different size, lands on the same
// grid stretched to fit -- so what is compared is the shape a piece makes and
// not the number of pixels it happens to cover. Two grids equal means the
// same drawing at the same scale, which is the one case where the pixel
// overlap used to be exact and still is.
constexpr int GRID = 40;

void fillGrid(std::vector<uint8_t>& g, const uint8_t* mask, int cell, const ShapeStat& s,
              int& cx, int& cy) {
    g.assign((size_t)GRID * GRID, 0);
    cx = cy = GRID / 2;
    if (s.bw <= 0 || s.bh <= 0 || cell <= 0) return;
    long sx = 0, sy = 0;
    int n = 0;
    for (int gy = 0; gy < GRID; ++gy) {
        const int py = s.minY + (gy * 2 + 1) * s.bh / (2 * GRID);
        if (py < 0 || py >= cell) continue;
        for (int gx = 0; gx < GRID; ++gx) {
            const int px = s.minX + (gx * 2 + 1) * s.bw / (2 * GRID);
            if (px < 0 || px >= cell) continue;
            if (!mask[(size_t)py * cell + px]) continue;
            g[(size_t)gy * GRID + gx] = 1;
            sx += gx;
            sy += gy;
            ++n;
        }
    }
    if (n > 0) {
        cx = (int)(sx / n);
        cy = (int)(sy / n);
    }
}

// One cell of give on every side of a shape. Sets draw the same outline at
// different weights -- a hairline cut on one board is a shaded band on the
// next -- and a cell either way is the difference between two drawings of
// the same piece, not between two different pieces. The overlap grades both
// pictures through this, so it grades shape rather than stroke.
void dilateGrid(const std::vector<uint8_t>& g, std::vector<uint8_t>& out) {
    out.assign((size_t)GRID * GRID, 0);
    for (int gy = 0; gy < GRID; ++gy)
        for (int gx = 0; gx < GRID; ++gx) {
            if (!g[(size_t)gy * GRID + gx]) continue;
            for (int dy = -1; dy <= 1; ++dy) {
                const int y = gy + dy;
                if (y < 0 || y >= GRID) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = gx + dx;
                    if (x < 0 || x >= GRID) continue;
                    out[(size_t)y * GRID + x] = 1;
                }
            }
        }
}

struct Tpl {
    Piece piece = 0;
    std::vector<uint8_t> mask;  // cell * cell, exactly as off the screen
    std::vector<int> rows;      // mask pixels per row (st's top fifth comes off these)
    int col[3] = {0, 0, 0};     // mean colour of those pixels
    int acol[3] = {0, 0, 0};    // mean of every piece of this colour's
                                // pixels -- what a square's colour is judged
                                // against, so the answer names the colour and
                                // the shape alone names the piece
    ShapeStat st;               // its own geometry, as above
    std::vector<uint8_t> g;     // the box redrawn on the GRID, for overlap
    std::vector<uint8_t> gm;    // ... and mirrored: a set draws its two
                                // knights facing opposite ways and the match
                                // has to work for either direction
    std::vector<uint8_t> gd;    // g and gm one cell fatter, so the overlap
    std::vector<uint8_t> gmd;   // compares shape rather than stroke
    int gcx = 0, gcy = 0;       // centroid of `g`, in grid cells -- printed
                                // by the dump, not used to line the pictures
                                // up (both boxes already fill the frame)
};

struct TplSet {
    int cell = 0;
    std::vector<Tpl> t[2];  // by square parity (0 = even = colA)
    bool ok = false;
};

// A piece is one shape. What else sits in an edge square is the coordinate
// letter a site paints into its corner: separate, small, and the reason an
// entire back rank can come out as knights once that letter is inside the
// square with the piece. Everything well under a fifth of the biggest blob
// goes -- a glyph is a few per cent of a piece, while a piece cut in two by
// an arrow still keeps both halves. Returns the pixels thrown away; `comp`
// and `work` are scratch the caller owns so this costs no allocation a square
// at a time.
int keepBiggestBlob(std::vector<uint8_t>& mask, int cell, std::vector<int>& comp,
                    std::vector<int>& work) {
    if (cell <= 0) return 0;
    comp.assign((size_t)cell * cell, 0);
    std::vector<int> size;  // pixels in each blob, by blob id
    size.push_back(0);      // id 0 means "not part of any blob"
    std::vector<int> bx0{0}, by0{0}, bx1{0}, by1{0};  // each blob's own corners
    int big = 0;
    for (int i = 0; i < cell * cell; ++i) {
        if (!mask[i] || comp[i]) continue;
        const int id = (int)size.size();
        work.clear();
        work.push_back(i);
        comp[i] = id;
        int n = 0;
        const int k0x = i % cell, k0y = i / cell;
        bx0.push_back(k0x);
        by0.push_back(k0y);
        bx1.push_back(k0x);
        by1.push_back(k0y);
        while (!work.empty()) {
            const int k = work.back();
            work.pop_back();
            ++n;
            const int kx = k % cell, ky = k / cell;
            if (kx < bx0[id]) bx0[id] = kx;
            if (kx > bx1[id]) bx1[id] = kx;
            if (ky < by0[id]) by0[id] = ky;
            if (ky > by1[id]) by1[id] = ky;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = kx + dx, ny = ky + dy;
                    if (nx < 0 || ny < 0 || nx >= cell || ny >= cell) continue;
                    const int nk = ny * cell + nx;
                    if (mask[nk] && !comp[nk]) {
                        comp[nk] = id;
                        work.push_back(nk);
                    }
                }
        }
        size.push_back(n);
        if (n > size[big]) big = id;
    }
    if (size.size() <= 2) return 0;  // one blob: the square held one shape
    // The ink of a band, keyed out, leaves a piece standing in pieces stacked
    // on each other -- a sliver of its top over the mass of its base -- and
    // the fifth would then call the top noise. Two blobs standing exactly on
    // each other's column, within a tenth of the square of each other, are one
    // shape the band walked through. A coordinate glyph never lands like that:
    // it hugs the corner, its column hardly reaching the piece's own edge, so
    // the merge cannot swallow it.
    {
        const int nBlob = (int)size.size();
        std::vector<int> root((size_t)nBlob);
        for (int a = 0; a < nBlob; ++a) root[a] = a;
        auto find = [&](int a) {
            while (root[a] != a) a = root[a] = root[root[a]];
            return a;
        };
        const int gapMax = std::max(2, cell / 10);
        bool joined = true;
        while (joined) {
            joined = false;
            for (int a = 1; a < nBlob && !joined; ++a)
                for (int b = a + 1; b < nBlob; ++b) {
                    if (find(a) == find(b)) continue;
                    const int ox = std::min(bx1[a], bx1[b]) - std::max(bx0[a], bx0[b]) + 1;
                    const int narrow = std::min(bx1[a] - bx0[a] + 1, bx1[b] - bx0[b] + 1);
                    const int gap = std::max(0, std::max(by0[a], by0[b]) -
                                                    std::min(by1[a], by1[b]) - 1);
                    if (ox * 5 >= 4 * narrow && gap <= gapMax) {
                        root[find(a)] = find(b);
                        joined = true;
                        break;
                    }
                }
        }
        std::vector<int> merged((size_t)nBlob, 0);
        for (int a = 1; a < nBlob; ++a) merged[find(a)] += size[a];
        big = 1;
        for (int a = 2; a < nBlob; ++a)
            if (merged[a] > merged[big]) big = a;
        const int cut = merged[big] / 5;
        int dropped = 0;
        for (int i = 0; i < cell * cell; ++i)
            if (mask[i] && find(comp[i]) != big && merged[find(comp[i])] < cut) {
                mask[i] = 0;
                ++dropped;
            }
        return dropped;
    }
}

// One piece drawn on one square colour, then masked exactly the way a square
// off the screen will be masked, so the two can be compared pixel for pixel.
bool renderPieceMask(int cell, const int bg[3], Piece pc, int thr, Tpl& out,
                     PieceStyle st = PieceStyle::classic) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, cell, cell, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return false;
    SDL_Renderer* r = SDL_CreateSoftwareRenderer(s);
    if (!r) {
        SDL_FreeSurface(s);
        return false;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, bg[0], bg[1], bg[2], 255);
    SDL_RenderClear(r);
    const SDL_Rect box{1, 1, cell - 2, cell - 2};
    drawPiece(r, pc, box, st);
    SDL_RenderPresent(r);

    const bool locked = SDL_MUSTLOCK(s);
    if (locked) SDL_LockSurface(s);
    out.mask.assign((size_t)cell * cell, 0);
    out.rows.assign(cell, 0);
    long n = 0;
    long cr = 0, cg = 0, cb = 0;
    int minX = cell, minY = cell, maxX = -1, maxY = -1;
    const uint32_t* px = (const uint32_t*)s->pixels;
    const int pitch = s->pitch / 4;
    const int thr2 = thr * thr;
    const int bgLuma = lumaOf(bg);
    const bool ringGate = bgLuma >= 190 && cell < 80;
    // What is not the square, first; what shape that leaves, after the small
    // separate blobs have gone (keepBiggestBlob). Measuring in a later pass
    // keeps the mask, the row widths and every statistic agreeing on the same
    // pixels -- dropping anything afterwards would leave them describing a
    // square that is no longer there.
    for (int y = 0; y < cell; ++y)
        for (int x = 0; x < cell; ++x) {
            const uint32_t v = px[(size_t)y * pitch + x];
            const int rgb[3] = {int((v >> 16) & 255), int((v >> 8) & 255), int(v & 255)};
            const int d2 = dist2i(rgb, bg);
            if (d2 <= thr2) continue;
            if (ringGate && !lumaIsPiece(lumaOf(rgb), bgLuma)) continue;
            // A soft shadow only darkens the square's own colour a little, so
            // a pixel barely clear of the square and darker than it is the
            // board showing through rather than the piece. Anything brighter
            // or well clear of that range is real: dark squares already get
            // this for free, their own threshold is the higher one.
            if (d2 <= 30 * 30 && lumaOf(rgb) < bgLuma) continue;
            // Arrow ink goes on the screen side (tooSaturated); no piece of
            // ours reaches that saturation, so this loses nothing here and
            // the rule the two masks are built by stays one and the same.
            if (tooSaturated(rgb)) continue;
            out.mask[(size_t)y * cell + x] = 1;
        }
    {
        std::vector<int> comp, work;
        keepBiggestBlob(out.mask, cell, comp, work);
    }
    for (int y = 0; y < cell; ++y)
        for (int x = 0; x < cell; ++x) {
            if (!out.mask[(size_t)y * cell + x]) continue;
            const uint32_t v = px[(size_t)y * pitch + x];
            const int rgb[3] = {int((v >> 16) & 255), int((v >> 8) & 255), int(v & 255)};
            out.rows[y]++;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
            cr += rgb[0];
            cg += rgb[1];
            cb += rgb[2];
            ++n;
        }
    if (locked) SDL_UnlockSurface(s);
    SDL_DestroyRenderer(r);
    SDL_FreeSurface(s);
    if (n == 0) return false;
    out.col[0] = (int)(cr / n);
    out.col[1] = (int)(cg / n);
    out.col[2] = (int)(cb / n);
    out.st = shapeStat(out.mask, out.rows, cell, minX, minY, maxX, maxY);
    fillGrid(out.g, out.mask.data(), cell, out.st, out.gcx, out.gcy);
    out.gm.assign((size_t)GRID * GRID, 0);
    for (int gy = 0; gy < GRID; ++gy)
        for (int gx = 0; gx < GRID; ++gx)
            out.gm[(size_t)gy * GRID + gx] = out.g[(size_t)gy * GRID + (GRID - 1 - gx)];
    dilateGrid(out.g, out.gd);
    dilateGrid(out.gm, out.gmd);
    return true;
}

// The shapes on both square colours of this board size: twelve pieces plus
// the second, foreign drawing of queen and king (four more).
bool buildTemplates(int cell, const int colA[3], const int colB[3], TplSet& out) {
    out.cell = cell;
    out.ok = false;
    out.t[0].clear();
    out.t[1].clear();
    const int bgs[2][3] = {{colA[0], colA[1], colA[2]}, {colB[0], colB[1], colB[2]}};
    for (int g = 0; g < 2; ++g) {
        const int thr = maskThreshold(bgs[g]);
        for (int col = 0; col < 2; ++col)
            for (int t = PAWN; t <= KING; ++t) {
                Tpl tpl;
                tpl.piece = makePiece((PieceType)t, (Color)col);
                if (renderPieceMask(cell, bgs[g], tpl.piece, thr, tpl)) out.t[g].push_back(tpl);
            }
        // A second drawing of the two royals, for the sets that crown them
        // the widest part of the piece instead of banding it. Appended after
        // the twelve; scored exactly like them -- the reader already takes
        // the best score of any template carrying one type, so whichever
        // drawing fits a square better names the type.
        const size_t classic = out.t[g].size();
        for (int col = 0; col < 2; ++col)
            for (int t : {QUEEN, KING}) {
                Tpl tpl;
                tpl.piece = makePiece((PieceType)t, (Color)col);
                if (renderPieceMask(cell, bgs[g], tpl.piece, thr, tpl, PieceStyle::foreign))
                    out.t[g].push_back(tpl);
            }
        // One anchor per colour: the mean of what that colour's six pieces
        // average to. Each piece's own mean would answer a different
        // question -- a knight is mostly outline and a queen mostly fill,
        // so their means differ by more than a white piece's and a black
        // one's do on a foreign board -- and the score was letting that
        // difference, not the silhouette, pick between types. The anchor is
        // taken over the twelve only: the second drawings carry the same
        // palette, and folding them in would weight the mean towards the
        // royals.
        {
            int sum[2][3] = {{0, 0, 0}, {0, 0, 0}};
            int n[2] = {0, 0};
            for (size_t i = 0; i < classic; ++i) {
                const Tpl& tp = out.t[g][i];
                const int c = (int)colorOf(tp.piece);
                for (int k = 0; k < 3; ++k) sum[c][k] += tp.col[k];
                ++n[c];
            }
            for (Tpl& tp : out.t[g]) {
                const int c = (int)colorOf(tp.piece);
                for (int k = 0; k < 3; ++k) tp.acol[k] = n[c] ? sum[c][k] / n[c] : tp.col[k];
            }
        }
    }
    out.ok = out.t[0].size() >= 6 && out.t[1].size() >= 6;
    return out.ok;
}

// How well one rendered shape fits what is on the screen. Both pictures are
// redrawn over their own bounding box onto one fixed grid before anything is
// measured, so a set that draws its pieces larger than ours -- or a board a
// different size -- lands on the same frame stretched to fit and the
// comparison is about *shape* rather than about how many pixels the two
// happen to cover. Both boxes fill that frame, so the two pictures are
// already in register and the overlap is taken as they sit: sliding one
// toward the other's centroid -- which a crown-heavy queen and a base-heavy
// king put in different places -- skews frames that were meant to meet
// corner to corner, and the squares that lost were the ones the skew hit
// hardest. It is measured both ways round as well because a set draws its
// white knight facing the other way from the black one, each picture
// counted one cell generous on every side (sets draw the same outline at
// different weights, and a hairline's difference is stroke rather than
// shape), and the ratio taken over that generous pair: a broad template
// cannot score by merely covering a smaller piece, since every extra cell
// it claims lands in the denominator too. Then a row by row width profile
// -- a knight and a bishop cover a similar area but very different rows;
// and then what is left: the mean colour, the size the piece was drawn at,
// how thin its very top is, how far it leans.

// The six things a template score is made of, kept whole for the refusal
// line. The total alone cannot say *which* measurement decided a square, and
// the weights are choices -- a misread's cause is a fact about one of them.
struct Terms {
    double iou = 0.0, prof = 0.0, col = 0.0, size = 0.0, top = 0.0, lop = 0.0;
};

double scoreTemplate(const Tpl& t, const uint8_t* obsG, const uint8_t* obsD, int cell,
                     const int* obsCol, const ShapeStat& fs, Terms* tr = nullptr) {
    auto overlap = [&](const std::vector<uint8_t>& tgd) {
        int inter = 0, uni = 0;
        for (int i = 0; i < GRID * GRID; ++i) {
            const bool o = obsD[i] != 0, t = tgd[i] != 0;
            if (o && t) ++inter;
            if (o || t) ++uni;
        }
        return uni > 0 ? (double)inter / uni : 0.0;
    };
    double iou = overlap(t.gd);
    // The same shape with the box read right to left: mirroring turns the
    // horse around, and either direction can be the one on the screen.
    const double iouBack = overlap(t.gmd);
    if (iouBack > iou) iou = iouBack;

    int sa = 0, sb = 0, diff = 0;
    for (int gy = 0; gy < GRID; ++gy) {
        int ro = 0, rt = 0;
        for (int gx = 0; gx < GRID; ++gx) {
            ro += obsG[(size_t)gy * GRID + gx];
            rt += t.g[(size_t)gy * GRID + gx];
        }
        sa += ro;
        sb += rt;
        diff += std::abs(ro - rt);
    }
    const double prof = (sa + sb) > 0 ? 1.0 - (double)diff / (sa + sb) : 0.0;

    // The silhouette on its own cannot tell a white piece from a black one --
    // they are cut from the same outlines, so the two score exactly alike and
    // whichever came first in the list would win every time. The colour they
    // are painted in has a say as well: the square's mean is taken over the
    // same kind of pixel (one far enough from the square to count as piece)
    // and compared against what that colour's six pieces average to, so a
    // piece read off the screen lands on top of its own colour and well clear
    // of the other, while the shape alone still picks the piece's type. Each
    // template's own mean would smuggle the type in through here: one piece
    // is mostly outline, another mostly fill, and between two types of one
    // colour that difference can run wider than the gap between the colours
    // themselves -- wide enough to hand a square to whichever rival's mean
    // happened to sit nearer.
    const double cd = std::sqrt((double)dist2i(obsCol, t.acol));
    const double colScore = 64.0 / (64.0 + cd);

    // The three that are ratios rather than counts, so a piece set's drawing
    // style and a board's size have little to argue with. `k` says how much
    // of a difference counts: a whole square before size gives up entirely --
    // its job now is only to notice that the thing at all is the size of a
    // piece, since which *kind* of piece is settled on the grid above -- a
    // third of the box in top width, a fifth in how lopsided the shape is.
    auto howClose = [](double a, double b, double k) {
        const double d = a < b ? b - a : a - b;
        const double v = 1.0 - k * d;
        return v < 0.0 ? 0.0 : v;
    };
    const double size = howClose((double)fs.bw, (double)t.st.bw, 1.0 / cell) *
                        howClose((double)fs.bh, (double)t.st.bh, 1.0 / cell);
    const double top = howClose(fs.bw ? (double)fs.top / fs.bw : 0.0,
                                t.st.bw ? (double)t.st.top / t.st.bw : 0.0, 3.0);
    const double lopsided = howClose(fs.area ? (double)fs.mirror / fs.area : 0.0,
                                     t.st.area ? (double)t.st.mirror / t.st.area : 0.0, 4.0);

    if (tr) {
        tr->iou = iou;
        tr->prof = prof;
        tr->col = colScore;
        tr->size = size;
        tr->top = top;
        tr->lop = lopsided;
    }
    return 0.30 * iou + 0.17 * prof + 0.25 * colScore + 0.16 * size + 0.07 * top +
           0.05 * lopsided;
}

// Image row 0 is the top of the screen. Whichever way the board is turned its
// bottom-left square is a dark one, so only the colour the player chose and a
// manual flip decide where the coordinates go.
int squareOfImage(int r, int c, bool blackAtBottom) {
    const int f = blackAtBottom ? 7 - c : c;
    const int rk = blackAtBottom ? r : 7 - r;
    return makeSquare(f, rk);
}

char pieceChar(Piece p) {
    char c;
    switch (typeOf(p)) {
        case PAWN: c = 'P'; break;
        case KNIGHT: c = 'N'; break;
        case BISHOP: c = 'B'; break;
        case ROOK: c = 'R'; break;
        case QUEEN: c = 'Q'; break;
        default: c = 'K'; break;
    }
    return p < 0 ? (char)(c + ('a' - 'A')) : c;
}

std::string sqName(int sq) {
    std::string s;
    s += (char)('a' + fileOf(sq));
    s += (char)('1' + rankOf(sq));
    return s;
}

// En passant is nowhere on the board itself, so it is only offered when the
// position changed under a pawn in one move and an enemy pawn stands next to it.
std::string enPassant(const Piece b[64], const Piece* prev, Color side) {
    if (!prev) return "-";
    for (int sq = 0; sq < 64; ++sq) {
        const Piece p = b[sq];
        if (isEmpty(p) || typeOf(p) != PAWN) continue;
        const int dir = (p > 0) ? 1 : -1;
        const int f = fileOf(sq);
        const int rk = rankOf(sq);
        const int fromRk = rk - 2 * dir;
        const int midRk = rk - dir;
        if (fromRk < 0 || fromRk > 7 || midRk < 0 || midRk > 7) continue;
        if (prev[makeSquare(f, rk)] != 0) continue;
        if (prev[makeSquare(f, midRk)] != 0) continue;
        if (prev[makeSquare(f, fromRk)] != p) continue;
        if (side == colorOf(p)) continue;  // the mover is not the side to move
        bool taken = false;
        for (int df = -1; df <= 1; df += 2) {
            const int nf = f + df;
            if (nf < 0 || nf > 7) continue;
            const Color enemy = (colorOf(p) == WHITE) ? BLACK : WHITE;
            if (b[makeSquare(nf, rk)] == makePiece(PAWN, enemy)) taken = true;
        }
        if (taken) return sqName(makeSquare(f, midRk));
    }
    return "-";
}

std::string buildFen(const Piece b[64], Color side, const Piece* prev) {
    std::string fen;
    for (int rk = 7; rk >= 0; --rk) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            const Piece p = b[makeSquare(f, rk)];
            if (isEmpty(p)) {
                ++empty;
                continue;
            }
            if (empty) {
                fen += (char)('0' + empty);
                empty = 0;
            }
            fen += pieceChar(p);
        }
        if (empty) fen += (char)('0' + empty);
        if (rk) fen += '/';
    }
    fen += ' ';
    fen += (side == WHITE ? 'w' : 'b');
    fen += ' ';
    // Castling rights are invisible after the fact: a king and a rook still
    // sitting on their starting squares is the usual reading.
    std::string castle;
    if (b[makeSquare(4, 0)] == makePiece(KING, WHITE)) {
        if (b[makeSquare(7, 0)] == makePiece(ROOK, WHITE)) castle += 'K';
        if (b[makeSquare(0, 0)] == makePiece(ROOK, WHITE)) castle += 'Q';
    }
    if (b[makeSquare(4, 7)] == makePiece(KING, BLACK)) {
        if (b[makeSquare(7, 7)] == makePiece(ROOK, BLACK)) castle += 'k';
        if (b[makeSquare(0, 7)] == makePiece(ROOK, BLACK)) castle += 'q';
    }
    fen += castle.empty() ? std::string("-") : castle;
    fen += ' ';
    fen += enPassant(b, prev, side);
    fen += " 0 1";
    return fen;
}

}  // namespace

ReadResult readBoard(const ScreenImage& img, const BoardRect& b, bool blackAtBottom, Color side,
                     const ArrowGeom& arrow, const Piece* prev, bool wantDetail) {
    ReadResult rr;
    if (!b.ok || b.cell < 16 || img.empty()) return rr;
    // A rectangle the frame does not fully contain cannot be read: the
    // samples for the squares past the edge would be pixels that are not in
    // the image at all. The model measures its board wherever it found the
    // checkerboard -- including off the bottom of the screen -- so this is
    // reachable, and a refused read beats a read of memory.
    if (b.x < 0 || b.y < 0 || b.x + (long)b.cell * 8 > img.w ||
        b.y + (long)b.cell * 8 > img.h)
        return rr;
    const int cell = b.cell;

    TplSet tpl;
    if (!buildTemplates(cell, b.colA, b.colB, tpl)) return rr;

    const int inset = std::max(1, cell / 24);
    const int area = std::max(1, (cell - 2 * inset) * (cell - 2 * inset));
    const int q = std::max(2, cell * 45 / 100);
    const int off[6][2] = {{-q, -q}, {q, -q}, {-q, q}, {q, q}, {-q, 0}, {q, 0}};
    const bool hasArrow = arrow.on && arrow.bbox[2] > 0 && arrow.bbox[3] > 0;

    std::vector<uint8_t> obs((size_t)cell * cell, 0);
    std::vector<uint8_t> obsG((size_t)GRID * GRID, 0);
    std::vector<uint8_t> obsD((size_t)GRID * GRID, 0);
    int obsGcx = 0, obsGcy = 0;
    std::vector<int> obsRows(cell, 0);
    std::vector<int> blobComp((size_t)cell * cell, 0), blobWork;
    std::vector<int> colSat(cell, 0);  // saturated pixels per column, for the band
    // What every square was read as, for the refusal line only: the answer,
    // how sure it was, how each *other* piece type scored (a king losing to a
    // knight by two points is a near miss; by twenty it is a different bug
    // altogether) and how much left the square with a smaller blob.
    struct SqNote {
        char got = 0;    // pieceChar, or '?' for something that was not a piece
        char gtype = 0;  // the winner's type, so the list knows what to skip
        int sc = 0;      // winning score, per cent
        int frac = 0;    // per cent of the square with something on it
        int drop = 0;    // per cent thrown away with the smaller blobs
        // Every piece type's best score, per cent. A misread is settled by
        // what the *right* answer scored, not by what won: a king losing to a
        // knight by two points is a tuning problem, losing by thirty is not
        // the same bug at all.
        int typeSc[7] = {0, 0, 0, 0, 0, 0, 0};
        // ...and the six measurements that got it there, same index: which
        // weight moved is the whole question once the total is in.
        Terms typeTr[7];
        // Raw geometry behind the size term: this square's mask box, and the
        // box of whichever template won for each type. `s` above is a
        // weighted product of the two; the score alone cannot say whether a
        // piece was the wrong shape or simply drawn at a different scale,
        // and only the numbers can.
        int bw = 0, bh = 0;
        int tBw[7] = {0, 0, 0, 0, 0, 0, 0}, tBh[7] = {0, 0, 0, 0, 0, 0, 0};
    };
    SqNote note[64] = {};
    double confSum = 0.0;
    // OV_GRID=<square> collects that square's two pictures for the caller to
    // print: what the screen had there, and each candidate drawn on the same
    // grid, side by side. The overlap is computed on exactly these grids, so
    // a comparison the totals cannot explain is one that can be looked at.
    const char* gDump = std::getenv("OV_GRID");
    std::vector<uint8_t> gDumpObs;
    const std::vector<Tpl>* gDumpSet = nullptr;
    std::string gDumpName;
    // One column per piece type, not just the three the totals liked: a
    // square misread because the right type scored fourth is exactly the one
    // whose picture is missing from a top-three dump.
    int gDumpIdx[7] = {-1, -1, -1, -1, -1, -1, -1};
    double gDumpSc[7] = {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
    Terms gDumpTr[7] = {};
    int gDumpCx = GRID / 2, gDumpCy = GRID / 2;
    // The square's widest top-fifth row as a ratio of its width, in pixels:
    // the `t` term compares exactly these two ratios (this one against each
    // template's own), and the grid columns below count cells instead, which
    // follows them only loosely.
    double gDumpTop = 0.0;

    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) {
            const int x0 = b.x + c * cell;
            const int y0 = b.y + r * cell;
            std::fill(obs.begin(), obs.end(), 0);
            std::fill(obsRows.begin(), obsRows.end(), 0);

            // The square's own colour, from six points near its edges: the
            // four corners and the middle of each side. A piece stops well
            // short of that (ours 13% in, most sets nearer 10%), so what
            // these catch is the site's coordinate glyphs -- and six points
            // lose two of them (a label and a letter, say) before the
            // median stops being the square's own colour.
            int smp[6][3];
            const int cx = x0 + cell / 2, cy = y0 + cell / 2;
            for (int k = 0; k < 6; ++k) sampleBox(img, cx + off[k][0], cy + off[k][1], 3, smp[k]);
            int bg[3];
            medianN(smp, 6, bg);
            const int thr2 = maskThreshold(bg) * maskThreshold(bg);
            const int bgLuma = lumaOf(bg);
            const bool ringGate = bgLuma >= 190 && cell < 80;
            if (std::getenv("OV_SQBG")) {
                std::fprintf(stderr, "sqbg c%d r%d:", c, r);
                for (int k = 0; k < 6; ++k)
                    std::fprintf(stderr, " %d,%d,%d", smp[k][0], smp[k][1], smp[k][2]);
                std::fprintf(stderr, " -> bg %d,%d,%d thr %d\n", bg[0], bg[1], bg[2],
                             maskThreshold(bg));
            }

            int mask = 0, minX = cell, minY = cell, maxX = -1, maxY = -1;
            long cr = 0, cg = 0, cb = 0;
            int dropped = 0;
            // Arrow ink, once seen, is remembered by colour: a site's arrow
            // is solid at its core and a ramp of blends where it meets the
            // square, and only the core is saturated enough to be caught
            // below. The colours that were caught name the ink, and what
            // follows the core is then found by the line it lies on --
            // from the square's own colour toward that ink, at whatever
            // opacity the site drew it.
            int inkCol[16][3] = {}, inkCnt[16] = {}, inkN = 0, inkMass = 0;
            // What is not the square. Only the tests happen here; the shape
            // is measured afterwards, from what survived them, so the mask,
            // the row widths and every statistic describe the same pixels.
            for (int y = inset; y < cell - inset; ++y)
                for (int x = inset; x < cell - inset; ++x) {
                    const int gx = x0 + x, gy = y0 + y;
                    // The arrow lies over two squares; those pixels are the
                    // arrow's, not the board's, so they are left out.
                    if (hasArrow && gx >= arrow.bbox[0] && gy >= arrow.bbox[1] &&
                        gx < arrow.bbox[0] + arrow.bbox[2] && gy < arrow.bbox[1] + arrow.bbox[3] &&
                        (inConvex(arrow.shaft, 4, gx, gy) || inConvex(arrow.head, 3, gx, gy)))
                        continue;
                    const uint8_t* p = img.px(gx, gy);
                    const int d2 = dist2p(p, bg);
                    if (d2 <= thr2) continue;
                    if (ringGate && !lumaIsPiece(lumaOf(p[0], p[1], p[2]), bgLuma)) continue;
                    // A soft shadow only darkens the square's own colour a
                    // little: a pixel barely clear of the square and darker
                    // than it is the board, not the piece. Templates drop the
                    // same pixels, so the two masks still match.
                    if (d2 <= 30 * 30 && lumaOf(p[0], p[1], p[2]) < bgLuma) continue;
                    // Arrow ink is not art. The band a site's arrow drags
                    // across a square joins the piece's own silhouette and
                    // stretches its box upwards -- on the green board it made
                    // the bishop's tall template the fitter box and cost the
                    // pawn on g2 its square. Templates cannot draw a saturated
                    // pixel, so dropping these keeps both masks alike.
                    if (tooSaturated(p)) {
                        ++inkMass;
                        ++colSat[x];
                        int k = 0;
                        for (; k < inkN; ++k)
                            if (inkCol[k][0] == p[0] && inkCol[k][1] == p[1] &&
                                inkCol[k][2] == p[2])
                                break;
                        if (k == inkN && inkN < 16) {
                            inkCol[inkN][0] = p[0];
                            inkCol[inkN][1] = p[1];
                            inkCol[inkN][2] = p[2];
                            ++inkN;
                        }
                        if (k < inkN) ++inkCnt[k];
                        continue;
                    }
                    obs[(size_t)y * cell + x] = 1;
                }
            // The ramp the core leaves behind: blends of that ink with the
            // square's own colour, none of them saturated enough to be told
            // apart by saturation alone -- they would stand along the piece
            // and stretch its box just as the core did. Every such blend
            // lies on the line from the square towards the ink colour; a
            // piece does not, its grays standing far off those lines, its
            // whites before the ink and its blacks past it. So a band
            // crossing a piece cuts away only the band, and keepBiggestBlob
            // below finds the halves it left.
            if (inkMass >= 24) {
                // What the band looks like *over the square itself* names
                // it: a site draws its arrow at one opacity, so the core
                // over the square is one fixed tone, and the core over a
                // piece is that tone pushed a little toward the piece's own
                // colour. The strip at the very top of the square -- two or
                // three rows, which no piece reaches and no coordinate
                // glyph sits in, and clear of the board's own line at the
                // edges -- holds only the band lying on the square, so the
                // loudest saturated colour there is the band's empty
                // signature. An arrow that stops short of the top leaves
                // that strip blank, and then there is nothing to name the
                // band by.
                int aCol[3] = {}, haveA = 0;
                {
                    const int wy0 = inset, wy1 = std::min(cell - inset, inset + std::max(2, cell / 24));
                    const int wx0 = inset + cell / 10, wx1 = cell - inset - cell / 10;
                    int mCol[8][3] = {}, mCnt[8] = {}, mN = 0;
                    for (int y = wy0; y < wy1; ++y)
                        for (int x = wx0; x < wx1; ++x) {
                            const int gx = x0 + x, gy = y0 + y;
                            if (hasArrow && gx >= arrow.bbox[0] && gy >= arrow.bbox[1] &&
                                gx < arrow.bbox[0] + arrow.bbox[2] &&
                                gy < arrow.bbox[1] + arrow.bbox[3] &&
                                (inConvex(arrow.shaft, 4, gx, gy) || inConvex(arrow.head, 3, gx, gy)))
                                continue;
                            const uint8_t* p = img.px(gx, gy);
                            if (!tooSaturated(p)) continue;
                            int k = 0;
                            for (; k < mN; ++k)
                                if (mCol[k][0] == p[0] && mCol[k][1] == p[1] && mCol[k][2] == p[2])
                                    break;
                            if (k == mN && mN < 8) {
                                mCol[mN][0] = p[0];
                                mCol[mN][1] = p[1];
                                mCol[mN][2] = p[2];
                                ++mN;
                            }
                            if (k < mN) ++mCnt[k];
                        }
                    int best = -1;
                    for (int k = 0; k < mN; ++k) {
                        const long dd = (long)(mCol[k][0] - bg[0]) * (mCol[k][0] - bg[0]) +
                                        (long)(mCol[k][1] - bg[1]) * (mCol[k][1] - bg[1]) +
                                        (long)(mCol[k][2] - bg[2]) * (mCol[k][2] - bg[2]);
                        if (mCnt[k] >= 6 && dd >= 900 && (best < 0 || mCnt[k] > mCnt[best]))
                            best = k;
                    }
                    if (best >= 0) {
                        aCol[0] = mCol[best][0];
                        aCol[1] = mCol[best][1];
                        aCol[2] = mCol[best][2];
                        haveA = 1;
                    }
                }
                if (haveA) {
                    // Named. One line now says what is not the square: the
                    // band's blends over it lie on the square's own colour
                    // toward that tone, at any opacity, and go. What the
                    // band lies *over* is told apart by how far it pushed
                    // the tone: not at all means the square underneath,
                    // while a piece showing through the site's opacity
                    // shifts it by a fixed small step -- a white piece, the
                    // only colour that shifts it inward rather than down,
                    // moves it by a little over a dozen levels. Further
                    // than that and some other layer is in the pixel, the
                    // board's own line or a second arrow, which is not the
                    // piece either. Only the columns the band runs down are
                    // read that way; the band's own edge columns are part
                    // blend with the square and part with the piece, and
                    // the bottom of the square holds the board's line,
                    // which blends the same way.
                    const int showLo = 36, showHi = 625;
                    const int colNeed = (cell + 2) / 3, yCut = cell - cell / 8;
                    for (int y = inset; y < cell - inset; ++y)
                        for (int x = inset; x < cell - inset; ++x) {
                            const int gx = x0 + x, gy = y0 + y;
                            if (hasArrow && gx >= arrow.bbox[0] && gy >= arrow.bbox[1] &&
                                gx < arrow.bbox[0] + arrow.bbox[2] &&
                                gy < arrow.bbox[1] + arrow.bbox[3] &&
                                (inConvex(arrow.shaft, 4, gx, gy) || inConvex(arrow.head, 3, gx, gy)))
                                continue;
                            const uint8_t* p = img.px(gx, gy);
                            if (tooSaturated(p)) {
                                if (colSat[x] < colNeed || y >= yCut) continue;
                                const long ddx = p[0] - aCol[0], ddy = p[1] - aCol[1],
                                           ddz = p[2] - aCol[2];
                                // What shows through a white piece steps
                                // toward it in every channel; a blend of two
                                // inks on the square steps whichever way the
                                // second ink differs, and never all three
                                // upward at once.
                                if (ddx < -2 || ddy < -2 || ddz < -2) continue;
                                const long d2 = ddx * ddx + ddy * ddy + ddz * ddz;
                                if (d2 > showLo && d2 <= showHi) obs[(size_t)y * cell + x] = 1;
                                continue;
                            }
                            if (!obs[(size_t)y * cell + x]) continue;
                            const double px = p[0] - bg[0], py = p[1] - bg[1],
                                          pz = p[2] - bg[2];
                            const double dx = aCol[0] - bg[0], dy = aCol[1] - bg[1],
                                         dz = aCol[2] - bg[2];
                            const double dd = dx * dx + dy * dy + dz * dz;
                            const double a = (px * dx + py * dy + pz * dz) / dd;
                            if (a < 0.08 || a > 1.08) continue;
                            const double rx = px - a * dx, ry = py - a * dy, rz = pz - a * dz;
                            // A dark band's line passes within twenty-five
                            // levels of a piece's own mid gray, so only what
                            // lies on it almost exactly -- the ramp itself,
                            // which is a straight blend -- is called band.
                            // A light band's line clears the grays by
                            // fifty.
                            if (rx * rx + ry * ry + rz * rz <= 256.0)
                                obs[(size_t)y * cell + x] = 0;
                        }
                } else {
                // No strip to name the band from: the colours caught above
                // are all there is.
                // The loudest colours: a core is flat and repeats hundreds
                // of times, each step of the ramp only a handful.
                int ord[16], nOrd = 0;
                for (int k = 0; k < inkN; ++k) ord[nOrd++] = k;
                for (int a = 0; a < nOrd; ++a)
                    for (int b = a + 1; b < nOrd; ++b)
                        if (inkCnt[ord[b]] > inkCnt[ord[a]]) {
                            const int t = ord[a];
                            ord[a] = ord[b];
                            ord[b] = t;
                        }
                if (nOrd > 6) nOrd = 6;
                for (int y = inset; y < cell - inset; ++y)
                    for (int x = inset; x < cell - inset; ++x) {
                        if (!obs[(size_t)y * cell + x]) continue;
                        const uint8_t* p = img.px(x0 + x, y0 + y);
                        const double px = p[0] - bg[0], py = p[1] - bg[1],
                                      pz = p[2] - bg[2];
                        for (int t = 0; t < nOrd; ++t) {
                            const int* s = inkCol[ord[t]];
                            const double dx = s[0] - bg[0], dy = s[1] - bg[1],
                                         dz = s[2] - bg[2];
                            const double dd = dx * dx + dy * dy + dz * dz;
                            if (dd < 900.0) continue;  // too near the square to name anything
                            const double a = (px * dx + py * dy + pz * dz) / dd;
                            if (a < 0.08 || a > 1.08) continue;
                            const double rx = px - a * dx, ry = py - a * dy, rz = pz - a * dz;
                            if (rx * rx + ry * ry + rz * rz <= 625.0) {
                                obs[(size_t)y * cell + x] = 0;
                                break;
                            }
                        }
                    }
                }
            }
            // Then the coordinate letter goes with everything else too small
            // to be the piece, so a corner glyph cannot make the whole back
            // rank read as something else.
            dropped = keepBiggestBlob(obs, cell, blobComp, blobWork);
            for (int y = inset; y < cell - inset; ++y)
                for (int x = inset; x < cell - inset; ++x) {
                    if (!obs[(size_t)y * cell + x]) continue;
                    const uint8_t* p = img.px(x0 + x, y0 + y);
                    ++obsRows[y];
                    ++mask;
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                    cr += p[0];
                    cg += p[1];
                    cb += p[2];
                }

            const int sq = squareOfImage(r, c, blackAtBottom);
            const float frac = mask / (float)area;
            Piece piece = 0;
            float conf = 1.f;
            double best = -1.0;  // the winning template's score
            // Best score each *type* reached, whatever colour it was drawn
            // in, and how it got there: the refusal line prints both.
            double typeBest[7] = {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
            int typeIdx[7] = {-1, -1, -1, -1, -1, -1, -1};
            Terms typeTr[7] = {};
            int typeBw[7] = {0, 0, 0, 0, 0, 0, 0}, typeBh[7] = {0, 0, 0, 0, 0, 0, 0};
            ShapeStat fs;  // the mask's own geometry, measured when there is one

            if (mask > 0) {
                const int ext = std::max(maxY - minY + 1, maxX - minX + 1);
                // A shadow lying under a piece is wide but flat; a piece spans
                // a good part of the square even when it barely contrasts.
                const bool isPiece = frac >= 0.07f || (ext * 10 >= cell * 3 && frac >= 0.03f);
                if (isPiece) {
                    const std::vector<Tpl>& set = tpl.t[(r + c) & 1];
                    const int obsCol[3] = {(int)(cr / mask), (int)(cg / mask), (int)(cb / mask)};
                    // What this square's silhouette measures like, for the
                    // terms that care about geometry rather than art style.
                    fs = shapeStat(obs, obsRows, cell, minX, minY, maxX, maxY);
                    // The square's shape on the same grid the templates are
                    // drawn on: every measurement below compares grids, so
                    // how big either one was drawn in pixels never enters it.
                    fillGrid(obsG, obs.data(), cell, fs, obsGcx, obsGcy);
                    dilateGrid(obsG, obsD);
                    const bool dumpThis = gDump && sqName(sq) == gDump;
                    if (dumpThis) {
                        gDumpObs = obsG;
                        gDumpSet = &set;
                        gDumpName = sqName(sq);
                        gDumpCx = obsGcx;
                        gDumpCy = obsGcy;
                        gDumpTop = fs.bw ? (double)fs.top / fs.bw : 0.0;
                    }
                    for (size_t i = 0; i < set.size(); ++i) {
                        Terms tr;
                        const double sc =
                            scoreTemplate(set[i], obsG.data(), obsD.data(), cell, obsCol, fs,
                                          &tr);
                        const Piece p = set[i].piece;
                        const int ti = (int)typeOf(p);
                        if (sc > typeBest[ti]) {
                            typeBest[ti] = sc;
                            typeIdx[ti] = (int)i;
                            typeTr[ti] = tr;
                            typeBw[ti] = set[i].st.bw;
                            typeBh[ti] = set[i].st.bh;
                        }
                        if (sc > best) {
                            best = sc;
                            piece = p;
                        }
                    }
                    if (dumpThis)
                        for (int t = PAWN; t <= KING; ++t) {
                            gDumpIdx[t] = typeIdx[t];
                            gDumpSc[t] = typeBest[t];
                            gDumpTr[t] = typeTr[t];
                        }
                    conf = (float)clampi((int)(((best - 0.35) / 0.40) * 1000), 0, 1000) / 1000.f;
                } else {
                    conf = 0.6f;  // something was there, just not a piece
                }
            }

            // The note the refusal line prints for this square.
            SqNote& nt = note[sq];
            nt.frac = (int)(((long long)(mask + dropped) * 100) / area);
            if (dropped > 0) nt.drop = (dropped * 100) / (dropped + mask);
            if (!isEmpty(piece)) {
                nt.got = pieceChar(piece);
                nt.gtype = (char)typeOf(piece);
                nt.sc = (int)(best * 100.0 + 0.5);
                nt.bw = fs.bw;
                nt.bh = fs.bh;
                for (int t = PAWN; t <= KING; ++t)
                    if (typeBest[t] >= 0.0) {
                        nt.typeSc[t] = (int)(typeBest[t] * 100.0 + 0.5);
                        nt.typeTr[t] = typeTr[t];
                        nt.tBw[t] = typeBw[t];
                        nt.tBh[t] = typeBh[t];
                    }
            } else if (mask > 0) {
                nt.got = '?';  // something was there, but not a piece
            }

            confSum += conf;
            rr.board[sq] = piece;
        }
    }

    // A pawn on the first or last rank cannot happen in a real game: it is a
    // misread, and the engine would push it straight off the board.
    rr.kings = 0;
    for (int sq = 0; sq < 64; ++sq) {
        if (isEmpty(rr.board[sq])) continue;
        if (typeOf(rr.board[sq]) == PAWN && (rankOf(sq) == 0 || rankOf(sq) == 7)) rr.board[sq] = 0;
        if (typeOf(rr.board[sq]) == KING) ++rr.kings;
    }

    // The FEN comes out either way: when the read is refused for lack of
    // kings, it is the only way to see what the reader thought it was looking
    // at, and "not a chess position" on its own never says which square went
    // wrong.
    rr.confidence = (float)(confSum / 64.0);
    rr.fen = buildFen(rr.board, side, prev);
    // Not exactly two kings means this was never a chess position, and the
    // engine must not be handed it -- `kings` says why. A third king is a
    // queen that read as a king (the two crowns differ by pixels), which is
    // a wrong position, not a lenient one.
    rr.ok = rr.kings == 2;
    // ...and the squares, rank by rank, whenever the answer is refused or
    // someone asked for them outright (`wantDetail`): this is a diagnosis, not
    // a report, and a read that came out a chess position has no use for it
    // -- except when it is wrong, which is the one thing a refusal never is.
    // `a2 B69~P66~R61^7` reads: a2 came out a bishop at 0.69, with a pawn the
    // best of its kind at 0.66 and a rook at 0.61, after 7% of its pixels left
    // with a smaller blob -- the shape a coordinate letter makes when it sits
    // in the corner of an edge square. Every piece type is listed, because the
    // question a misread raises is not what won but what the *right* answer
    // scored: losing by one point is a tuning problem, losing by thirty is
    // something else entirely. `h3 ?7` is a square where something was there
    // at 7% of the square and none of it was a piece.
    if (wantDetail || !rr.ok) {
        for (int rk = 7; rk >= 0; --rk) {
            std::string line = "rank " + std::to_string(rk + 1) + ":";
            for (int f = 0; f < 8; ++f) {
                const SqNote& nt = note[makeSquare(f, rk)];
                if (nt.got == 0) continue;
                line += ' ';
                // The square is named, not just counted: a skipped square
                // shifts everything after it, and with empty squares left out
                // there is no other way to say which one is missing.
                line += squareName(makeSquare(f, rk));
                line += nt.got;
                line += std::to_string(nt.got == '?' ? nt.frac : nt.sc);
                if (nt.gtype > 0) {
                    // Every other type, best first: who came second is not
                    // the point, how far behind the right answer finished is.
                    // Thirty-five is the floor -- a type under that was never
                    // in the running, and naming it would only say that it
                    // was not.
                    int used = 1 << nt.gtype;
                    for (int pass = 0; pass < 4; ++pass) {
                        int bt = 0;
                        for (int t = PAWN; t <= KING; ++t)
                            if (!(used & (1 << t)) && nt.typeSc[t] >= 35 &&
                                (bt == 0 || nt.typeSc[t] > nt.typeSc[bt]))
                                bt = t;
                        if (bt == 0) break;
                        used |= 1 << bt;
                        line += '~';
                        line += pieceChar(makePiece((PieceType)bt, WHITE));
                        line += std::to_string(nt.typeSc[bt]);
                    }
                }
                if (nt.drop > 0) {
                    line += '^';
                    line += std::to_string(nt.drop);
                }
            }
            if (line.size() > 7) rr.detail.push_back(line);  // more than "rank 8:"
        }

        // Which squares earn their six measurements: every one decided by
        // fewer than five points, because a misread lives in a narrow win and
        // a square won by thirty has nothing to explain -- plus the square
        // nobody scored well, which is the other way a square goes wrong.
        // Closest first, capped, so a board where everything is close does
        // not print a novel. The margin is the gap to the best other type,
        // which is the number that says how close a wrong answer came.
        auto num = [](double v) { return std::to_string((int)(v * 100.0 + 0.5)); };
        int worstSq = -1, worstSc = 1000;
        std::vector<std::pair<int, int> > narrow;  // margin, square
        for (int sq = 0; sq < 64; ++sq) {
            const SqNote& nt = note[sq];
            if (nt.gtype == 0) continue;
            if (nt.sc < worstSc) {
                worstSc = nt.sc;
                worstSq = sq;
            }
            int runner = 0;
            for (int t = PAWN; t <= KING; ++t)
                if (t != nt.gtype && nt.typeSc[t] > runner) runner = nt.typeSc[t];
            if (nt.sc - runner < 5) narrow.push_back(std::make_pair(nt.sc - runner, sq));
        }
        std::vector<int> picks;
        if (wantDetail) {
            // Someone asked outright: every square with a piece on it, in
            // board order. The narrow ones are a good guess at what went
            // wrong, but a *confident* mistake -- a queen read as a knight
            // by fifteen points -- is never narrow, and the guess cannot
            // know which answer is the wrong one without ground truth.
            for (int sq = 0; sq < 64; ++sq)
                if (note[sq].gtype != 0) picks.push_back(sq);
        } else {
            std::sort(narrow.begin(), narrow.end());
            if (worstSq >= 0) picks.push_back(worstSq);
            for (size_t i = 0; i < narrow.size() && picks.size() < 12; ++i) {
                bool have = false;
                for (size_t k = 0; k < picks.size(); ++k)
                    if (picks[k] == narrow[i].second) have = true;
                if (!have) picks.push_back(narrow[i].second);
            }
        }
        for (size_t pi = 0; pi < picks.size(); ++pi) {
            const int sq = picks[pi];
            const SqNote& nt = note[sq];
            std::string line =
                squareName(sq) + " o" + std::to_string(nt.bw) + "x" + std::to_string(nt.bh) + ":";
            int used = 1 << nt.gtype;
            for (int n = 0; n < 4; ++n) {
                int bt = nt.gtype;
                if (n > 0) {
                    bt = 0;
                    for (int t = PAWN; t <= KING; ++t)
                        if (!(used & (1 << t)) && nt.typeSc[t] >= 35 &&
                            (bt == 0 || nt.typeSc[t] > nt.typeSc[bt]))
                            bt = t;
                }
                if (bt == 0) break;
                used |= 1 << bt;
                if (n > 0) line += ' ';
                line += pieceChar(makePiece((PieceType)bt, WHITE));
                line += std::to_string(nt.typeSc[bt]);
                const Terms& tr = nt.typeTr[bt];
                // iou, profile, colour, size, top, lopsided: the score above
                // is a weighted sum of these six, and the sum never says
                // which weight was decisive. `b` is that template's own box,
                // against the `o` box measured off the square itself -- the
                // two that `s` compares, before any weighting touched them.
                line += "[i" + num(tr.iou) + " p" + num(tr.prof) + " c" + num(tr.col) + " s" +
                        num(tr.size) + " t" + num(tr.top) + " l" + num(tr.lop) + " b" +
                        std::to_string(nt.tBw[bt]) + "x" + std::to_string(nt.tBh[bt]) + "]";
            }
            rr.terms.push_back(line);
        }
    }

    // The OV_GRID picture, appended last so no block above can clear it: the
    // square's own grid first, then the best template of every piece type
    // beside it, all on the same frame the overlap compares them on. The
    // header carries each column's six terms and both pictures' centroids,
    // which say how far the mass sits from the box centre -- recomputing a
    // comparison the totals cannot explain starts from these.
    if (gDumpSet && !gDumpName.empty()) {
        std::string head = "grid " + gDumpName + ": observed cx" + std::to_string(gDumpCx) +
                           " cy" + std::to_string(gDumpCy);
        const Tpl* picks[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
        for (int t = PAWN; t <= KING; ++t) {
            if (gDumpIdx[t] < 0) continue;
            picks[t] = &(*gDumpSet)[gDumpIdx[t]];
            head += " | ";
            head += pieceChar(makePiece((PieceType)t, WHITE));
            head += std::to_string((int)(gDumpSc[t] * 100.0 + 0.5));
            char buf[96];
            const Terms& tr = gDumpTr[t];
            std::snprintf(buf, sizeof buf, "[i%2d p%2d c%2d s%2d t%2d l%2d T%.3f cx%d cy%d]",
                          (int)(tr.iou * 100 + 0.5), (int)(tr.prof * 100 + 0.5),
                          (int)(tr.col * 100 + 0.5), (int)(tr.size * 100 + 0.5),
                          (int)(tr.top * 100 + 0.5), (int)(tr.lop * 100 + 0.5),
                          picks[t]->st.bw ? (double)picks[t]->st.top / picks[t]->st.bw : 0.0,
                          picks[t]->gcx, picks[t]->gcy);
            head += buf;
        }
        char obuf[48];
        std::snprintf(obuf, sizeof obuf, " | obsT %.3f", gDumpTop);
        head += obuf;
        rr.terms.push_back(head);
        for (int gy = 0; gy < GRID; ++gy) {
            std::string row;
            for (int gx = 0; gx < GRID; ++gx)
                row += gDumpObs[(size_t)gy * GRID + gx] ? '#' : '.';
            for (int t = PAWN; t <= KING; ++t) {
                if (!picks[t]) continue;
                row += " | ";
                for (int gx = 0; gx < GRID; ++gx)
                    row += picks[t]->g[(size_t)gy * GRID + gx] ? '#' : '.';
            }
            rr.terms.push_back(row);
        }
    }
    return rr;
}

ArrowGeom makeArrow(const BoardRect& b, bool blackAtBottom, int from, int to, int thick,
                    SDL_Color color) {
    ArrowGeom a;
    if (!b.ok || b.cell <= 0 || from < 0 || from > 63 || to < 0 || to > 63) return a;
    auto centre = [&](int sq, float& px, float& py) {
        const int f = fileOf(sq), rk = rankOf(sq);
        const int col = blackAtBottom ? 7 - f : f;
        const int row = blackAtBottom ? rk : 7 - rk;
        px = b.x + (col + 0.5f) * b.cell;
        py = b.y + (row + 0.5f) * b.cell;
    };
    float x1, y1, x2, y2;
    centre(from, x1, y1);
    centre(to, x2, y2);

    const float dx = x2 - x1, dy = y2 - y1;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 4.f) return a;
    const float ux = dx / len, uy = dy / len;
    const float headLen = std::min(len * 0.5f, thick * 2.6f);
    const float headW = thick * 2.0f;
    const float bx = x2 - ux * headLen, by = y2 - uy * headLen;
    const float ox = -uy * thick * 0.5f, oy = ux * thick * 0.5f;
    auto put = [](SDL_Point& p, float X, float Y) {
        p.x = (int)(X + (X >= 0 ? 0.5f : -0.5f));
        p.y = (int)(Y + (Y >= 0 ? 0.5f : -0.5f));
    };
    put(a.shaft[0], x1 + ox, y1 + oy);
    put(a.shaft[1], bx + ox, by + oy);
    put(a.shaft[2], bx - ox, by - oy);
    put(a.shaft[3], x1 - ox, y1 - oy);
    put(a.head[0], x2 - ux * thick * 0.3f, y2 - uy * thick * 0.3f);
    put(a.head[1], bx - uy * headW * 0.5f, by + ux * headW * 0.5f);
    put(a.head[2], bx + uy * headW * 0.5f, by - ux * headW * 0.5f);

    int minx = a.shaft[0].x, maxx = a.shaft[0].x;
    int miny = a.shaft[0].y, maxy = a.shaft[0].y;
    for (int i = 1; i < 4; ++i) {
        minx = std::min(minx, a.shaft[i].x);
        maxx = std::max(maxx, a.shaft[i].x);
        miny = std::min(miny, a.shaft[i].y);
        maxy = std::max(maxy, a.shaft[i].y);
    }
    for (int i = 0; i < 3; ++i) {
        minx = std::min(minx, a.head[i].x);
        maxx = std::max(maxx, a.head[i].x);
        miny = std::min(miny, a.head[i].y);
        maxy = std::max(maxy, a.head[i].y);
    }
    a.bbox[0] = minx - 1;
    a.bbox[1] = miny - 1;
    a.bbox[2] = maxx - minx + 3;
    a.bbox[3] = maxy - miny + 3;
    a.on = true;
    a.color = color;
    return a;
}

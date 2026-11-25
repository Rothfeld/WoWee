#include "rendering/x11_shm_presenter.hpp"

#include "core/logger.hpp"

#if !defined(WOWEE_X11_SHM_PRESENT)

// Built without X11 and its MIT-SHM extension: no window ever qualifies.
namespace wowee::rendering {
struct X11ShmPresenter::State {};
std::unique_ptr<X11ShmPresenter> X11ShmPresenter::create(SDL_Window*, std::string& reason) {
    reason = "this build has no X11 shared-memory support";
    return nullptr;
}
X11ShmPresenter::~X11ShmPresenter() = default;
void X11ShmPresenter::releaseSegment() {}
void X11ShmPresenter::waitForCompletion() {}
bool X11ShmPresenter::resize(uint32_t, uint32_t) { return false; }
uint8_t* X11ShmPresenter::pixels() { return nullptr; }
void X11ShmPresenter::put() {}
bool X11ShmPresenter::readWindow(std::vector<uint8_t>&) { return false; }
} // namespace wowee::rendering

#else

#include <SDL3/SDL.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <cerrno>
#include <cstring>

namespace wowee::rendering {

struct X11ShmPresenter::State {
    Display* display = nullptr;
    ::Window window = 0;
    Visual* visual = nullptr;
    int depth = 0;
    GC gc = nullptr;
    XImage* image = nullptr;
    XShmSegmentInfo segment{};
    bool attached = false;
    int completionEvent = 0;
};

namespace {

// Xlib reports a failed request through one handler for the whole process.
// Attaching a segment is the one request here that can fail for a reason
// worth answering (a server in another IPC namespace), so the handler is
// swapped for the length of that request and its XSync.
bool gAttachFailed = false;
int noteAttachError(Display*, XErrorEvent*) {
    gAttachFailed = true;
    return 0;
}

bool hasExtension(Display* display, const char* name) {
    int opcode = 0, event = 0, error = 0;
    return XQueryExtension(display, name, &opcode, &event, &error) != 0;
}

} // namespace

std::unique_ptr<X11ShmPresenter> X11ShmPresenter::create(SDL_Window* window, std::string& reason) {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || std::strcmp(driver, "x11") != 0) {
        reason = std::string("the window is not an X11 window (SDL video driver: ") + (driver ? driver : "none") + ")";
        return nullptr;
    }
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    auto* sdlDisplay = static_cast<Display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    const auto xid = static_cast<::Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    if (!sdlDisplay || xid == 0) {
        reason = "SDL does not give the window's X11 display and id";
        return nullptr;
    }

    std::unique_ptr<X11ShmPresenter> presenter(new X11ShmPresenter());
    presenter->state_ = std::make_unique<State>();
    State& s = *presenter->state_;
    s.display = XOpenDisplay(XDisplayString(sdlDisplay));
    if (!s.display) {
        reason = std::string("a second connection to display ") + XDisplayString(sdlDisplay) + " could not be opened";
        return nullptr;
    }
    s.window = xid;

    if (hasExtension(s.display, "DRI3")) {
        reason = "the X server has DRI3 and takes GPU images directly";
        return nullptr;
    }
    if (hasExtension(s.display, "NV-GLX")) {
        reason = "the X server has NV-GLX and takes GPU images directly";
        return nullptr;
    }
    if (!XShmQueryExtension(s.display)) {
        reason = "the X server has no MIT-SHM";
        return nullptr;
    }
    s.completionEvent = XShmGetEventBase(s.display) + ShmCompletion;

    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(s.display, s.window, &attributes)) {
        reason = "the window's attributes cannot be read";
        return nullptr;
    }
    s.visual = attributes.visual;
    s.depth = attributes.depth;
    // One row of a probe image says how the server lays out a pixel of this
    // visual; nothing is attached or sent for it.
    XShmSegmentInfo probeSegment{};
    XImage* probe = XShmCreateImage(s.display, s.visual, static_cast<unsigned>(s.depth), ZPixmap,
                                    nullptr, &probeSegment, 1, 1);
    if (!probe) {
        reason = "the X server does not offer a shared image for the window's visual";
        return nullptr;
    }
    const bool bgrx = probe->bits_per_pixel == 32 && probe->byte_order == LSBFirst &&
                      probe->red_mask == 0xff0000UL && probe->green_mask == 0x00ff00UL &&
                      probe->blue_mask == 0x0000ffUL;
    if (!bgrx) {
        reason = "the window's pixels are not 32-bit blue-green-red (bits " +
                 std::to_string(probe->bits_per_pixel) + ", red mask " + std::to_string(probe->red_mask) + ")";
        XDestroyImage(probe);
        return nullptr;
    }
    XDestroyImage(probe);

    s.gc = XCreateGC(s.display, s.window, 0, nullptr);
    if (!s.gc) {
        reason = "no graphics context for the window";
        return nullptr;
    }
    return presenter;
}

X11ShmPresenter::~X11ShmPresenter() {
    if (!state_) return;
    releaseSegment();
    if (state_->gc) XFreeGC(state_->display, state_->gc);
    if (state_->display) XCloseDisplay(state_->display);
}

void X11ShmPresenter::releaseSegment() {
    State& s = *state_;
    if (putPending_) waitForCompletion();
    if (s.attached) {
        XShmDetach(s.display, &s.segment);
        XSync(s.display, False);
        s.attached = false;
    }
    if (s.image) {
        // The image does not own the segment's memory.
        s.image->data = nullptr;
        XDestroyImage(s.image);
        s.image = nullptr;
    }
    if (s.segment.shmaddr && s.segment.shmaddr != reinterpret_cast<char*>(-1)) {
        shmdt(s.segment.shmaddr);
    }
    s.segment = XShmSegmentInfo{};
    width_ = 0;
    height_ = 0;
}

bool X11ShmPresenter::resize(uint32_t width, uint32_t height) {
    State& s = *state_;
    if (width == width_ && height == height_ && s.attached) return true;
    releaseSegment();
    if (width == 0 || height == 0) return false;

    s.image = XShmCreateImage(s.display, s.visual, static_cast<unsigned>(s.depth), ZPixmap,
                              nullptr, &s.segment, width, height);
    if (!s.image) {
        LOG_ERROR("X11 shared-memory present: no shared image of ", width, "x", height);
        return false;
    }
    if (s.image->bytes_per_line != static_cast<int>(width * 4)) {
        LOG_ERROR("X11 shared-memory present: rows of ", s.image->bytes_per_line,
                  " bytes for a width of ", width, ", expected ", width * 4);
        releaseSegment();
        return false;
    }
    const size_t bytes = static_cast<size_t>(s.image->bytes_per_line) * height;
    s.segment.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
    if (s.segment.shmid < 0) {
        LOG_ERROR("X11 shared-memory present: shmget of ", bytes, " bytes failed: ", std::strerror(errno));
        releaseSegment();
        return false;
    }
    s.segment.shmaddr = static_cast<char*>(shmat(s.segment.shmid, nullptr, 0));
    if (s.segment.shmaddr == reinterpret_cast<char*>(-1)) {
        LOG_ERROR("X11 shared-memory present: shmat failed: ", std::strerror(errno));
        shmctl(s.segment.shmid, IPC_RMID, nullptr);
        releaseSegment();
        return false;
    }
    s.segment.readOnly = True;
    s.image->data = s.segment.shmaddr;

    gAttachFailed = false;
    XSync(s.display, False);
    XErrorHandler previous = XSetErrorHandler(noteAttachError);
    XShmAttach(s.display, &s.segment);
    XSync(s.display, False);
    XSetErrorHandler(previous);
    // Marked for removal now that both sides have it (or the server refused):
    // the segment goes when the last of them detaches, also after a crash.
    shmctl(s.segment.shmid, IPC_RMID, nullptr);
    if (gAttachFailed) {
        LOG_ERROR("X11 shared-memory present: the X server could not attach the segment "
                  "(a server in another IPC namespace or on another machine)");
        releaseSegment();
        return false;
    }
    s.attached = true;
    width_ = width;
    height_ = height;
    return true;
}

void X11ShmPresenter::waitForCompletion() {
    State& s = *state_;
    while (putPending_) {
        XEvent event;
        XNextEvent(s.display, &event);
        if (event.type == s.completionEvent) putPending_ = false;
    }
}

uint8_t* X11ShmPresenter::pixels() {
    if (putPending_) waitForCompletion();
    return reinterpret_cast<uint8_t*>(state_->segment.shmaddr);
}

void X11ShmPresenter::put() {
    State& s = *state_;
    if (putPending_) waitForCompletion();
    XShmPutImage(s.display, s.window, s.gc, s.image, 0, 0, 0, 0, width_, height_, True);
    XFlush(s.display);
    putPending_ = true;
}

bool X11ShmPresenter::readWindow(std::vector<uint8_t>& out) {
    State& s = *state_;
    if (putPending_) waitForCompletion();
    XImage* image = XGetImage(s.display, s.window, 0, 0, width_, height_, AllPlanes, ZPixmap);
    if (!image) return false;
    const size_t rowBytes = static_cast<size_t>(width_) * 4;
    out.resize(rowBytes * height_);
    for (uint32_t y = 0; y < height_; ++y) {
        std::memcpy(out.data() + y * rowBytes, image->data + static_cast<size_t>(y) * image->bytes_per_line, rowBytes);
    }
    XDestroyImage(image);
    return true;
}

} // namespace wowee::rendering

#endif // WOWEE_X11_SHM_PRESENT

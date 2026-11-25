#pragma once

// Finished frames into an X11 window through shared memory (MIT-SHM).
//
// A Vulkan driver hands a frame to the X server as a GPU image when the server
// can take one (DRI3, or NVIDIA's own NV-GLX protocol). A server that draws in
// main memory - Xvfb, Xvnc, Xephyr - cannot, and the driver then reads the
// frame back and sends every pixel through the X socket: at 1280x720 that is
// 3.7 MB written per frame, measured at 13 ms of the main thread, most of it
// in the kernel (_experiments/20261125T192530Z.present_nameplate_cost).
//
// The same server does offer MIT-SHM: client and server map one memory
// segment, the client writes the frame into it and the server copies it to
// the window from there. This class is that segment and the request; VkContext
// fills it from its own readback of the swapchain image (vk_context.cpp,
// "Presenting through shared memory").
//
// Own connection to the display, apart from SDL's: the completion events the
// server sends after each put would otherwise sit in SDL's queue.
//
// Linux/X11 only. Main thread only.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

namespace wowee::rendering {

class X11ShmPresenter {
public:
    /// The presenter for `window`, or null with `reason` saying which
    /// requirement is not met. The requirements:
    ///  - the window is an X11 window (SDL's x11 video driver);
    ///  - the server has MIT-SHM and can attach a segment of this process
    ///    (same machine and IPC namespace);
    ///  - the server has neither DRI3 nor NV-GLX, the two ways a GPU image
    ///    reaches it without passing through main memory;
    ///  - the window's pixels are 32 bits, least significant byte first,
    ///    red 0xff0000, green 0xff00, blue 0xff: the bytes of a
    ///    VK_FORMAT_B8G8R8A8 image, so a frame is copied and not converted.
    static std::unique_ptr<X11ShmPresenter> create(SDL_Window* window, std::string& reason);

    ~X11ShmPresenter();
    X11ShmPresenter(const X11ShmPresenter&) = delete;
    X11ShmPresenter& operator=(const X11ShmPresenter&) = delete;

    /// A segment for frames of this size. False, with the reason logged, when
    /// the server cannot attach it.
    bool resize(uint32_t width, uint32_t height);
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }

    /// Where the next frame's bytes go: width*4 bytes a row, rows top to
    /// bottom, no padding. Waits for the server to finish reading the frame
    /// put before, so a frame is never overwritten while it is being copied.
    uint8_t* pixels();

    /// Ask the server to copy the segment to the window. Returns at once; the
    /// copy happens in the server.
    void put();

    /// The window's pixels as the server holds them now, in the layout of
    /// pixels(). For checking a present against the frame it presented; this
    /// one does send every pixel through the socket.
    bool readWindow(std::vector<uint8_t>& out);

private:
    X11ShmPresenter() = default;
    void releaseSegment();
    void waitForCompletion();

    struct State;
    std::unique_ptr<State> state_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool putPending_ = false;
};

} // namespace wowee::rendering

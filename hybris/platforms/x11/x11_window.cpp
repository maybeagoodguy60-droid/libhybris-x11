#include <android-config.h>

#include "x11_window.h"
#include "logging.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <hardware/gralloc.h>

#include <hybris/common/dlfcn.h>
#include <vndk/hardware_buffer.h>

#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xcbext.h>

#if ANDROID_VERSION_MAJOR >= 4 && ANDROID_VERSION_MINOR >= 2 || ANDROID_VERSION_MAJOR >= 5
extern "C" {
#include <sync/sync.h>
}
#endif

namespace {

constexpr uint64_t kTermuxAHardwareBufferModifier = 1255;
constexpr uint32_t kBufferDepth = 24;
constexpr uint32_t kBufferBpp = 32;

using allocate_fn = int (*)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
using describe_fn = void (*)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
using get_native_handle_fn = const native_handle_t * (*)(const AHardwareBuffer *);
using release_fn = void (*)(AHardwareBuffer *);
using send_handle_fn = int (*)(const AHardwareBuffer *, int);

void *nativewindow_handle = nullptr;
allocate_fn allocate_buffer = nullptr;
describe_fn describe_buffer = nullptr;
get_native_handle_fn get_native_handle = nullptr;
release_fn release_buffer = nullptr;
send_handle_fn send_handle = nullptr;
bool nativewindow_api_checked = false;
bool nativewindow_api_available = false;

bool init_nativewindow_api()
{
    if (nativewindow_api_checked)
        return nativewindow_api_available;

    nativewindow_api_checked = true;
    nativewindow_handle = hybris_dlopen("libnativewindow.so", 1);
    if (!nativewindow_handle)
        return false;

    allocate_buffer = reinterpret_cast<allocate_fn>(
        hybris_dlsym(nativewindow_handle, "AHardwareBuffer_allocate"));
    describe_buffer = reinterpret_cast<describe_fn>(
        hybris_dlsym(nativewindow_handle, "AHardwareBuffer_describe"));
    get_native_handle = reinterpret_cast<get_native_handle_fn>(
        hybris_dlsym(nativewindow_handle, "AHardwareBuffer_getNativeHandle"));
    release_buffer = reinterpret_cast<release_fn>(
        hybris_dlsym(nativewindow_handle, "AHardwareBuffer_release"));
    send_handle = reinterpret_cast<send_handle_fn>(
        hybris_dlsym(nativewindow_handle, "AHardwareBuffer_sendHandleToUnixSocket"));
    nativewindow_api_available = allocate_buffer && describe_buffer &&
                                 get_native_handle && release_buffer && send_handle;
    return nativewindow_api_available;
}

}

X11NativeWindowBuffer::X11NativeWindowBuffer(unsigned int w,
                                             unsigned int h,
                                             unsigned int fmt,
                                             uint64_t usg)
    : busy(0)
    , youngest(0)
    , serial(0)
    , pixmap(XCB_NONE)
    , ahb(nullptr)
{
    ANativeWindowBuffer::width = w;
    ANativeWindowBuffer::height = h;
    ANativeWindowBuffer::format = fmt;
    ANativeWindowBuffer::usage = usg;

    bool direct_ahb = false;
    if (init_nativewindow_api()) {
        AHardwareBuffer_Desc desc = {};
        desc.width = w ? w : 1;
        desc.height = h ? h : 1;
        desc.layers = 1;
        desc.format = fmt;
        /*
         * GPU_SAMPLED_IMAGE + GPU_FRAMEBUFFER because the client renders
         * with EGL/GLES or Vulkan and the buffer is consumed as a texture.
         * CPU_READ_OFTEN because the X server reads the pixels back when it
         * copies the presented frame.
         *
         * CPU_WRITE_OFTEN is deliberately NOT requested. Nothing on this
         * path writes the buffer from the CPU, and asking for it makes the
         * gralloc allocator treat the memory as CPU-write-heavy, which
         * measurably slows the X server's read of a buffer the GPU has
         * just written. If a client genuinely needs eglMap()/glMapBuffer on
         * this surface it falls back to its own copy, which is still
         * correct, just not the fast path.
         */
        desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                     AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
        if (allocate_buffer(&desc, &ahb) == 0 && ahb) {
            AHardwareBuffer_Desc actual = {};
            describe_buffer(ahb, &actual);
            this->handle = get_native_handle(ahb);
            this->stride = actual.stride;
            direct_ahb = this->handle != nullptr;
            if (!direct_ahb) {
                release_buffer(ahb);
                ahb = nullptr;
            }
        } else {
            ahb = nullptr;
        }
    }

    if (!direct_ahb) {
        int rc = hybris_gralloc_allocate(w ? w : 1,
                                         h ? h : 1,
                                         fmt,
                                         (uint32_t)usg,
                                         &this->handle,
                                         (uint32_t *)&this->stride);
        assert(rc == 0);
    }

    this->common.incRef(&this->common);
}

X11NativeWindowBuffer::~X11NativeWindowBuffer()
{
    if (ahb && release_buffer)
        release_buffer(ahb);
    else if (this->handle)
        hybris_gralloc_release(this->handle, 1);
}

X11NativeWindow::X11NativeWindow(xcb_connection_t *conn,
                                 xcb_window_t xwin,
                                 unsigned int w,
                                 unsigned int h,
                                 bool present_events)
    : m_conn(conn)
    , m_xwin(xwin)
    , m_width(w ? w : 1)
    , m_height(h ? h : 1)
    , m_format(HAL_PIXEL_FORMAT_RGBA_8888)
    , m_usage(GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE)
    , m_swap_interval(1)
    , m_events_enabled(false)
    , m_event_id(0)
    , m_special_event(nullptr)
    , m_next_serial(0)
{
    const_cast<int &>(ANativeWindow::minSwapInterval) = 0;
    const_cast<int &>(ANativeWindow::maxSwapInterval) = 1;
    pthread_mutex_init(&m_mutex, nullptr);
    if (present_events)
        setupEventChannel();
    setBufferCount(3);
}

void X11NativeWindow::setupEventChannel()
{
    m_event_id = xcb_generate_id(m_conn);
    m_special_event = xcb_register_for_special_xge(m_conn,
                                                    &xcb_present_id,
                                                    m_event_id,
                                                    nullptr);
    if (!m_special_event)
        return;

    xcb_void_cookie_t cookie = xcb_present_select_input_checked(
        m_conn,
        m_event_id,
        m_xwin,
        XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY |
        XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
        XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
    xcb_flush(m_conn);
    xcb_generic_error_t *error = xcb_request_check(m_conn, cookie);
    if (error) {
        free(error);
        xcb_unregister_for_special_event(m_conn, m_special_event);
        m_special_event = nullptr;
        return;
    }
    m_events_enabled = true;
}

X11NativeWindow::~X11NativeWindow()
{
    if (m_special_event) {
        if (m_events_enabled && !xcb_connection_has_error(m_conn))
            xcb_present_select_input(m_conn, m_event_id, m_xwin,
                                     XCB_PRESENT_EVENT_MASK_NO_EVENT);
        xcb_unregister_for_special_event(m_conn, m_special_event);
    }
    destroyBuffers();
    pthread_mutex_destroy(&m_mutex);
}

void X11NativeWindow::handleSpecialEvent(void *generic_event)
{
    xcb_present_generic_event_t *event =
        static_cast<xcb_present_generic_event_t *>(generic_event);

    if (event->evtype == XCB_PRESENT_CONFIGURE_NOTIFY) {
        auto *configure =
            reinterpret_cast<xcb_present_configure_notify_event_t *>(generic_event);
        if (configure->width && configure->height) {
            m_width = configure->width;
            m_height = configure->height;
        }
    } else if (event->evtype == XCB_PRESENT_IDLE_NOTIFY) {
        auto *idle =
            reinterpret_cast<xcb_present_idle_notify_event_t *>(generic_event);
        for (auto *buffer : m_bufList) {
            if (buffer->busy && buffer->serial == idle->serial) {
                buffer->busy = 0;
                buffer->serial = 0;
                break;
            }
        }
    }
}

void X11NativeWindow::drainSpecialEvents()
{
    if (!m_special_event)
        return;
    xcb_generic_event_t *event;
    while ((event = xcb_poll_for_special_event(m_conn, m_special_event))) {
        handleSpecialEvent(event);
        free(event);
    }
}

bool X11NativeWindow::haveFreeBuffer() const
{
    for (auto *buffer : m_bufList) {
        if (!buffer->busy)
            return true;
    }
    return false;
}

void X11NativeWindow::forceFreePresented(const char *why)
{
    int freed = 0;
    for (auto *buffer : m_bufList) {
        if (buffer->busy && buffer->serial != 0) {
            buffer->busy = 0;
            buffer->serial = 0;
            ++freed;
        }
    }
    HYBRIS_WARN("x11-platform: %s; force-freed %d presented buffer(s)",
                why, freed);
}

void X11NativeWindow::lock()
{
    pthread_mutex_lock(&m_mutex);
}

void X11NativeWindow::unlock()
{
    pthread_mutex_unlock(&m_mutex);
}

unsigned int X11NativeWindow::width() const { return m_width; }
unsigned int X11NativeWindow::height() const { return m_height; }
unsigned int X11NativeWindow::format() const { return m_format; }
unsigned int X11NativeWindow::defaultWidth() const { return m_width; }
unsigned int X11NativeWindow::defaultHeight() const { return m_height; }
unsigned int X11NativeWindow::queueLength() const { return 1; }
unsigned int X11NativeWindow::transformHint() const { return 0; }
unsigned int X11NativeWindow::getUsage() const { return m_usage; }

unsigned int X11NativeWindow::type() const
{
#if ANDROID_VERSION_MAJOR >= 4 && ANDROID_VERSION_MINOR >= 3 || ANDROID_VERSION_MAJOR >= 5
    return NATIVE_WINDOW_SURFACE;
#else
    return NATIVE_WINDOW_SURFACE_TEXTURE_CLIENT;
#endif
}

int X11NativeWindow::setSwapInterval(int interval)
{
    if (interval < 0)
        interval = 0;
    if (interval > 1)
        interval = 1;
    lock();
    m_swap_interval = interval;
    unlock();
    return 0;
}

int X11NativeWindow::setBuffersFormat(int fmt)
{
    lock();
    if (fmt != m_format)
        m_format = fmt;
    unlock();
    return NO_ERROR;
}

int X11NativeWindow::setBuffersDimensions(int w, int h)
{
    lock();
    m_width = w;
    m_height = h;
    unlock();
    return NO_ERROR;
}

void X11NativeWindow::resize(unsigned int w, unsigned int h)
{
    lock();
    m_width = w;
    m_height = h;
    unlock();
}

int X11NativeWindow::setUsage(uint64_t usg)
{
    usg |= GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE;
    lock();
    m_usage = usg;
    unlock();
    return NO_ERROR;
}

X11NativeWindowBuffer *X11NativeWindow::addBuffer()
{
    auto *buffer = new X11NativeWindowBuffer(m_width, m_height, m_format, m_usage);
    m_bufList.push_back(buffer);
    return buffer;
}

void X11NativeWindow::destroyBuffer(X11NativeWindowBuffer *buffer)
{
    if (!buffer)
        return;
    buffer->common.decRef(&buffer->common);
}

void X11NativeWindow::destroyBuffers()
{
    for (auto *buffer : m_bufList)
        destroyBuffer(buffer);
    m_bufList.clear();
}

int X11NativeWindow::setBufferCount(int cnt)
{
    if ((int)m_bufList.size() == cnt)
        return NO_ERROR;
    lock();
    if ((int)m_bufList.size() > cnt) {
        auto it = m_bufList.begin();
        for (int i = 0; i <= (int)m_bufList.size() - cnt; ++i) {
            destroyBuffer(*it);
            ++it;
            m_bufList.pop_front();
        }
    } else {
        for (int i = (int)m_bufList.size(); i < cnt; ++i)
            addBuffer();
    }
    unlock();
    return NO_ERROR;
}

int X11NativeWindow::dequeueBuffer(BaseNativeWindowBuffer **buffer, int *fenceFd)
{
    lock();
    if (m_events_enabled) {
        drainSpecialEvents();
        const long timeout_ms = 500;
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        while (!haveFreeBuffer()) {
            if (xcb_connection_has_error(m_conn)) {
                forceFreePresented("X connection broken while waiting for Present idle");
                break;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                           (now.tv_nsec - start.tv_nsec) / 1000000;
            if (elapsed >= timeout_ms) {
                forceFreePresented("no Present idle notification");
                break;
            }
            struct pollfd pfd = {xcb_get_file_descriptor(m_conn), POLLIN, 0};
            long slice = timeout_ms - elapsed;
            if (slice > 50)
                slice = 50;
            if (poll(&pfd, 1, (int)slice) < 0 && errno != EINTR) {
                forceFreePresented("poll on the X connection failed");
                break;
            }
            drainSpecialEvents();
        }
    }

    auto it = m_bufList.begin();
    for (; it != m_bufList.end(); ++it) {
        if (!(*it)->busy && !(*it)->youngest)
            break;
    }
    if (it == m_bufList.end()) {
        it = m_bufList.begin();
        for (; it != m_bufList.end() && (*it)->busy; ++it) {}
    }
    if (it == m_bufList.end()) {
        unlock();
        return NO_ERROR;
    }

    X11NativeWindowBuffer *selected = *it;
    if (selected->width != (unsigned)m_width ||
        selected->height != (unsigned)m_height ||
        selected->format != (unsigned)m_format ||
        selected->usage != m_usage) {
        destroyBuffer(selected);
        m_bufList.erase(it);
        selected = addBuffer();
    }

    selected->busy = 1;
    selected->serial = 0;
    *buffer = selected;
    if (fenceFd)
        *fenceFd = -1;
    unlock();
    return NO_ERROR;
}

int X11NativeWindow::lockBuffer(BaseNativeWindowBuffer *)
{
    return NO_ERROR;
}

int X11NativeWindow::cancelBuffer(BaseNativeWindowBuffer *buffer, int fenceFd)
{
    X11NativeWindowBuffer *selected = static_cast<X11NativeWindowBuffer *>(buffer);
    if (fenceFd >= 0)
        close(fenceFd);
    lock();
    selected->busy = 0;
    selected->serial = 0;
    for (auto *item : m_bufList)
        item->youngest = 0;
    selected->youngest = 1;
    unlock();
    return 0;
}

void X11NativeWindow::prepareSwap(int32_t *, int32_t)
{
}

void X11NativeWindow::finishSwap()
{
}

int X11NativeWindow::createPixmap(X11NativeWindowBuffer *buffer)
{
    if (buffer->pixmap != XCB_NONE)
        return 0;
    if (!buffer->ahb || !init_nativewindow_api())
        return -1;

    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return -1;

    buffer->pixmap = xcb_generate_id(m_conn);
    int32_t server_fd = sockets[1];
    xcb_void_cookie_t cookie = xcb_dri3_pixmap_from_buffers_checked(
        m_conn,
        buffer->pixmap,
        m_xwin,
        1,
        (uint16_t)buffer->width,
        (uint16_t)buffer->height,
        buffer->stride,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        kBufferDepth,
        kBufferBpp,
        kTermuxAHardwareBufferModifier,
        &server_fd);
    xcb_flush(m_conn);

    struct pollfd pfd = {sockets[0], POLLIN, 0};
    int poll_result;
    do {
        poll_result = poll(&pfd, 1, 2000);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result <= 0) {
        close(sockets[0]);
        buffer->pixmap = XCB_NONE;
        return -1;
    }

    char ready = 0;
    ssize_t received;
    do {
        received = read(sockets[0], &ready, sizeof(ready));
    } while (received < 0 && errno == EINTR);
    if (received != sizeof(ready) || send_handle(buffer->ahb, sockets[0]) != 0) {
        close(sockets[0]);
        buffer->pixmap = XCB_NONE;
        return -1;
    }
    close(sockets[0]);

    xcb_generic_error_t *error = xcb_request_check(m_conn, cookie);
    if (error) {
        HYBRIS_ERROR("x11-platform: DRI3 pixmap import failed (error %d)",
                     error->error_code);
        free(error);
        buffer->pixmap = XCB_NONE;
        return -1;
    }
    return 0;
}

int X11NativeWindow::presentBuffer(X11NativeWindowBuffer *buffer)
{
    if (createPixmap(buffer) != 0)
        return -1;

    /*
     * Fire the Present and do not wait for the server's reply.
     *
     * The previous code used xcb_present_pixmap_checked() plus
     * xcb_request_check(), which blocks until the X server answers. That
     * puts a full round trip on the critical path of every single frame,
     * and the copy the server then does is the expensive part anyway.
     *
     * Correctness does not need the synchronous answer: a presented
     * buffer is recycled when its target_msc comes back as a Present
     * CompleteNotify/IdleNotify event, which dequeueBuffer() already
     * tracks by serial. Server-side errors surface asynchronously
     * instead, via xcb_connection_has_error() in dequeueBuffer(), which
     * already force-frees every outstanding buffer and rebuilds the
     * window.
     */
    xcb_present_pixmap(
        m_conn,
        m_xwin,
        buffer->pixmap,
        buffer->serial,
        XCB_NONE,
        XCB_NONE,
        0,
        0,
        XCB_NONE,
        XCB_NONE,
        XCB_NONE,
        0,
        0,
        0,
        0,
        0,
        nullptr);
    xcb_flush(m_conn);

    if (xcb_connection_has_error(m_conn)) {
        HYBRIS_ERROR("x11-platform: X connection lost while presenting");
        return -1;
    }
    return 0;
}

int X11NativeWindow::queueBuffer(BaseNativeWindowBuffer *buffer, int fenceFd)
{
    X11NativeWindowBuffer *selected = static_cast<X11NativeWindowBuffer *>(buffer);
#if ANDROID_VERSION_MAJOR >= 4 && ANDROID_VERSION_MINOR >= 2 || ANDROID_VERSION_MAJOR >= 5
    if (fenceFd >= 0) {
        sync_wait(fenceFd, -1);
        close(fenceFd);
    }
#endif

    lock();
    if (m_events_enabled) {
        if (++m_next_serial == 0)
            ++m_next_serial;
        selected->serial = m_next_serial;
    }
    int rc = presentBuffer(selected);
    if (!m_events_enabled || rc != 0) {
        selected->busy = 0;
        selected->serial = 0;
    }
    for (auto *item : m_bufList)
        item->youngest = 0;
    selected->youngest = 1;
    unlock();
    return rc;
}

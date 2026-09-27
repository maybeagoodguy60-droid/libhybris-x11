#ifndef LIBHYBRIS_X11_WINDOW_H
#define LIBHYBRIS_X11_WINDOW_H

#include "nativewindowbase.h"

#include <hybris/gralloc/gralloc.h>

#include <android/hardware_buffer.h>

#include <pthread.h>

#include <list>

extern "C" {
#include <xcb/xcb.h>
}

class X11NativeWindowBuffer : public BaseNativeWindowBuffer
{
public:
    X11NativeWindowBuffer(unsigned int width,
                          unsigned int height,
                          unsigned int format,
                          uint64_t usage);
    ~X11NativeWindowBuffer();

    int busy;
    int youngest;
    uint32_t serial;
    xcb_pixmap_t pixmap;
    AHardwareBuffer *ahb;
};

class X11NativeWindow : public BaseNativeWindow
{
public:
    X11NativeWindow(xcb_connection_t *conn,
                    xcb_window_t xwin,
                    unsigned int width,
                    unsigned int height,
                    bool present_events);
    ~X11NativeWindow();

    void prepareSwap(int32_t *damage_rects, int32_t damage_n_rects);
    void finishSwap();
    void resize(unsigned int width, unsigned int height);

    virtual int setSwapInterval(int interval);

protected:
    virtual int dequeueBuffer(BaseNativeWindowBuffer **buffer, int *fenceFd);
    virtual int lockBuffer(BaseNativeWindowBuffer *buffer);
    virtual int queueBuffer(BaseNativeWindowBuffer *buffer, int fenceFd);
    virtual int cancelBuffer(BaseNativeWindowBuffer *buffer, int fenceFd);
    virtual unsigned int type() const;
    virtual unsigned int width() const;
    virtual unsigned int height() const;
    virtual unsigned int format() const;
    virtual unsigned int defaultWidth() const;
    virtual unsigned int defaultHeight() const;
    virtual unsigned int queueLength() const;
    virtual unsigned int transformHint() const;
    virtual unsigned int getUsage() const;
    virtual int setUsage(uint64_t usage);
    virtual int setBuffersFormat(int format);
    virtual int setBuffersDimensions(int width, int height);
    virtual int setBufferCount(int cnt);

private:
    void lock();
    void unlock();
    X11NativeWindowBuffer *addBuffer();
    void destroyBuffer(X11NativeWindowBuffer *buffer);
    void destroyBuffers();
    int createPixmap(X11NativeWindowBuffer *buffer);
    int presentBuffer(X11NativeWindowBuffer *buffer);
    void setupEventChannel();
    void handleSpecialEvent(void *event);
    void drainSpecialEvents();
    bool haveFreeBuffer() const;
    void forceFreePresented(const char *why);

    xcb_connection_t *m_conn;
    xcb_window_t m_xwin;

    std::list<X11NativeWindowBuffer *> m_bufList;

    int m_width;
    int m_height;
    int m_format;
    uint64_t m_usage;
    int m_swap_interval;

    bool m_events_enabled;
    uint32_t m_event_id;
    xcb_special_event_t *m_special_event;
    uint32_t m_next_serial;

    pthread_mutex_t m_mutex;
};

#endif

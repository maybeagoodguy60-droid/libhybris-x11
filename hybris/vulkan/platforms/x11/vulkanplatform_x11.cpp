/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * libhybris Vulkan platform plugin for X11/XCB on servers with DRI3 and
 * Present support.
 *
 * The Android Vulkan driver only knows how to present to an ANativeWindow.
 * This plugin therefore advertises the native X11/XCB WSI entry points and
 * rewrites them onto VK_KHR_android_surface, backed by the same
 * DRI3/Present AHardwareBuffer transport used by the EGL X11 platform.
 */

#include <android-config.h>
#include <ws.h>
#include <vulkanhybris.h>

#include <assert.h>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <vulkanplatformcommon.h>
}

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xcbext.h>

#include <hybris/gralloc/gralloc.h>
#include <hybris/common/binding.h>

#include "logging.h"
#include "x11_window.h"

struct X11VulkanSurface {
    X11NativeWindow *window;
    /* Only set when the surface was created through the Xlib entry point and
     * we opened the Display connection ourselves. */
    Display *owned_xdpy;
};

static bool init_done = false;

/* Keep track of active Vulkan window surfaces */
static std::map<VkSurfaceKHR, X11VulkanSurface *> _surface_window_map;

static VkResult (*_vkCreateAndroidSurfaceKHR)(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) = NULL;
static PFN_vkVoidFunction (*_vkDestroySurfaceKHR)(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator) = NULL;
static VkResult (*_vkEnumerateInstanceExtensionProperties)(const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties) = NULL;
static VkResult (*_vkCreateInstance)(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) = NULL;
static PFN_vkVoidFunction (*_vkGetInstanceProcAddr)(VkInstance instance, const char *pName) = NULL;

extern "C" void x11ws_init_module(struct ws_vulkan_interface *vulkan_iface)
{
    if (init_done)
        return;
    hybris_gralloc_initialize(0);
    vulkanplatformcommon_init(vulkan_iface);
    init_done = true;
}

static bool x11_surface_has_mapping(VkSurfaceKHR surface)
{
    return _surface_window_map.find(surface) != _surface_window_map.end();
}

static void x11_surface_push_mapping(VkSurfaceKHR surface, X11VulkanSurface *xsurface)
{
    assert(!x11_surface_has_mapping(surface));
    _surface_window_map[surface] = xsurface;
}

static X11VulkanSurface *x11_surface_pop_mapping(VkSurfaceKHR surface)
{
    std::map<VkSurfaceKHR, X11VulkanSurface *>::iterator it;
    it = _surface_window_map.find(surface);

    assert(it != _surface_window_map.end());

    X11VulkanSurface *result = it->second;
    _surface_window_map.erase(it);
    return result;
}

static X11VulkanSurface *x11_surface_get_mapping(VkSurfaceKHR surface)
{
    std::map<VkSurfaceKHR, X11VulkanSurface *>::iterator it;
    it = _surface_window_map.find(surface);
    if (it == _surface_window_map.end())
        return NULL;
    return it->second;
}

static bool x11_dri3_present_available(xcb_connection_t *conn)
{
    const xcb_query_extension_reply_t *dri3 =
        xcb_get_extension_data(conn, &xcb_dri3_id);
    const xcb_query_extension_reply_t *present =
        xcb_get_extension_data(conn, &xcb_present_id);
    if (!dri3 || !dri3->present || !present || !present->present) {
        HYBRIS_ERROR("x11-vulkan: DRI3 and Present are required");
        return false;
    }

    xcb_dri3_query_version_reply_t *dri3_reply = xcb_dri3_query_version_reply(
        conn, xcb_dri3_query_version(conn, 1, 0), nullptr);
    if (!dri3_reply) {
        HYBRIS_ERROR("x11-vulkan: DRI3 QueryVersion failed");
        return false;
    }
    free(dri3_reply);

    xcb_present_query_version_reply_t *present_reply =
        xcb_present_query_version_reply(
            conn, xcb_present_query_version(conn, 1, 0), nullptr);
    if (!present_reply) {
        HYBRIS_ERROR("x11-vulkan: Present QueryVersion failed");
        return false;
    }
    free(present_reply);
    return true;
}

/* The Android driver reports VK_KHR_android_surface. We advertise the XCB
 * surface extension in its place, and translate any X11 WSI extension the
 * application enables back into the Android one. */
static bool is_surface_extension(const char *name, const char **replacement)
{
    if (strcmp(name, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME) == 0) {
        *replacement = VK_KHR_XCB_SURFACE_EXTENSION_NAME;
        return true;
    }
    if (strcmp(name, VK_KHR_XCB_SURFACE_EXTENSION_NAME) == 0) {
        *replacement = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
        return true;
    }
    if (strcmp(name, VK_KHR_XLIB_SURFACE_EXTENSION_NAME) == 0) {
        *replacement = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
        return true;
    }
    return false;
}

static VkResult x11ws_vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties)
{
    VkResult res;

    if (_vkEnumerateInstanceExtensionProperties == NULL) {
        _vkEnumerateInstanceExtensionProperties = (VkResult (*)(const char *, uint32_t *, VkExtensionProperties *))
            (*_vkGetInstanceProcAddr)(NULL, "vkEnumerateInstanceExtensionProperties");
    }

    res = (*_vkEnumerateInstanceExtensionProperties)(pLayerName, pPropertyCount, pProperties);
    if (res == VK_SUCCESS && *pPropertyCount > 0 && pProperties != NULL) {
        uint32_t i;
        for (i = 0; i < *pPropertyCount; i++) {
            const char *replacement = NULL;
            if (is_surface_extension(pProperties[i].extensionName, &replacement)) {
                strncpy(pProperties[i].extensionName, replacement, VK_MAX_EXTENSION_NAME_SIZE);
                pProperties[i].extensionName[VK_MAX_EXTENSION_NAME_SIZE - 1] = '\0';
            }
        }
    }
    return res;
}

VkResult x11ws_vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance)
{
    VkInstanceCreateInfo createInfo = *pCreateInfo;
    VkResult result;
    /* Temporary array to replace X11 surface extensions with the Android one */
    char **enabledExtensions = NULL;
    uint32_t i;

    if (_vkCreateInstance == NULL) {
        _vkCreateInstance = (VkResult (*)(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *))
            (*_vkGetInstanceProcAddr)(NULL, "vkCreateInstance");
    }

    if (pCreateInfo->enabledExtensionCount > 0) {
        enabledExtensions = (char **)calloc(pCreateInfo->enabledExtensionCount, sizeof(char *));
        if (!enabledExtensions)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    for (i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        const char *replacement = NULL;
        if (is_surface_extension(pCreateInfo->ppEnabledExtensionNames[i], &replacement))
            enabledExtensions[i] = strdup(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
        else
            enabledExtensions[i] = strdup(pCreateInfo->ppEnabledExtensionNames[i]);
        if (!enabledExtensions[i]) {
            while (i-- > 0)
                free(enabledExtensions[i]);
            free(enabledExtensions);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    createInfo.ppEnabledExtensionNames = (const char * const *)enabledExtensions;

    result = (*_vkCreateInstance)(&createInfo, pAllocator, pInstance);

    for (i = 0; i < pCreateInfo->enabledExtensionCount; i++)
        free(enabledExtensions[i]);
    free(enabledExtensions);

    return result;
}

static void x11_free_surface(X11VulkanSurface *xsurface)
{
    if (xsurface->window) {
        xsurface->window->common.decRef(&xsurface->window->common);
        xsurface->window = NULL;
    }
    if (xsurface->owned_xdpy) {
        XCloseDisplay(xsurface->owned_xdpy);
        xsurface->owned_xdpy = NULL;
    }
    delete xsurface;
}

static VkResult x11ws_vkCreateXcbSurfaceKHR(VkInstance instance,
        const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSurfaceKHR *pSurface)
{
    VkAndroidSurfaceCreateInfoKHR createInfo;
    VkResult result;
    xcb_connection_t *conn = pCreateInfo->connection;
    xcb_window_t xwin = pCreateInfo->window;

    if (_vkCreateAndroidSurfaceKHR == NULL) {
        _vkCreateAndroidSurfaceKHR = (VkResult (*)(VkInstance, const VkAndroidSurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *))
            (*_vkGetInstanceProcAddr)(instance, "vkCreateAndroidSurfaceKHR");
    }
    if (!_vkCreateAndroidSurfaceKHR) {
        HYBRIS_ERROR("x11-vulkan: vkCreateAndroidSurfaceKHR unavailable");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    if (!conn || xcb_connection_has_error(conn)) {
        HYBRIS_ERROR("x11-vulkan: invalid XCB connection");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!x11_dri3_present_available(conn)) {
        HYBRIS_ERROR("x11-vulkan: DRI3/Present unavailable");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    xcb_get_geometry_reply_t *gg = xcb_get_geometry_reply(
        conn, xcb_get_geometry(conn, xwin), NULL);
    if (!gg) {
        HYBRIS_ERROR("x11-vulkan: GetGeometry on window 0x%x failed", xwin);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    unsigned int w = gg->width  ? gg->width  : 1;
    unsigned int h = gg->height ? gg->height : 1;
    free(gg);

    X11VulkanSurface *xsurface = new X11VulkanSurface;
    xsurface->owned_xdpy = NULL;
    xsurface->window = new X11NativeWindow(conn, xwin, w, h, true);
    xsurface->window->common.incRef(&xsurface->window->common);

    createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    createInfo.pNext = NULL;
    createInfo.flags = 0;
    createInfo.window = xsurface->window;

    result = (*_vkCreateAndroidSurfaceKHR)(instance, &createInfo, pAllocator, pSurface);

    if (result == VK_SUCCESS) {
        x11_surface_push_mapping(*pSurface, xsurface);
    } else {
        HYBRIS_ERROR("x11-vulkan: vkCreateAndroidSurfaceKHR failed");
        x11_free_surface(xsurface);
    }
    return result;
}

static VkResult x11ws_vkCreateXlibSurfaceKHR(VkInstance instance,
        const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSurfaceKHR *pSurface)
{
    Display *xdpy = pCreateInfo->dpy;
    if (!xdpy) {
        xdpy = XOpenDisplay(NULL);
        if (!xdpy) {
            HYBRIS_ERROR("x11-vulkan: XOpenDisplay(NULL) failed (DISPLAY=%s)",
                         getenv("DISPLAY"));
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    xcb_connection_t *conn = XGetXCBConnection(xdpy);
    if (!conn) {
        HYBRIS_ERROR("x11-vulkan: XGetXCBConnection returned NULL");
        if (!pCreateInfo->dpy)
            XCloseDisplay(xdpy);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkXcbSurfaceCreateInfoKHR xcbInfo;
    xcbInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    xcbInfo.pNext = pCreateInfo->pNext;
    xcbInfo.flags = pCreateInfo->flags;
    xcbInfo.connection = conn;
    xcbInfo.window = (xcb_window_t)pCreateInfo->window;

    VkResult result = x11ws_vkCreateXcbSurfaceKHR(instance, &xcbInfo, pAllocator, pSurface);

    if (!pCreateInfo->dpy) {
        if (result == VK_SUCCESS) {
            X11VulkanSurface *xsurface = x11_surface_get_mapping(*pSurface);
            if (xsurface)
                xsurface->owned_xdpy = xdpy;
        } else {
            XCloseDisplay(xdpy);
        }
    }
    return result;
}

static VkBool32 x11ws_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, xcb_connection_t *connection, uint32_t visual_id)
{
    return connection && !xcb_connection_has_error(connection) ? VK_TRUE : VK_FALSE;
}

static VkBool32 x11ws_vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, Display *dpy, VisualID visualID)
{
    return dpy ? VK_TRUE : VK_FALSE;
}

static void x11ws_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator)
{
    if (x11_surface_has_mapping(surface)) {
        X11VulkanSurface *xsurface = x11_surface_pop_mapping(surface);

        if (_vkDestroySurfaceKHR == NULL) {
            _vkDestroySurfaceKHR = (PFN_vkVoidFunction (*)(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *))
                (*_vkGetInstanceProcAddr)(instance, "vkDestroySurfaceKHR");
        }

        if (_vkDestroySurfaceKHR)
            _vkDestroySurfaceKHR(instance, surface, pAllocator);
        x11_free_surface(xsurface);
    }
}

extern "C" void x11ws_vkSetInstanceProcAddrFunc(PFN_vkVoidFunction addr)
{
    if (_vkGetInstanceProcAddr == NULL)
        _vkGetInstanceProcAddr = (PFN_vkVoidFunction (*)(VkInstance, const char *))addr;
}

static void x11ws_patchSurfaceCapabilities(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
    /* X11 windows are freely resizable, so let the application pick the
     * swapchain size; prepareSwapchain resizes the native window to match. */
    pSurfaceCapabilities->currentExtent.width = 0xFFFFFFFF;
    pSurfaceCapabilities->currentExtent.height = 0xFFFFFFFF;

    if (pSurfaceCapabilities->maxImageExtent.width < 16384)
        pSurfaceCapabilities->maxImageExtent.width = 16384;
    if (pSurfaceCapabilities->maxImageExtent.height < 16384)
        pSurfaceCapabilities->maxImageExtent.height = 16384;
}

static void x11ws_prepareSwapchain(const VkSwapchainCreateInfoKHR *pCreateInfo)
{
    X11VulkanSurface *xsurface = x11_surface_get_mapping(pCreateInfo->surface);
    if (!xsurface || !xsurface->window)
        return;

    unsigned int width = pCreateInfo->imageExtent.width;
    unsigned int height = pCreateInfo->imageExtent.height;
    if (width > 0 && height > 0)
        xsurface->window->resize(width, height);
}

struct ws_module ws_module_info = {
    x11ws_init_module,

    x11ws_vkEnumerateInstanceExtensionProperties,
    x11ws_vkCreateInstance,
#ifdef WANT_WAYLAND
    NULL,
    NULL,
#endif
    x11ws_vkCreateXcbSurfaceKHR,
    x11ws_vkCreateXlibSurfaceKHR,
    x11ws_vkGetPhysicalDeviceXcbPresentationSupportKHR,
    x11ws_vkGetPhysicalDeviceXlibPresentationSupportKHR,
    x11ws_vkDestroySurfaceKHR,
    x11ws_patchSurfaceCapabilities,
    x11ws_prepareSwapchain,
    x11ws_vkSetInstanceProcAddrFunc,
};

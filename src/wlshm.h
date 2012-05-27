#ifndef _XF86_VIDEO_WAYLAND_SHM_H_
#define _XF86_VIDEO_WAYLAND_SHM_H_

/* All drivers should typically include these */
#include "xf86.h"
#include "xf86_OSproc.h"

#include "xf86Cursor.h"

#include "xf86xv.h"
#include <X11/extensions/Xv.h>
#include <string.h>

#include "xwayland.h"

/* globals */
struct wlshm_device
{
    DGAModePtr		dga_modes;
    int			num_dga_modes;
    Bool		dga_active;
    int			dga_viewport_status;

    /* options */
    OptionInfoPtr options;

    /* proc pointer */
    CloseScreenProcPtr CloseScreen;
    CreateWindowProcPtr	CreateWindow;
    DestroyWindowProcPtr DestroyWindow;
    UnrealizeWindowProcPtr UnrealizeWindow;
    SetWindowPixmapProcPtr SetWindowPixmap;

    pointer* fb;

    struct xwl_screen *xwl_screen;
};

struct wlshm_pixmap {
    int fd;
    void *orig;
    void *data;
    size_t bytes;
};

static inline struct wlshm_device *wlshm_scrninfo_priv(ScrnInfoPtr pScrn)
{
    return ((struct wlshm_device *)((pScrn)->driverPrivate));
}

static inline struct wlshm_device *wlshm_screen_priv(ScreenPtr pScreen)
{
    return wlshm_scrninfo_priv(xf86Screens[pScreen->myNum]);
}

#endif

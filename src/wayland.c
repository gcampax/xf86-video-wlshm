/*
 * Copyright © 2002 SuSE Linux AG
 * Copyright © 2010 commonIT
 * Copyright © 2012 Raspberry Pi Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors: Egbert Eich <eich@freedesktop.org>
 *          Corentin Chary <corentincj@iksaif.net>
 *          Daniel Stone <daniel@fooishbar.org>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* All drivers should typically include these */
#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Modes.h"
#include "micmap.h"

/* All drivers initialising the SW cursor need this */
#include "mipointer.h"

/* All drivers using framebuffer need this */
#include "fb.h"
#include "picturestr.h"

/* All drivers using xwayland module need this */
#include "xwayland.h"
#include <xf86Priv.h>

/*
 * Driver data structures.
 */
#include "wayland.h"
#include "compat-api.h"

#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

/* These need to be checked */
#include <X11/X.h>
#include <X11/Xproto.h>
#include "scrnintstr.h"
#include "servermd.h"

static DevPrivateKeyRec wlshm_pixmap_private_key;

static Bool
wlshm_get_device(ScrnInfoPtr pScrn)
{
    /*
     * Allocate a wlshm_device, and hook it into pScrn->driverPrivate.
     * pScrn->driverPrivate is initialised to NULL, so we can check if
     * the allocation has already been done.
     */
    if (pScrn->driverPrivate != NULL)
	return TRUE;

    pScrn->driverPrivate = xnfcalloc(sizeof(struct wlshm_device), 1);

    if (pScrn->driverPrivate == NULL)
	return FALSE;

    return TRUE;
}

static void
wlshm_free_device(ScrnInfoPtr pScrn)
{
    if (pScrn->driverPrivate == NULL)
	return;

    free(pScrn->driverPrivate);
    pScrn->driverPrivate = NULL;
}

static Bool
wlshm_save_screen(ScreenPtr pScreen, int mode)
{
    return TRUE;
}

static Bool
wlshm_enter_vt(VT_FUNC_ARGS_DECL)
{
    return TRUE;
}

static void
wlshm_leave_vt(VT_FUNC_ARGS_DECL)
{
}

static Bool
wlshm_switch_mode(SWITCH_MODE_ARGS_DECL)
{
    return TRUE;
}

static void
wlshm_adjust_frame(ADJUST_FRAME_ARGS_DECL)
{
}

static void
block_handler(pointer blockData, OSTimePtr pTimeout, pointer pReadMask)
{
    struct wlshm_device *wlshm = (struct wlshm_device *) blockData;

    if (wlshm->xwl_screen)
        xwl_screen_post_damage(wlshm->xwl_screen);
}

static void
wakeup_handler(pointer blockData, int result, pointer pReadMask)
{
}

static Bool
wlshm_close_screen(CLOSE_SCREEN_ARGS_DECL)
{
    ScrnInfoPtr pScrn = xf86Screens[screen->myNum];
    struct wlshm_device *wlshm = wlshm_scrninfo_priv(pScrn);

    if (wlshm->fb)
	free(wlshm->fb);
    wlshm->fb = NULL;

    RemoveBlockAndWakeupHandlers(block_handler, wakeup_handler, wlshm);

    xwl_screen_close(wlshm->xwl_screen);

    pScrn->vtSema = FALSE;
    screen->CloseScreen = wlshm->CloseScreen;
    return (*screen->CloseScreen)(CLOSE_SCREEN_ARGS);
}

static void
wlshm_free_screen(FREE_SCREEN_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    struct wlshm_device *wlshm = wlshm_scrninfo_priv(scrn);

    if (wlshm) {
        if (wlshm->xwl_screen)
	    xwl_screen_destroy(wlshm->xwl_screen);
	wlshm->xwl_screen = NULL;
    }

    wlshm_free_device(scrn);
}

static ModeStatus
wlshm_valid_mode(SCRN_ARG_TYPE arg, DisplayModePtr mode, Bool verbose, int flags)
{
    return MODE_OK;
}

static void
wlshm_free_pixmap(PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    struct wlshm_device *wlshm = wlshm_screen_priv(pScreen);
    struct wlshm_pixmap *d;

    d = dixLookupPrivate(&pixmap->devPrivates, &wlshm_pixmap_private_key);
    if (!d)
        return;

    pScreen->ModifyPixmapHeader(pixmap, 0, 0, 0, 0, 0, NULL);

    munmap(d->data, d->bytes);
    close(d->fd);
    free(d);
    dixSetPrivate(&pixmap->devPrivates, &wlshm_pixmap_private_key, NULL);
}

static Bool
wlshm_screen_init(SCREEN_INIT_ARGS_DECL)
{
    ScrnInfoPtr pScrn;
    struct wlshm_device *wlshm;
    int ret;
    VisualPtr visual;

    if (!dixRegisterPrivateKey(&wlshm_pixmap_private_key, PRIVATE_PIXMAP, 0))
        return BadAlloc;

    pScrn = xf86Screens[screen->myNum];
    wlshm = wlshm_screen_priv(screen);

    /* Reset visual list. */
    miClearVisualTypes();

    /* Setup the visuals we support. */
    if (!miSetVisualTypes(pScrn->depth,
                          miGetDefaultVisualMask(pScrn->depth),
                          pScrn->rgbBits, pScrn->defaultVisual))
         return FALSE;

    if (!miSetPixmapDepths())
        return FALSE;

    wlshm->fb = malloc(pScrn->virtualX * pScrn->virtualY * pScrn->bitsPerPixel);
    if (!wlshm->fb)
	return FALSE;

    /*
     * Call the framebuffer layer's ScreenInit function, and fill in other
     * pScreen fields.
     */
    ret = fbScreenInit(screen, wlshm->fb,
                       pScrn->virtualX, pScrn->virtualY,
                       pScrn->xDpi, pScrn->yDpi,
                       pScrn->displayWidth, pScrn->bitsPerPixel);
    if (!ret)
	return FALSE;

    if (pScrn->depth > 8) {
        /* Fixup RGB ordering */
        visual = screen->visuals + screen->numVisuals;
        while (--visual >= screen->visuals) {
	    if ((visual->class | DynamicClass) != DirectColor)
                continue;

            visual->offsetRed = pScrn->offset.red;
            visual->offsetGreen = pScrn->offset.green;
            visual->offsetBlue = pScrn->offset.blue;
            visual->redMask = pScrn->mask.red;
            visual->greenMask = pScrn->mask.green;
            visual->blueMask = pScrn->mask.blue;
	}
    }

    /* must be after RGB ordering fixed */
    fbPictureInit(screen, 0, 0);

    xf86SetBlackWhitePixels(screen);

    xf86SetBackingStore(screen);
    xf86SetSilkenMouse(screen);

    /* Initialise cursor functions */
    miDCInitialize(screen, xf86GetPointerScreenFuncs());

    /* FIXME: colourmap */
    miCreateDefColormap(screen);

    screen->SaveScreen = wlshm_save_screen;

    /* Wrap the current CloseScreen function */
    wlshm->CloseScreen = screen->CloseScreen;
    screen->CloseScreen = wlshm_close_screen;

    /* Report any unused options (only for the first generation) */
    if (serverGeneration == 1)
	xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);

    miScreenDevPrivateInit(screen, screen->width, wlshm->fb);

    RegisterBlockAndWakeupHandlers(block_handler, wakeup_handler, wlshm);

    if (wlshm->xwl_screen)
	return (xwl_screen_init(wlshm->xwl_screen, screen) == Success);

    return TRUE;
}

static int
wlshm_create_window_buffer(struct xwl_window *xwl_window,
                           PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    char filename[] = "/tmp/wayland-shm-XXXXXX";
    int ret = BadAlloc;
    struct wlshm_pixmap *d;

    d = calloc(sizeof(struct wlshm_pixmap), 1);
    if (!d) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "can't alloc wlshm pixmap: %s\n",
                   strerror(errno));
        goto exit;
    }
    d->fd = -1;
    d->data = MAP_FAILED;

    d->fd = mkstemp(filename);
    if (d->fd < 0) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "open %s failed: %s\n",
                   filename, strerror(errno));
        goto exit;
    }

    d->bytes = pixmap->drawable.width * pixmap->drawable.height *
               (pixmap->drawable.bitsPerPixel / 8);

    if (ftruncate(d->fd, d->bytes) < 0) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "ftruncate failed: %s\n",
                   strerror(errno));
        goto exit;
    }

    d->data = mmap(NULL, d->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
    unlink(filename);

    if (d->data == MAP_FAILED) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "mmap failed: %s\n",
                   strerror(errno));
        goto exit;
    }

    ret = xwl_create_window_buffer_shm(xwl_window, pixmap, d->fd);
    if (ret != Success) {
        goto exit;
    }

    memcpy(d->data, pixmap->devPrivate.ptr, d->bytes);

    pScreen->ModifyPixmapHeader(pixmap, 0, 0, 0, 0, 0, d->data);

    dixSetPrivate(&pixmap->devPrivates, &wlshm_pixmap_private_key, d);

    return ret;
exit:
    if (d) {
        if (d->fd != -1)
            close(d->fd);
        if (d->data != MAP_FAILED)
            munmap(d->data, d->bytes);
        free(d);
    }

    return ret;
}

static struct xwl_driver xwl_driver = {
    .version = 2,
    .create_window_buffer = wlshm_create_window_buffer,
};

static const OptionInfoRec wlshm_options[] = {
    { -1,                  NULL,           OPTV_NONE,	{0}, FALSE }
};

static Bool
wlshm_pre_init(ScrnInfoPtr pScrn, int flags)
{
    struct wlshm_device *wlshm;
    int i;
    GDevPtr device;
    int flags24;

    if (flags & PROBE_DETECT)
	return TRUE;

    if (!xorgWayland) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "You must run Xorg with -xwayland parameter\n");
        return FALSE;
    }

    /* Allocate the wlshm_device driverPrivate */
    if (!wlshm_get_device(pScrn))
	return FALSE;

    wlshm = wlshm_scrninfo_priv(pScrn);

    pScrn->chipset = WAYLAND_DRIVER_NAME;
    pScrn->monitor = pScrn->confScreen->monitor;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Initializing Wayland SHM driver\n");

    flags24 = Support32bppFb | SupportConvert24to32 | PreferConvert24to32;
    if (!xf86SetDepthBpp(pScrn, 0, 0, 0, flags24))
	return FALSE;

    /* Check that the returned depth is one we support */
    switch (pScrn->depth) {
    case 24:
    case 30:
    case 32:
        break;
    default:
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "Given depth (%d) is not supported by this driver\n",
		   pScrn->depth);
	return FALSE;
    }

    xf86PrintDepthBpp(pScrn);

    /*
     * This must happen after pScrn->display has been set because
     * xf86SetWeight references it.
     */
    if (pScrn->depth > 8) {
	/* The defaults are OK for us */
	rgb zeros = {0, 0, 0};

	if (!xf86SetWeight(pScrn, zeros, zeros))
	    return FALSE;

        /* XXX check that weight returned is supported */
	;
    }

    if (!xf86SetDefaultVisual(pScrn, -1))
	return FALSE;

    if (pScrn->depth > 1) {
	Gamma zeros = {0.0, 0.0, 0.0};

	if (!xf86SetGamma(pScrn, zeros))
	    return FALSE;
    }

    device = xf86GetEntityInfo(pScrn->entityList[0])->device;
    xf86CollectOptions(pScrn, device->options);
    free(device);

    /* Process the options */
    if (!(wlshm->options = malloc(sizeof(wlshm_options))))
	return FALSE;

    memcpy(wlshm->options, wlshm_options, sizeof(wlshm_options));

    xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, wlshm->options);

    wlshm->xwl_screen = xwl_screen_create();
    if (!wlshm->xwl_screen) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to initialize xwayland.\n");
        goto error;
    }

    if (!xwl_screen_pre_init(pScrn, wlshm->xwl_screen, 0, &xwl_driver)) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "Failed to pre-init xwayland screen\n");
        xwl_screen_destroy(wlshm->xwl_screen);
    }

    /* Subtract memory for HW cursor */
    xf86ValidateModesSize(pScrn, pScrn->monitor->Modes,
                          pScrn->display->virtualX,
                          pScrn->display->virtualY,
                          0);

    /* Prune the modes marked as invalid */
    xf86PruneDriverModes(pScrn);

    if (pScrn->modes == NULL) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No valid modes found\n");
	goto error;
    }

    /*
     * Set the CRTC parameters for all of the modes based on the type
     * of mode, and the chipset's interlace requirements.
     *
     * Calling this is required if the mode->Crtc* values are used by the
     * driver and if the driver doesn't provide code to set them.  They
     * are not pre-initialised at all.
     */
    xf86SetCrtcForModes(pScrn, 0);

    /* Set the current mode to the first in the list */
    pScrn->currentMode = pScrn->modes;

    /* Print the list of modes being used */
    xf86PrintModes(pScrn);

    /* If monitor resolution is set on the command line, use it */
    xf86SetDpi(pScrn, 0, 0);

    if (xf86LoadSubModule(pScrn, "fb") == NULL)
	goto error;

    /* We have no contiguous physical fb in physical memory */
    pScrn->memPhysBase = 0;
    pScrn->fbOffset = 0;

    return TRUE;

error:
    wlshm_free_device(pScrn);
    return FALSE;
}

/* Mandatory */
static Bool
wayland_probe(DriverPtr drv, int flags)
{
    Bool found = FALSE;
    int count;
    GDevPtr *sections;
    int i;

    if (flags & PROBE_DETECT)
	return FALSE;
    /*
     * Find the config file Device sections that match this
     * driver, and return if there are none.
     */
    count = xf86MatchDevice(WAYLAND_DRIVER_NAME, &sections);

    if (count <= 0) {
	return FALSE;
    }

    for (i = 0; i < count; i++) {
        int entityIndex = xf86ClaimNoSlot(drv, 0, sections[i], TRUE);
        ScrnInfoPtr pScrn = xf86AllocateScreen(drv, 0);

        if (!pScrn)
            continue;

        xf86AddEntityToScreen(pScrn, entityIndex);
        pScrn->driverVersion = COMBINED_DRIVER_VERSION;
        pScrn->driverName    = WAYLAND_DRIVER_NAME;
        pScrn->name          = WAYLAND_DRIVER_NAME;
        pScrn->Probe         = wayland_probe;
        pScrn->PreInit       = wlshm_pre_init;
        pScrn->ScreenInit    = wlshm_screen_init;
        pScrn->SwitchMode    = wlshm_switch_mode;
        pScrn->AdjustFrame   = wlshm_adjust_frame;
        pScrn->EnterVT       = wlshm_enter_vt;
        pScrn->LeaveVT       = wlshm_leave_vt;
        pScrn->FreeScreen    = wlshm_free_screen;
        pScrn->ValidMode     = wlshm_valid_mode;

        found = TRUE;
    }

    free(sections);

    return found;
}

static const OptionInfoRec *
wayland_available_options(int chipid, int busid)
{
    return wlshm_options;
}

#ifndef HW_SKIP_CONSOLE
#define HW_SKIP_CONSOLE 4
#endif

#ifndef HW_WAYLAND
#define HW_WAYLAND 8
#endif

static Bool
wayland_driver_func(ScrnInfoPtr pScrn, xorgDriverFuncOp op, pointer ptr)
{
    CARD32 *flag;

    switch (op) {
    case GET_REQUIRED_HW_INTERFACES:
        flag = (CARD32*)ptr;
	(*flag) = HW_WAYLAND;
	return TRUE;
    default:
	return FALSE;
    }
}

/*
 * This contains the functions needed by the server after loading the driver
 * module.  It must be supplied, and gets passed back by the SetupProc
 * function in the dynamic case.  In the static case, a reference to this
 * is compiled in, and this requires that the name of this DriverRec be
 * an upper-case version of the driver name.
 */

_X_EXPORT DriverRec wayland = {
    COMBINED_DRIVER_VERSION,
    WAYLAND_DRIVER_NAME,
    NULL,
    wayland_probe,
    wayland_available_options,
    NULL,
    0,
    wayland_driver_func
};

static XF86ModuleVersionInfo wayland_vers_rec =
{
    WAYLAND_DRIVER_NAME,
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_VIDEODRV,
    ABI_VIDEODRV_VERSION,
    MOD_CLASS_VIDEODRV,
    { 0, 0, 0, 0 }
};


static pointer
wayland_setup(pointer module, pointer opts, int *errmaj, int *errmin)
{
    static Bool initialized = FALSE;

    if (initialized) {
	if (errmaj)
            *errmaj = LDR_ONCEONLY;
	return NULL;
    }

    initialized = TRUE;
    xf86AddDriver(&wayland, module, HaveDriverFuncs);

    /*
     * The return value must be non-NULL on success even though there
     * is no TearDownProc.
     */
    return (pointer) 1;
}

/*
 * This is the module init data.
 * Its name has to be the driver name followed by ModuleData
 */
_X_EXPORT XF86ModuleData waylandModuleData = {
    &wayland_vers_rec,
    wayland_setup,
    NULL
};

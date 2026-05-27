// viv - vmunix's image viewer, *NIX/X11 port
// Original Windows/WinAPI version by vmunix, 5/24/26
// Ported to X11/Xlib with Zenity + Athena fallback file picker

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Shell.h>
#include <X11/Xaw/Form.h>
#include <X11/Xaw/Label.h>
#include <X11/Xaw/AsciiText.h>
#include <X11/Xaw/Command.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

// --- Globals ---

static Display*  gDisplay     = NULL;
static Window    gWindow      = None;
static GC        gGC          = None;
static XImage*   gXImage      = NULL;
static int       gImageWidth  = 0;
static int       gImageHeight = 0;
static int       gScreen      = 0;

// Athena dialog state
static XtAppContext  gAppContext   = NULL;
static Widget        gShellWidget  = NULL;  // top-level Xt shell (never mapped)
static Widget        gDialogShell  = NULL;
static Widget        gTextWidget   = NULL;
static char          gPickedPath[4096] = "";
static int           gDialogDone  = 0;  // 0 = running, 1 = ok, -1 = cancel

// --- Forward declarations ---
static void OpenImageDialog(void);
static bool LoadImageFile(const char* filename);
static void RunAthenaDialog(void);

// -----------------------------------------------------------------------
// Image loading
// -----------------------------------------------------------------------

static bool LoadImageFile(const char* filename)
{
    int width, height, channels;
    unsigned char* data = stbi_load(filename, &width, &height, &channels, 4);

    if (!data)
    {
        fprintf(stderr, "viv: failed to load '%s': %s\n",
                filename, stbi_failure_reason());
        return false;
    }

    if (gXImage)
    {
        // XDestroyImage frees both the XImage struct and ximage->data
        XDestroyImage(gXImage);
        gXImage = NULL;
    }

    Visual* visual = DefaultVisual(gDisplay, gScreen);
    int depth = DefaultDepth(gDisplay, gScreen);

    // We'll build a 32-bpp image buffer in BGRA order (what X expects for
    // TrueColor at 32 bpp). The actual byte order depends on the server,
    // so we check XImageByteOrder.
    int rowBytes = width * 4;
    char* imgData = (char*)malloc(rowBytes * height);
    if (!imgData)
    {
        stbi_image_free(data);
        fprintf(stderr, "viv: out of memory\n");
        return false;
    }

    bool lsbFirst = (XImageByteOrder(gDisplay) == LSBFirst);

    for (int i = 0; i < width * height; i++)
    {
        unsigned char r = data[i * 4 + 0];
        unsigned char g = data[i * 4 + 1];
        unsigned char b = data[i * 4 + 2];
        // alpha is ignored for now (XPutImage has no alpha blend)

        // Pack as 0x00RRGGBB or 0x00BBGGRR depending on byte order
        unsigned char* dst = (unsigned char*)(imgData + i * 4);
        if (lsbFirst)
        {
            // Little-endian: pixel stored as B G R 00
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
            dst[3] = 0;
        }
        else
        {
            // Big-endian: pixel stored as 00 R G B
            dst[0] = 0;
            dst[1] = r;
            dst[2] = g;
            dst[3] = b;
        }
    }

    stbi_image_free(data);

    gXImage = XCreateImage(
        gDisplay,
        visual,
        depth,
        ZPixmap,
        0,
        imgData,      // XDestroyImage will free this
        width,
        height,
        32,
        rowBytes);

    if (!gXImage)
    {
        free(imgData);
        fprintf(stderr, "viv: XCreateImage failed\n");
        return false;
    }

    gImageWidth  = width;
    gImageHeight = height;

    // Force a redraw
    XClearArea(gDisplay, gWindow, 0, 0, 0, 0, True);
    XFlush(gDisplay);
    return true;
}

// -----------------------------------------------------------------------
// Rendering - scale-to-fit with aspect ratio, centered
// -----------------------------------------------------------------------

static void DrawImage(void)
{
    if (!gXImage) return;

    XWindowAttributes wa;
    XGetWindowAttributes(gDisplay, gWindow, &wa);
    int winW = wa.width;
    int winH = wa.height;

    float imageAspect  = (float)gImageWidth  / (float)gImageHeight;
    float windowAspect = (float)winW / (float)winH;

    int drawW, drawH;
    if (windowAspect > imageAspect)
    {
        drawH = winH;
        drawW = (int)(winH * imageAspect);
    }
    else
    {
        drawW = winW;
        drawH = (int)(winW / imageAspect);
    }

    int x = (winW - drawW) / 2;
    int y = (winH - drawH) / 2;

    // Scale the image with nearest-neighbour into a temporary XImage,
    // then XPutImage that to the window.
    int rowBytes = drawW * 4;
    char* scaled = (char*)malloc(rowBytes * drawH);
    if (!scaled) return;

    for (int dy = 0; dy < drawH; dy++)
    {
        int sy = dy * gImageHeight / drawH;
        for (int dx = 0; dx < drawW; dx++)
        {
            int sx = dx * gImageWidth / drawW;
            // Copy 4 bytes from source pixel
            memcpy(scaled + (dy * drawW + dx) * 4,
                   gXImage->data + (sy * gImageWidth + sx) * 4,
                   4);
        }
    }

    Visual* visual   = DefaultVisual(gDisplay, gScreen);
    int     depth    = DefaultDepth(gDisplay, gScreen);

    XImage* scaledImg = XCreateImage(
        gDisplay, visual, depth, ZPixmap, 0,
        scaled, drawW, drawH, 32, rowBytes);

    if (!scaledImg)
    {
        free(scaled);
        return;
    }

    // Clear background
    XSetForeground(gDisplay, gGC,
                   XWhitePixel(gDisplay, gScreen));
    XFillRectangle(gDisplay, gWindow, gGC, 0, 0, winW, winH);

    XPutImage(gDisplay, gWindow, gGC, scaledImg,
              0, 0, x, y, drawW, drawH);

    // XDestroyImage frees scaledImg->data (== scaled) too
    XDestroyImage(scaledImg);
}

// -----------------------------------------------------------------------
// Zenity file picker
// -----------------------------------------------------------------------

// Returns true and fills `out` (size outSize) if the user picked a file.
static bool TryZenity(char* out, int outSize)
{
    // Check if zenity exists on PATH
    if (system("which zenity > /dev/null 2>&1") != 0)
        return false;

    FILE* fp = popen(
        "zenity --file-selection --title='Open image' "
        "--file-filter='Images | *.png *.jpg *.jpeg *.bmp *.tga *.gif' "
        "--file-filter='All files | *' 2>/dev/null",
        "r");

    if (!fp) return false;

    bool got = (fgets(out, outSize, fp) != NULL);
    pclose(fp);

    if (got)
    {
        // Strip trailing newline
        int len = strlen(out);
        if (len > 0 && out[len - 1] == '\n')
            out[len - 1] = '\0';
    }

    return got && out[0] != '\0';
}

// -----------------------------------------------------------------------
// Athena fallback dialog
// -----------------------------------------------------------------------

static void AthenaOkCb(Widget w, XtPointer clientData, XtPointer callData)
{
    (void)w; (void)clientData; (void)callData;

    // Grab the text from the AsciiText widget
    String val = NULL;
    XtVaGetValues(gTextWidget, XtNstring, &val, NULL);

    if (val && val[0] != '\0')
    {
        strncpy(gPickedPath, val, sizeof(gPickedPath) - 1);
        gPickedPath[sizeof(gPickedPath) - 1] = '\0';
        gDialogDone = 1;
    }
    else
    {
        gDialogDone = -1;
    }
}

static void AthenaCancelCb(Widget w, XtPointer clientData, XtPointer callData)
{
    (void)w; (void)clientData; (void)callData;
    gDialogDone = -1;
}

static void RunAthenaDialog(void)
{
    gDialogDone  = 0;
    gPickedPath[0] = '\0';

    // Create a top-level application shell that we never map, just so
    // we have an Xt root to hang the dialog off.
    // (gShellWidget was already created in main.)

    // Popup shell for the dialog
    gDialogShell = XtVaCreatePopupShell(
        "openDialog",
        transientShellWidgetClass,
        gShellWidget,
        XtNtitle,  "Open image",
        XtNwidth,  420,
        NULL);

    // Form container
    Widget form = XtVaCreateManagedWidget(
        "form", formWidgetClass, gDialogShell,
        XtNdefaultDistance, 8,
        NULL);

    // Label
    Widget label = XtVaCreateManagedWidget(
        "label", labelWidgetClass, form,
        XtNlabel,       "Image path:",
        XtNborderWidth, 0,
        XtNtop,         XawChainTop,
        XtNleft,        XawChainLeft,
        XtNright,       XawChainRight,
        NULL);

    // Single-line text field
    gTextWidget = XtVaCreateManagedWidget(
        "path", asciiTextWidgetClass, form,
        XtNtype,         XawAsciiString,
        XtNwidth,        390,
        XtNeditType,     XawtextEdit,
        XtNstring,       "",
        XtNfromVert,     label,
        XtNtop,          XawChainTop,
        XtNleft,         XawChainLeft,
        XtNright,        XawChainRight,
        NULL);

    // OK button
    Widget okBtn = XtVaCreateManagedWidget(
        "OK", commandWidgetClass, form,
        XtNfromVert,  gTextWidget,
        XtNtop,       XawChainTop,
        XtNleft,      XawChainLeft,
        NULL);
    XtAddCallback(okBtn, XtNcallback, AthenaOkCb, NULL);

    // Cancel button
    Widget cancelBtn = XtVaCreateManagedWidget(
        "Cancel", commandWidgetClass, form,
        XtNfromVert,  gTextWidget,
        XtNfromHoriz, okBtn,
        XtNtop,       XawChainTop,
        XtNleft,      XawChainLeft,
        NULL);
    XtAddCallback(cancelBtn, XtNcallback, AthenaCancelCb, NULL);

    XtPopup(gDialogShell, XtGrabExclusive);

    // Run a nested Xt event loop until the user clicks OK or Cancel
    while (gDialogDone == 0)
        XtAppProcessEvent(gAppContext, XtIMAll);

    XtPopdown(gDialogShell);
    XtDestroyWidget(gDialogShell);
    gDialogShell = NULL;
    gTextWidget  = NULL;
}

// -----------------------------------------------------------------------
// Unified "open image" entry point
// -----------------------------------------------------------------------

static void OpenImageDialog(void)
{
    char path[4096] = "";

    if (TryZenity(path, sizeof(path)))
    {
        LoadImageFile(path);
        return;
    }

    // Zenity not available — fall back to Athena dialog
    RunAthenaDialog();

    if (gDialogDone == 1 && gPickedPath[0] != '\0')
        LoadImageFile(gPickedPath);
}

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // Initialise Xt (needed for the Athena fallback) but never show its
    // root shell — we only use it as an app-context + widget root.
    int    xtArgc = 1;           // Xt gets only argv[0]
    char*  xtArgv[] = { argv[0], NULL };

	XtToolkitInitialize();
	gAppContext = XtCreateApplicationContext();

	gDisplay = XtOpenDisplay(gAppContext, NULL, "viv", "viv",
				NULL, 0, &xtArgc, xtArgv);

	if (!gDisplay)
	{
		fprintf(stderr, "viv: cannot open display\n");
		return 1;
	}

	gScreen = DefaultScreen(gDisplay);

	gShellWidget = XtVaAppCreateShell(
		"viv", "viv",
		applicationShellWidgetClass,
		gDisplay,
		XtNwidth, 1,
		XtNheight, 1,
		XtNmappedWhenManaged, False,
		NULL);


    XtRealizeWidget(gShellWidget);   // must be realised before we can make children

    // Create the main viewer window (plain Xlib — no Xt)
    unsigned long bg = XAllocNamedColor(gDisplay,
                           DefaultColormap(gDisplay, gScreen),
                           "gray78", // close to RGB(200,200,200)
                           &(XColor){}, &(XColor){})
                       ? ((XColor){ .pixel=0 }, // I'm sorry -vmunix
                          [](Display* d, Colormap cm) { // Win32 is miles better
                              XColor exact, screen;
                              XAllocNamedColor(d, cm, "gray78", &screen, &exact);
                              return screen.pixel;
                          }(gDisplay, DefaultColormap(gDisplay, gScreen)))
                       : XWhitePixel(gDisplay, gScreen);

    unsigned long fg = XBlackPixel(gDisplay, gScreen);

    gWindow = XCreateSimpleWindow(
        gDisplay,
        DefaultRootWindow(gDisplay),
        0, 0,
        800, 600,
        1, fg, bg);

    XStoreName(gDisplay, gWindow, "viv");

    // Subscribe to the events we care about
    XSelectInput(gDisplay, gWindow,
                 ExposureMask       |
                 KeyPressMask       |
                 StructureNotifyMask);

    // Let the WM send us a WM_DELETE_WINDOW message instead of just
    // killing the connection.
    Atom wmDelete = XInternAtom(gDisplay, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(gDisplay, gWindow, &wmDelete, 1);

    // Simple GC for drawing
    gGC = XCreateGC(gDisplay, gWindow, 0, NULL);

    XMapWindow(gDisplay, gWindow);
    XFlush(gDisplay);

    // If a filename was given on the command line, load it immediately;
    // otherwise pop the file picker.
    if (argc >= 2)
    {
        LoadImageFile(argv[1]);
    }
    else
    {
        OpenImageDialog();
    }

    // ---- Event loop ----
    bool running = true;

    while (running)
    {
        // We use XtAppNextEvent so that Xt/Athena events are dispatched
        // correctly even outside the dialog (keeps gAppContext healthy).
        XEvent ev;
        XtAppNextEvent(gAppContext, &ev);

        // Only handle events for our plain Xlib window ourselves;
        // everything else goes to Xt.
        if (ev.xany.window != gWindow)
        {
            XtDispatchEvent(&ev);
            continue;
        }

        switch (ev.type)
        {
        case Expose:
            if (ev.xexpose.count == 0)
                DrawImage();
            break;

        case ConfigureNotify:
            // Window was resized — redraw
            DrawImage();
            break;

        case KeyPress:
            {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                if (ks == XK_l || ks == XK_L)
                    OpenImageDialog();
                else if (ks == XK_q || ks == XK_Q || ks == XK_Escape)
                    running = false;
            }
            break;

        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == wmDelete)
                running = false;
            break;
        }
    }

    // Cleanup
    if (gXImage)  { XDestroyImage(gXImage); gXImage = NULL; }
    XFreeGC(gDisplay, gGC);
    XDestroyWindow(gDisplay, gWindow);
    XtDestroyWidget(gShellWidget);
    XtDestroyApplicationContext(gAppContext);

    return 0;
}

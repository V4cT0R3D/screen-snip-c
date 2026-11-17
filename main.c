#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#define APP_NAME                        L"Snip"
#define TITLE                           L"Snipping Tool (New: Ctrl+N | Save: Ctrl+S | Copy: Click | Escape: Esc)"
#define DEFAULT_FILE                    L"Screenshot.png"

#define ERR_SAVE                        L"The file could not be saved."
#define ERR_CAPTURE                     L"Could not capture the screen."
#define ERR_GDIP                        L"Could not start GDI+."
#define ERR_CLASS                       L"The window could not be registered."
#define ERR_WINDOW                      L"The window could not be created."

#define WND_CLASS                       L"SnipWnd"
#define EDT_CLASS                       L"SnipEdtWnd"
#define MUTEX_NAME                      L"ScreensnipSingleInstance"

#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#define ROP_DPA                         0x00A000C9
#define KEY_REPEAT_BIT                  (1 << 30)
#define DIM_PATTERN_ROWS                8

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { UINT32 Version; void* Cb; BOOL NoThread, NoCodecs; } GdipIn;

extern int  __stdcall GdiplusStartup(ULONG_PTR*, const GdipIn*, void*);
extern void __stdcall GdiplusShutdown(ULONG_PTR);
extern int  __stdcall GdipCreateBitmapFromHBITMAP(HBITMAP, HPALETTE, void**);
extern int  __stdcall GdipSaveImageToFile(void*, const WCHAR*, const CLSID*, const void*);
extern int  __stdcall GdipDisposeImage(void*);
extern HRESULT __stdcall DwmSetWindowAttribute(HWND, DWORD, const void*, DWORD);
extern HRESULT __stdcall DwmFlush(void);

// Replacements for the CRT, since the build uses -nostdlib
void* __cdecl memset(void* d, int v, size_t n) {
    volatile unsigned char* p = (volatile unsigned char*)d;
    while (n--) *p++ = (unsigned char)v;
    return d;
}

void* __cdecl memcpy(void* d, const void* s, size_t n) {
    volatile unsigned char* dp = (volatile unsigned char*)d;
    const volatile unsigned char* sp = (const volatile unsigned char*)s;
    while (n--) *dp++ = *sp++;
    return d;
}

typedef struct {
    HDC dc;
    HBITMAP bmp;
    HGDIOBJ old;
    int w, h;
} Surface;

static void Surf_Free(Surface* s) {
    if (s->dc) {
        if (s->old) SelectObject(s->dc, s->old);
        DeleteDC(s->dc);
    }
    if (s->bmp) DeleteObject(s->bmp);
    memset(s, 0, sizeof *s);
}

static bool Surf_New(Surface* s, HDC ref, int w, int h) {
    memset(s, 0, sizeof *s);
    if (w <= 0 || h <= 0) return false;

    s->dc = CreateCompatibleDC(ref);
    if (s->dc) s->bmp = CreateCompatibleBitmap(ref, w, h);
    if (!s->bmp) { Surf_Free(s); return false; }

    s->old = SelectObject(s->dc, s->bmp);
    if (!s->old) { Surf_Free(s); return false; }

    PatBlt(s->dc, 0, 0, w, h, BLACKNESS);
    s->w = w;
    s->h = h;
    return true;
}

static struct {
    HINSTANCE inst;
    Surface scr;          // full desktop capture
    Surface back;         // back buffer for WM_PAINT
    Surface cap;          // final selection at full resolution
    Surface scaled;       // cached scaled copy for the edit view
    int scaledW, scaledH;
    HPEN pen;
    HBRUSH dimBrush;
    HBRUSH bgBrush;
    HCURSOR cross;
    HWND sel;             // selection overlay
    HWND edt;             // edit window
    int vx, vy;           // virtual desktop origin
    int vw, vh;           // virtual desktop size
    POINT p1, p2;         // selection corners
    bool drag;
    bool edit;
} g;

static int IMin(int a, int b) { return a < b ? a : b; }
static int IMax(int a, int b) { return a > b ? a : b; }

static RECT SelRect(void) {
    RECT r = { IMin(g.p1.x, g.p2.x), IMin(g.p1.y, g.p2.y),
               IMax(g.p1.x, g.p2.x), IMax(g.p1.y, g.p2.y) };
    return r;
}

static void Fail(const WCHAR* msg) {
    MessageBoxW(NULL, msg, APP_NAME, MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

static const WCHAR* FindExt(const WCHAR* p) {
    const WCHAR* ext = NULL;
    for (; *p; ++p) {
        if (*p == L'.') ext = p;
        else if (*p == L'\\' || *p == L'/') ext = NULL;
    }
    return ext;
}

/* Display size: at most 80% of the work area, keeping the aspect ratio.
   Small images are not scaled up.*/
static void EditSize(int* dw, int* dh) {
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    int maxW = (wa.right - wa.left) * 8 / 10;
    int maxH = (wa.bottom - wa.top) * 8 / 10;
    int w = g.cap.w, h = g.cap.h;

    if (w <= maxW && h <= maxH) { *dw = w; *dh = h; return; }

    if (w * maxH > h * maxW) { *dw = maxW; *dh = h * maxW / w; }
    else                     { *dh = maxH; *dw = w * maxH / h; }
}

// The only path that shows the edit window, so it never stays at (-32000, -32000)
static void ShowEditWindow(void) {
    if (!g.cap.bmp) { PostQuitMessage(0); return; }

    g.edit = true;

    int dw, dh;
    EditSize(&dw, &dh);
    RECT wr = { 0, 0, dw + 40, dh + 40 };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    int ww = IMax(360, wr.right - wr.left);
    int wh = IMax(240, wr.bottom - wr.top);

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(g.edt, HWND_NOTOPMOST,
                 wa.left + (wa.right - wa.left - ww) / 2,
                 wa.top  + (wa.bottom - wa.top - wh) / 2,
                 ww, wh, SWP_SHOWWINDOW);
    SetForegroundWindow(g.edt);
    SetFocus(g.edt);
}

// Rebuilds the scaled cache only when the display size changes
static void UpdateScaled(HDC refDc) {
    int dw, dh;
    EditSize(&dw, &dh);

    if (g.scaled.bmp && dw == g.scaledW && dh == g.scaledH) return;

    Surf_Free(&g.scaled);
    if (!Surf_New(&g.scaled, refDc, dw, dh)) return;

    SetStretchBltMode(g.scaled.dc, HALFTONE);
    SetBrushOrgEx(g.scaled.dc, 0, 0, NULL);
    StretchBlt(g.scaled.dc, 0, 0, dw, dh, g.cap.dc, 0, 0, g.cap.w, g.cap.h, SRCCOPY);
    g.scaledW = dw;
    g.scaledH = dh;
}

static void CopyClip(HWND hwnd) {
    if (!g.cap.bmp) return;

    // 24-bit rows padded to 4 bytes
    size_t stride  = ((size_t)g.cap.w * 3 + 3) & ~(size_t)3;
    size_t imgSize = stride * (size_t)g.cap.h;

    // biSizeImage is a DWORD
    if (imgSize == 0 || imgSize > 0x7FFFFFFFu) return;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + imgSize);
    if (!hMem) return;

    BITMAPINFO* bi = (BITMAPINFO*)GlobalLock(hMem);
    if (!bi) { GlobalFree(hMem); return; }

    memset(bi, 0, sizeof(BITMAPINFOHEADER));
    bi->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi->bmiHeader.biWidth       = g.cap.w;
    bi->bmiHeader.biHeight      = g.cap.h;   // positive: bottom-up rows, as CF_DIB requires
    bi->bmiHeader.biPlanes      = 1;
    bi->bmiHeader.biBitCount    = 24;
    bi->bmiHeader.biCompression = BI_RGB;
    bi->bmiHeader.biSizeImage   = (DWORD)imgSize;

    int lines = GetDIBits(g.cap.dc, g.cap.bmp, 0, g.cap.h,
                          (BYTE*)bi + sizeof(BITMAPINFOHEADER), bi, DIB_RGB_COLORS);
    GlobalUnlock(hMem);

    bool ok = false;
    if (lines > 0 && OpenClipboard(hwnd)) {
        EmptyClipboard();
        ok = SetClipboardData(CF_DIB, hMem) != NULL;
        CloseClipboard();
    }
    if (!ok) { GlobalFree(hMem); return; }

    SetWindowTextW(hwnd, L"Copied to clipboard");
    SetTimer(hwnd, 1, 1000, NULL);
}

static void SaveFile(HWND hwnd) {
    static const CLSID pngId = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    static const CLSID jpgId = { 0x557cf401, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

    if (!g.cap.bmp) return;

    WCHAR path[MAX_PATH] = DEFAULT_FILE;
    OPENFILENAMEW ofn = {
        .lStructSize  = sizeof ofn,
        .hwndOwner    = hwnd,
        .hInstance    = g.inst,
        .lpstrFilter  = L"PNG (*.png)\0*.png\0JPEG (*.jpg)\0*.jpg\0",
        .nFilterIndex = 1,
        .lpstrFile    = path,
        .nMaxFile     = MAX_PATH,
        .lpstrDefExt  = L"png",
        .Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST
    };
    if (!GetSaveFileNameW(&ofn)) return;

    // 1 = PNG, 2 = JPG, 0 = unrecognized or missing extension
    int kind = 0;
    const WCHAR* ext = FindExt(path);
    if (ext && ext[1]) {
        if (lstrcmpiW(ext, L".png") == 0)                                        kind = 1;
        else if (lstrcmpiW(ext, L".jpg") == 0 || lstrcmpiW(ext, L".jpeg") == 0) kind = 2;
    }
    if (kind == 0) {
        kind = (ofn.nFilterIndex == 2) ? 2 : 1;
        if (ext) path[ext - path] = L'\0';
        int len = lstrlenW(path);
        if (len + 5 < MAX_PATH) lstrcatW(path, kind == 2 ? L".jpg" : L".png");
    }

    void* img = NULL;
    if (GdipCreateBitmapFromHBITMAP(g.cap.bmp, NULL, &img) == 0 && img) {
        int st = GdipSaveImageToFile(img, path, kind == 2 ? &jpgId : &pngId, NULL);
        GdipDisposeImage(img);
        if (st != 0) MessageBoxW(hwnd, ERR_SAVE, APP_NAME, MB_OK | MB_ICONERROR);
    }
}

static void Render(HWND hwnd, HDC wndDc, const RECT* area) {
    RECT cr;
    GetClientRect(hwnd, &cr);

    int cw = g.edit ? cr.right  : g.vw;
    int ch = g.edit ? cr.bottom : g.vh;
    if (cw <= 0 || ch <= 0) return;

    if (cw > g.back.w || ch > g.back.h) {
        Surf_Free(&g.back);
        if (!Surf_New(&g.back, wndDc, cw, ch)) return;
    }

    HDC b = g.back.dc;
    int saved = SaveDC(b);
    IntersectClipRect(b, area->left, area->top, area->right, area->bottom);

    if (g.edit) {
        int dw, dh;
        EditSize(&dw, &dh);
        UpdateScaled(wndDc);
        int ix = (cw - dw) / 2;
        int iy = (ch - dh) / 2;

        FillRect(b, &cr, g.bgBrush);
        if (g.scaled.dc)
            BitBlt(b, ix, iy, dw, dh, g.scaled.dc, 0, 0, SRCCOPY);
    } else {
        // Darkened desktop
        BitBlt(b, 0, 0, cw, ch, g.scr.dc, 0, 0, SRCCOPY);
        SetTextColor(b, RGB(0, 0, 0));
        SetBkColor(b, RGB(255, 255, 255));
        HGDIOBJ ob = SelectObject(b, g.dimBrush);
        PatBlt(b, 0, 0, cw, ch, ROP_DPA);
        SelectObject(b, ob);

        // Selected area shown undimmed with a red border
        if (g.drag) {
            RECT s = SelRect();
            int sw = s.right - s.left;
            int sh = s.bottom - s.top;
            if (sw > 0 && sh > 0) {
                BitBlt(b, s.left, s.top, sw, sh, g.scr.dc, s.left, s.top, SRCCOPY);
                HGDIOBJ op  = SelectObject(b, g.pen);
                HGDIOBJ ob2 = SelectObject(b, GetStockObject(NULL_BRUSH));
                Rectangle(b, s.left, s.top, s.right, s.bottom);
                SelectObject(b, ob2);
                SelectObject(b, op);
            }
        }
    }

    RestoreDC(b, saved);
    BitBlt(wndDc, area->left, area->top, area->right - area->left,
           area->bottom - area->top, b, area->left, area->top, SRCCOPY);
}

// Cancels the current selection and returns to the previous capture, if any
static void CancelSelection(void) {
    if (g.drag) {
        g.drag = false;
        ReleaseCapture();
    }
    Surf_Free(&g.scr);
    Surf_Free(&g.back);
    ShowWindow(g.sel, SW_HIDE);
    ShowEditWindow();   // with no capture, this closes the program
}

static void BeginSelection(void) {
    Surf_Free(&g.scr);
    Surf_Free(&g.back);

    HDC desk = GetDC(NULL);
    bool ok = desk && Surf_New(&g.scr, desk, g.vw, g.vh);
    if (ok) BitBlt(g.scr.dc, 0, 0, g.vw, g.vh, desk, g.vx, g.vy, SRCCOPY);
    if (desk) ReleaseDC(NULL, desk);

    if (!ok) {
        MessageBoxW(NULL, ERR_CAPTURE, APP_NAME, MB_OK | MB_ICONERROR);
        ShowEditWindow();
        return;
    }

    g.edit = false;
    g.drag = false;
    SetWindowPos(g.sel, HWND_TOPMOST, g.vx, g.vy, g.vw, g.vh, SWP_NOACTIVATE);
    ShowWindow(g.sel, SW_SHOW);
    UpdateWindow(g.sel);
    SetForegroundWindow(g.sel);
    SetFocus(g.sel);
}

// Hides a window and waits until DWM has removed it from the screen
static void HideAndWait(HWND w) {
    SetWindowPos(w, NULL, -32000, -32000, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
    DwmFlush();
}

static void NewCapture(void) {
    HideAndWait(g.edt);   // capture only after the edit window is gone
    BeginSelection();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_KEYDOWN: {
        if (lp & KEY_REPEAT_BIT) return 0;
        if (wp == VK_ESCAPE) {
            if (g.edit) DestroyWindow(hwnd);
            else CancelSelection();
        }
        else if (wp == 'S' && g.edit && GetKeyState(VK_CONTROL) < 0) SaveFile(hwnd);
        else if (wp == 'N' && g.edit && GetKeyState(VK_CONTROL) < 0) NewCapture();
        return 0;
    }

    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            SetWindowTextW(hwnd, TITLE);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(g.edit ? LoadCursorW(NULL, IDC_ARROW) : g.cross);
            return TRUE;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        if (g.edit) InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_CAPTURECHANGED:
        g.drag = false;
        return 0;

    case WM_LBUTTONDOWN:
        if (g.edit) {
            CopyClip(hwnd);
            return 0;
        }
        g.drag = true;
        g.p1 = g.p2 = (POINT){ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        SetCapture(hwnd);
        return 0;

    case WM_RBUTTONDOWN:
        if (g.edit) CopyClip(hwnd);
        else CancelSelection();
        return 0;

    case WM_MOUSEMOVE:
        // Repaint only the old and new selection areas
        if (g.drag) {
            RECT before = SelRect();
            g.p2 = (POINT){ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT after = SelRect(), dirty;
            UnionRect(&dirty, &before, &after);
            InflateRect(&dirty, 4, 4);
            InvalidateRect(hwnd, &dirty, FALSE);
        }
        return 0;

    case WM_LBUTTONUP: {
        if (!g.drag) return 0;
        g.drag = false;
        ReleaseCapture();

        RECT s = SelRect();
        int w = s.right - s.left;
        int h = s.bottom - s.top;
        Surface nc;
        if (w <= 0 || h <= 0 || !Surf_New(&nc, g.scr.dc, w, h)) {
            CancelSelection();
            return 0;
        }
        BitBlt(nc.dc, 0, 0, w, h, g.scr.dc, s.left, s.top, SRCCOPY);
        Surf_Free(&g.cap);
        g.cap = nc;
        Surf_Free(&g.scaled);

        Surf_Free(&g.scr);
        Surf_Free(&g.back);
        ShowWindow(g.sel, SW_HIDE);
        ShowEditWindow();
        CopyClip(g.edt);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Render(hwnd, dc, &ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        if (hwnd == g.edt) {
            Surf_Free(&g.cap);
            Surf_Free(&g.scaled);
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void WINAPI WinMainCRTStartup(void) {
    g.inst = GetModuleHandleW(NULL);

    // Single instance: bring the existing edit window to the front and exit
    HANDLE mtx = CreateMutexW(NULL, FALSE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(EDT_CLASS, NULL);
        if (prev) {
            if (IsIconic(prev)) ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
        ExitProcess(0);
    }

    SetProcessDPIAware();

    ULONG_PTR token = 0;
    GdipIn in = { .Version = 1 };
    if (GdiplusStartup(&token, &in, NULL) != 0)
        Fail(ERR_GDIP);

    g.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    static const WORD pat[DIM_PATTERN_ROWS] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
    HBITMAP patBmp = CreateBitmap(DIM_PATTERN_ROWS, DIM_PATTERN_ROWS, 1, 1, pat);
    g.dimBrush = CreatePatternBrush(patBmp);
    DeleteObject(patBmp);

    g.pen     = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
    g.bgBrush = CreateSolidBrush(RGB(40, 40, 40));
    g.cross   = LoadCursorW(NULL, IDC_CROSS);

    WNDCLASSW wc = {
        .lpfnWndProc   = WndProc,
        .hInstance     = g.inst,
        .hCursor       = g.cross,
        .lpszClassName = WND_CLASS
    };

    WNDCLASSW wce = {
        .lpfnWndProc   = WndProc,
        .hInstance     = g.inst,
        .hCursor       = g.cross,
        .lpszClassName = EDT_CLASS
    };

    if (!g.cross || !RegisterClassW(&wc) || !RegisterClassW(&wce))
        Fail(ERR_CLASS);

    g.sel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WND_CLASS, TITLE,
                            WS_POPUP, g.vx, g.vy, g.vw, g.vh,
                            NULL, NULL, g.inst, NULL);
    g.edt = CreateWindowExW(WS_EX_APPWINDOW, EDT_CLASS, TITLE,
                            WS_OVERLAPPEDWINDOW, 0, 0, 400, 300,
                            NULL, NULL, g.inst, NULL);
    if (!g.sel || !g.edt) Fail(ERR_WINDOW);

    BOOL disableAnim = TRUE;
    DwmSetWindowAttribute(g.edt, DWMWA_TRANSITIONS_FORCEDISABLED, &disableAnim, sizeof disableAnim);

    BeginSelection();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Surf_Free(&g.scr);
    Surf_Free(&g.back);
    Surf_Free(&g.cap);
    Surf_Free(&g.scaled);
    if (g.pen)      DeleteObject(g.pen);
    if (g.dimBrush) DeleteObject(g.dimBrush);
    if (g.bgBrush)  DeleteObject(g.bgBrush);
    GdiplusShutdown(token);
    ExitProcess(0);
}

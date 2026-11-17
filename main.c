#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Enlaces para exportar HBITMAP a archivos */
typedef struct { UINT32 Version; void* Cb; BOOL NoThread, NoCodecs; } GdipIn;

extern int  __stdcall GdiplusStartup(ULONG_PTR*, const GdipIn*, void*);
extern void __stdcall GdiplusShutdown(ULONG_PTR);
extern int  __stdcall GdipCreateBitmapFromHBITMAP(HBITMAP, HPALETTE, void**);
extern int  __stdcall GdipSaveImageToFile(void*, const WCHAR*, const CLSID*, const void*);
extern int  __stdcall GdipDisposeImage(void*);

/* Sustituyen a la CRT porque se usa -nostdlib */
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

/* Lienzo en memoria para dibujar con GDI */
typedef struct {
    HDC dc;         /* contexto de dispositivo en memoria */
    HBITMAP bmp;    /* bitmap donde se dibuja */
    HGDIOBJ old;    /* objeto GDI original del DC */
    int w, h;       /* ancho y alto en pixeles */
} Surface;

/* Libera el DC y el bitmap de una superficie */
static void Surf_Free(Surface* s) {
    if (s->dc) {
        if (s->old) SelectObject(s->dc, s->old);
        DeleteDC(s->dc);
    }
    if (s->bmp) DeleteObject(s->bmp);
    memset(s, 0, sizeof *s);
}

/* Crea una superficie de w x h compatible con ref, en negro */
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

/* ==========================================================================
 * Estado global
 * ========================================================================== */

static struct {
    Surface scr;        /* captura de todo el escritorio */
    Surface dim;        /* escritorio oscurecido */
    Surface back;       /* bufer de dibujo para WM_PAINT */
    Surface cap;        /* recorte final a resolucion completa */
    HPEN pen;           /* borde rojo de la seleccion */
    HBRUSH dimBrush;    /* patron de rayas para oscurecer */
    HBRUSH bgBrush;     /* fondo gris de la vista de edicion */
    HCURSOR cross;      /* cursor de cruz */
    int vx, vy;         /* esquina superior izquierda del escritorio virtual */
    int vw, vh;         /* ancho y alto del escritorio virtual */
    POINT p1, p2;       /* esquinas de la seleccion */
    bool drag;          /* true mientras se arrastra el raton */
    bool edit;          /* true cuando ya hay recorte y se edita */
} g;

/* ==========================================================================
 * Utilidades
 * ========================================================================== */

static int IMin(int a, int b) { return a < b ? a : b; }
static int IMax(int a, int b) { return a > b ? a : b; }

/* Rectangulo normalizado a partir de las dos esquinas de la seleccion */
static RECT SelRect(void) {
    RECT r = { IMin(g.p1.x, g.p2.x), IMin(g.p1.y, g.p2.y),
               IMax(g.p1.x, g.p2.x), IMax(g.p1.y, g.p2.y) };
    return r;
}

/* Muestra un error y termina el programa */
static void Fail(const WCHAR* msg) {
    MessageBoxW(NULL, msg, L"Recorte", MB_ICONERROR);
    ExitProcess(1);
}

/* Imagen oscurecida del escritorio que se muestra al seleccionar */
static void BuildDim(void) {
    BitBlt(g.dim.dc, 0, 0, g.vw, g.vh, g.scr.dc, 0, 0, SRCCOPY);
    SetTextColor(g.dim.dc, RGB(0, 0, 0));
    SetBkColor(g.dim.dc, RGB(255, 255, 255));
    HGDIOBJ ob = SelectObject(g.dim.dc, g.dimBrush);
    PatBlt(g.dim.dc, 0, 0, g.vw, g.vh, 0x00A000C9);
    SelectObject(g.dim.dc, ob);
}

/* Copia el recorte al portapapeles*/
static void CopyClip(HWND hwnd) {
    if (!g.cap.bmp) return;

    DWORD stride  = (DWORD)(((g.cap.w * 24 + 31) / 32) * 4);
    DWORD imgSize = stride * (DWORD)g.cap.h;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + imgSize);
    if (!hMem) return;

    BITMAPINFO* bi = (BITMAPINFO*)GlobalLock(hMem);
    if (!bi) { GlobalFree(hMem); return; }

    memset(bi, 0, sizeof(BITMAPINFOHEADER));
    bi->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi->bmiHeader.biWidth       = g.cap.w;
    bi->bmiHeader.biHeight      = g.cap.h;
    bi->bmiHeader.biPlanes      = 1;
    bi->bmiHeader.biBitCount    = 24;
    bi->bmiHeader.biCompression = BI_RGB;
    bi->bmiHeader.biSizeImage   = imgSize;

    int lines = GetDIBits(g.cap.dc, g.cap.bmp, 0, g.cap.h,
                          (BYTE*)bi + sizeof(BITMAPINFOHEADER), bi, DIB_RGB_COLORS);
    GlobalUnlock(hMem);

    bool ok = false;
    if (lines > 0 && OpenClipboard(hwnd)) {
        EmptyClipboard();
        ok = SetClipboardData(CF_DIB, hMem) != NULL;
        CloseClipboard();
    }
    if (!ok) GlobalFree(hMem);
}

static bool HasExt(const WCHAR* p) {
    bool ext = false;
    for (; *p; ++p) {
        if (*p == L'.') ext = true;
        else if (*p == L'\\' || *p == L'/') ext = false;
    }
    return ext;
}

/* Dialogo "Guardar como" y guardado del recorte en PNG o JPG */
static void SaveFile(HWND hwnd) {
    /* CLSID de los codificadores PNG y JPEG de GDI+ */
    static const CLSID pngId = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    static const CLSID jpgId = { 0x557cf401, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

    if (!g.cap.bmp) return;

    WCHAR path[MAX_PATH] = L"Captura.png";
    OPENFILENAMEW ofn = {
        .lStructSize  = sizeof ofn,
        .hwndOwner    = hwnd,
        .lpstrFilter  = L"PNG (*.png)\0*.png\0JPEG (*.jpg)\0*.jpg\0",
        .nFilterIndex = 1,                       /* 1 = PNG, 2 = JPG */
        .lpstrFile    = path,
        .nMaxFile     = MAX_PATH,
        .Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST
    };
    if (!GetSaveFileNameW(&ofn)) return;

    bool isJpg = (ofn.nFilterIndex == 2);
    if (!HasExt(path)) {
        int len = lstrlenW(path);
        if (len + 5 < MAX_PATH) lstrcatW(path, isJpg ? L".jpg" : L".png");
    }

    void* img = NULL;
    if (GdipCreateBitmapFromHBITMAP(g.cap.bmp, NULL, &img) == 0 && img) {
        int st = GdipSaveImageToFile(img, path, isJpg ? &jpgId : &pngId, NULL);
        GdipDisposeImage(img);
        if (st != 0) MessageBoxW(hwnd, L"No se pudo guardar el archivo.", L"Recorte", MB_ICONERROR);
    }
}

/* Tamaño de visualizacion es de maximo 80% del area de trabajo, con proporcion.
   Las imagenes pequenas no se escalan */
static void EditSize(int* dw, int* dh) {
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    int maxW = (wa.right - wa.left) * 8 / 10;
    int maxH = (wa.bottom - wa.top) * 8 / 10;
    int w = g.cap.w, h = g.cap.h;

    if (w <= maxW && h <= maxH) { *dw = w; *dh = h; return; }

    if (w * maxH > h * maxW) { *dw = maxW; *dh = h * maxW / w; }  /* limita por ancho */
    else                     { *dh = maxH; *dw = w * maxH / h; }  /* limita por alto  */
}

/* En la seleccion el escritorio oscurece con el recorte resaltado*/
static void Render(HWND hwnd, HDC wndDc, const RECT* area) {
    RECT cr;
    GetClientRect(hwnd, &cr);

    int cw = g.edit ? cr.right  : g.vw;
    int ch = g.edit ? cr.bottom : g.vh;

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
        int ix = (cw - dw) / 2;
        int iy = (ch - dh) / 2;

        FillRect(b, &cr, g.bgBrush);
        SetStretchBltMode(b, HALFTONE);
        SetBrushOrgEx(b, 0, 0, NULL);
        StretchBlt(b, ix, iy, dw, dh, g.cap.dc, 0, 0, g.cap.w, g.cap.h, SRCCOPY);
    } else {
        BitBlt(b, 0, 0, cw, ch, g.dim.dc, 0, 0, SRCCOPY);

        /* La zona seleccionada se muestra sin oscurecer, con borde rojo */
        if (g.drag) {
            RECT s = SelRect();
            int sw = s.right - s.left;
            int sh = s.bottom - s.top;
            if (sw > 0 && sh > 0) {
                BitBlt(b, s.left, s.top, sw, sh, g.scr.dc, s.left, s.top, SRCCOPY);
                HGDIOBJ op = SelectObject(b, g.pen);
                HGDIOBJ ob = SelectObject(b, GetStockObject(NULL_BRUSH));
                Rectangle(b, s.left, s.top, s.right, s.bottom);
                SelectObject(b, ob);
                SelectObject(b, op);
            }
        }
    }

    RestoreDC(b, saved);
    BitBlt(wndDc, area->left, area->top, area->right - area->left,
           area->bottom - area->top, b, area->left, area->top, SRCCOPY);
}

/* ==========================================================================
 * Procedimiento de ventana
 * ========================================================================== */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        else if (wp == 'S' && g.edit && GetKeyState(VK_CONTROL) < 0) SaveFile(hwnd);
        return 0;

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
        else        DestroyWindow(hwnd);
        return 0;

    case WM_MOUSEMOVE:
        /* Repinta solo la zona de la seleccion anterior y la nueva */
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
        /* Recorta, pasa a modo edicion y dimensiona la ventana una sola vez */
        if (!g.drag) return 0;
        g.drag = false;
        ReleaseCapture();

        RECT s = SelRect();
        int w = s.right - s.left;
        int h = s.bottom - s.top;
        if (w <= 0 || h <= 0 || !Surf_New(&g.cap, g.scr.dc, w, h)) {
            DestroyWindow(hwnd);
            return 0;
        }
        BitBlt(g.cap.dc, 0, 0, w, h, g.scr.dc, s.left, s.top, SRCCOPY);

        Surf_Free(&g.scr);
        Surf_Free(&g.dim);
        Surf_Free(&g.back);
        g.edit = true;
        ShowWindow(hwnd, SW_HIDE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
        SetWindowTextW(hwnd, L"Herramienta Recortes (Guardar: Ctrl+S | Copiar: click | Escape: Esc)");
        SetClassLongPtrW(hwnd, GCLP_HCURSOR, (LONG_PTR)LoadCursorW(NULL, IDC_ARROW));
        SetCursor(LoadCursorW(NULL, IDC_ARROW));

        int dw, dh;
        EditSize(&dw, &dh);
        RECT wr = { 0, 0, dw + 40, dh + 40 };
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        int ww = IMax(360, wr.right - wr.left);
        int wh = IMax(240, wr.bottom - wr.top);

        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        SetWindowPos(hwnd, HWND_NOTOPMOST,
                     wa.left + (wa.right - wa.left - ww) / 2,
                     wa.top  + (wa.bottom - wa.top - wh) / 2,
                     ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        ShowWindow(hwnd, SW_SHOW);

        CopyClip(hwnd);
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
        Surf_Free(&g.scr);
        Surf_Free(&g.dim);
        Surf_Free(&g.back);
        Surf_Free(&g.cap);
        DeleteObject(g.pen);
        DeleteObject(g.dimBrush);
        DeleteObject(g.bgBrush);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

void WINAPI WinMainCRTStartup(void) {
    SetProcessDPIAware();
    ULONG_PTR token = 0;
    GdipIn in = { .Version = 1 };
    if (GdiplusStartup(&token, &in, NULL) != 0)
        Fail(L"No se pudo iniciar GDI+.");

    g.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC desk = GetDC(NULL);
    bool ok = desk && Surf_New(&g.scr, desk, g.vw, g.vh) && Surf_New(&g.dim, desk, g.vw, g.vh) && Surf_New(&g.back, desk, g.vw, g.vh);
    if (ok) BitBlt(g.scr.dc, 0, 0, g.vw, g.vh, desk, g.vx, g.vy, SRCCOPY);
    if (desk) ReleaseDC(NULL, desk);
    if (!ok) Fail(L"No se pudo capturar la pantalla.");

    /* Patron de rayas 8x8 cuando la ventana se este seleccionando */
    static const WORD pat[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
    HBITMAP patBmp = CreateBitmap(8, 8, 1, 1, pat);
    g.dimBrush = CreatePatternBrush(patBmp);
    DeleteObject(patBmp);
    BuildDim();

    g.pen     = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
    g.bgBrush = CreateSolidBrush(RGB(40, 40, 40));
    g.cross   = LoadCursorW(NULL, IDC_CROSS);

    WNDCLASSW wc = {
        .lpfnWndProc   = WndProc,
        .hCursor       = g.cross,
        .lpszClassName = L"SnipWnd"
    };
    if (!g.cross || !RegisterClassW(&wc))
        Fail(L"No se pudo registrar la ventana.");

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"SnipWnd", L"",
                                WS_POPUP | WS_VISIBLE,
                                g.vx, g.vy, g.vw, g.vh,
                                NULL, NULL, NULL, NULL);
    if (!hwnd) Fail(L"No se pudo crear la ventana.");

    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(token);
    ExitProcess(0);
}

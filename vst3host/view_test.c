/* Opens the editor of OpenMPC Host.vst3 in a window, saves a BMP of each tab. view_test <module.vst3> <out prefix> */
#include "vst3_abi.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
static void pump(int ms) { DWORD t = GetTickCount(); MSG m; while (GetTickCount() - t < (DWORD)ms) { while (PeekMessage(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); } Sleep(10); } }
static void shot(HWND w, const char *path, int cw, int ch) {
    HDC dc = GetDC(w), m = CreateCompatibleDC(dc);
    HBITMAP b = CreateCompatibleBitmap(dc, cw, ch); SelectObject(m, b);
    SendMessage(w, WM_PRINTCLIENT, (WPARAM)m, PRF_CLIENT);   /* synchronous: the view paints itself into our bitmap */
    BITMAPINFOHEADER bi = {sizeof bi, cw, ch, 1, 24, 0}; int row = (cw * 3 + 3) & ~3;
    unsigned char *px = malloc(row * ch); GetDIBits(m, b, 0, ch, px, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh = {0x4d42, sizeof fh + sizeof bi + row * ch, 0, 0, sizeof fh + sizeof bi};
    FILE *f = fopen(path, "wb"); fwrite(&fh, sizeof fh, 1, f); fwrite(&bi, sizeof bi, 1, f); fwrite(px, row, ch, f); fclose(f);
    free(px); DeleteDC(m); DeleteObject(b); ReleaseDC(w, dc);
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) { printf("load failed %lu\n", GetLastError()); return 1; }
    const FactoryVtbl **(*gf)(void) = (void *)GetProcAddress(lib, "GetPluginFactory");
    const FactoryVtbl **f = gf(); PClassInfo ci; (*f)->getClassInfo(f, 0, &ci);
    void *obj = 0; (*f)->createInstance(f, (const char *)ci.cid, (const char *)iid_IComponent, &obj);
    struct { const ComponentVtbl *v; } *c = obj; c->v->initialize(c, 0);
    void *ctl = 0; c->v->queryInterface(c, iid_IEditController, &ctl);
    struct { const ControllerVtbl *v; } *ct = ctl;
    struct { const PlugViewVtbl *v; } *view = ct->v->createView(ct, "editor");
    if (!view) { printf("no view\n"); return 1; }
    HWND w = CreateWindowA("STATIC", "host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 20, 20, 1000, 640, 0, 0, 0, 0);
    ViewRect r; view->v->getSize(view, &r);
    printf("view %dx%d\n", r.right, r.bottom);
    if (view->v->attached(view, w, "HWND") != kResultOk) { printf("attach failed\n"); return 1; }
    char p[512];
    pump(3000); snprintf(p, sizeof p, "%s-plugin.bmp", argv[2]); shot(GetWindow(w, GW_CHILD), p, r.right, r.bottom);
    HWND v = GetWindow(w, GW_CHILD);
    if (argc > 3 && !strcmp(argv[3], "skin")) {   /* DIR: REV, then drag the Density knob up, then page EDIT A */
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(785, 327)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(785, 327)); pump(300);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(103, 318)); for (int y = 318; y > 270; y -= 4) SendMessage(v, WM_MOUSEMOVE, 0, MAKELPARAM(103, y)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(103, 270)); pump(600);
        snprintf(p, sizeof p, "%s-skinclick.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(245, 56)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(245, 56)); pump(2500);
        snprintf(p, sizeof p, "%s-skinedit.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        view->v->removed(view); printf("ok\n"); return 0;
    }
    SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(300, 120)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(300, 120)); pump(300);   /* Mode: 8 knobs */
    snprintf(p, sizeof p, "%s-8.bmp", argv[2]); shot(v, p, r.right, r.bottom); printf("v=%p visible=%d\n", (void*)v, IsWindowVisible(v));
    SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(540, 153)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(540, 153)); pump(1500);   /* next page */
    printf("after page: visible=%d child=%p\n", IsWindowVisible(v), (void*)GetWindow(w, GW_CHILD)); snprintf(p, sizeof p, "%s-mode.bmp", argv[2]); shot(v, p, r.right, r.bottom);
    printf("click tab -> %lld\n", (long long)SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(480, 20))); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(480, 20)); pump(300);   /* Manager tab */
    SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(60, 60)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(60, 60));    /* Refresh */
    pump(6000); snprintf(p, sizeof p, "%s-manager.bmp", argv[2]); shot(v, p, r.right, r.bottom);
    if (argc > 3) {   /* install the 4th catalog row ("Acid"), then look at the Plugin tab */
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(100, 84 + 3 * 24 + 5)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(100, 161));
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(170, 60)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(170, 60));
        pump(25000); snprintf(p, sizeof p, "%s-status.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(380, 20)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(380, 20));
        pump(4000); snprintf(p, sizeof p, "%s-after.bmp", argv[2]); shot(v, p, r.right, r.bottom);
    }
    view->v->removed(view);
    printf("ok\n");
    return 0;
}

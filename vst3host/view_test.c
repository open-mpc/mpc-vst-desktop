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
/* On Windows a host passes interface IDs in the COM layout (the first three GUID fields little-endian): swap vst3_abi.h's plain big-endian ones as a DAW would. */
static const unsigned char *com(const unsigned char *b) {
    static unsigned char t[8][16]; static int n;
    static const int map[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    unsigned char *c = t[n++ & 7];
    for (int i = 0; i < 16; i++) c[i] = b[map[i]];
    return c;
}
#define CL(x, y) MAKELPARAM((int)((x) * sc), (int)((y) * sc))
/* audio: a DAW calls process() all the time; the hosted plugin's housekeeping (trigger spring-back, its own state) runs inside it */
static void *g_proc, *g_comp;
static DWORD WINAPI audio_thread(LPVOID arg) {
    (void)arg;
    struct { const ProcessorVtbl *v; } *p = g_proc;
    float L[512], R[512], *ch[2] = {L, R};
    AudioBusBuffers ob = {2, 0, ch};
    ProcessData pd = {0, 0, 512, 0, 1, 0, &ob, 0, 0, 0, 0, 0};
    for (;;) { p->v->process(p, &pd); Sleep(10); }
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) { printf("load failed %lu\n", GetLastError()); return 1; }
    const FactoryVtbl **(*gf)(void) = (void *)GetProcAddress(lib, "GetPluginFactory");
    const FactoryVtbl **f = gf(); PClassInfo ci; (*f)->getClassInfo(f, 0, &ci);
    void *obj = 0; (*f)->createInstance(f, (const char *)ci.cid, (const char *)com(iid_IComponent), &obj);
    struct { const ComponentVtbl *v; } *c = obj; c->v->initialize(c, 0);
    void *ctl = 0; c->v->queryInterface(c, com(iid_IEditController), &ctl);
    struct { const ControllerVtbl *v; } *ct = ctl;
    { void *pr = 0; c->v->queryInterface(c, com(iid_IAudioProcessor), &pr); struct { const ProcessorVtbl *v; } *pp = pr; ProcessSetup ps = {0, 0, 512, 44100}; pp->v->setupProcessing(pp, &ps); c->v->setActive(c, 1); g_proc = pr; CreateThread(0, 0, audio_thread, 0, 0, 0); }
    struct { const PlugViewVtbl *v; } *view = ct->v->createView(ct, "editor");
    if (!view) { printf("no view\n"); return 1; }
    HWND w = CreateWindowA("STATIC", "host", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 10, 0, 1420, 960, 0, 0, 0, 0);
    ViewRect r; view->v->getSize(view, &r);
    printf("view %dx%d\n", r.right, r.bottom);
    if (view->v->attached(view, w, "HWND") != kResultOk) { printf("attach failed\n"); return 1; }
    char p[512];
    pump(3000); snprintf(p, sizeof p, "%s-plugin.bmp", argv[2]); shot(GetWindow(w, GW_CHILD), p, r.right, r.bottom);
    HWND v = GetWindow(w, GW_CHILD);
    if (argc > 3 && !strcmp(argv[3], "mgr2")) {   /* the manager's own install: INSTALL Keyscope, APPLY twice, wait for the restart */
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(485, 20)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(485, 20)); pump(12000);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(840, 228)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(840, 228)); pump(2000);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(853, 480)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(853, 480)); pump(15000);
        snprintf(p, sizeof p, "%s-mgr2a.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(853, 480)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(853, 480)); pump(20000);
        snprintf(p, sizeof p, "%s-mgr2.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(853, 480)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(853, 480)); pump(8000);
        snprintf(p, sizeof p, "%s-mgr3.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(590, 412)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(590, 412)); pump(15000);   /* RESTART & APPLY in the prompt */
        snprintf(p, sizeof p, "%s-mgr4.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        view->v->removed(view); printf("ok\n"); return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "q")) {   /* the faceplate: coordinates are logical (1348 wide for a 1280 skin); the window may be scaled down */
        const double sc = r.right / 1348.0;
        printf("window %dx%d scale %.2f\n", r.right, r.bottom, sc);
        pump(3000);
        snprintf(p, sizeof p, "%s-q4.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        SendMessage(v, WM_LBUTTONDOWN, 0, CL(245, 773)); SendMessage(v, WM_LBUTTONUP, 0, CL(245, 773)); pump(800);   /* bank 3 */
        SendMessage(v, WM_LBUTTONDOWN, 0, CL(474, 782)); for (int y = 782; y > 742; y -= 5) SendMessage(v, WM_MOUSEMOVE, 0, CL(474, y)); SendMessage(v, WM_LBUTTONUP, 0, CL(474, 742)); pump(600);   /* knob 1 up */
        snprintf(p, sizeof p, "%s-qb3.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        SendMessage(v, WM_LBUTTONDOWN, 0, CL(162, 816)); SendMessage(v, WM_LBUTTONUP, 0, CL(162, 816)); pump(1200);   /* 8 */
        snprintf(p, sizeof p, "%s-q8.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        SendMessage(v, WM_LBUTTONDOWN, 0, CL(347, 725)); SendMessage(v, WM_LBUTTONUP, 0, CL(347, 725)); pump(2500);   /* function key: EDIT A */
        snprintf(p, sizeof p, "%s-qe.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        SendMessage(v, WM_LBUTTONDOWN, 0, CL(360, 22)); SendMessage(v, WM_LBUTTONUP, 0, CL(360, 22)); pump(800);   /* the plugin list */
        snprintf(p, sizeof p, "%s-qm.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        view->v->removed(view); printf("ok\n"); return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "sc")) {   /* the host resizes the editor to 75%: it must redraw scaled and map clicks back */
        ViewRect nr = {0, 0, 1011, 657};
        view->v->checkSizeConstraint(view, &nr); printf("constraint -> %dx%d\n", nr.right, nr.bottom);
        view->v->onSize(view, &nr); pump(1500);
        const double sc = nr.right / 1348.0;
        SendMessage(v, WM_LBUTTONDOWN, 0, CL(245, 773)); SendMessage(v, WM_LBUTTONUP, 0, CL(245, 773)); pump(800);   /* bank 3 */
        snprintf(p, sizeof p, "%s-sc.bmp", argv[2]); shot(v, p, nr.right, nr.bottom);
        view->v->removed(view); printf("ok\n"); return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "sw")) {   /* start on the manager's skin, then switch to the second plugin: its skin must not reuse the first one's pictures */
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(485, 20)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(485, 20)); pump(4000);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(380, 20)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(380, 20)); pump(1000);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(100, 108)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(100, 108)); pump(8000);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(485, 20)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(485, 20)); pump(5000);
        snprintf(p, sizeof p, "%s-sw.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        view->v->removed(view); printf("ok\n"); return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "mgr")) {   /* the stock manager's own screen on the Skin tab */
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(485, 20)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(485, 20)); pump(15000);
        snprintf(p, sizeof p, "%s-mgr.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        view->v->removed(view); printf("ok\n"); return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "skin")) {   /* DIR: REV, then drag the Density knob up, then page EDIT A */
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(785, 295)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(785, 295)); pump(300);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(103, 286)); for (int y = 286; y > 238; y -= 4) SendMessage(v, WM_MOUSEMOVE, 0, MAKELPARAM(103, y)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(103, 238)); pump(600);
        snprintf(p, sizeof p, "%s-skinclick.bmp", argv[2]); shot(v, p, r.right, r.bottom);
        SendMessage(v, WM_LBUTTONDOWN, 0, MAKELPARAM(245, 539)); SendMessage(v, WM_LBUTTONUP, 0, MAKELPARAM(245, 539)); pump(2500);
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

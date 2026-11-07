/* Plugin switching in OpenMPC Host.vst3: sets the "Plugin" list parameter, waits for the worker, checks the parameter list changed and the new plugin sounds.
 *   switch_test <module> <plugin name to switch to> */
#include "vst3_abi.h"
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
typedef struct { const IComponentHandlerVtbl *v; int restarts; } H;
static tresult hq(void *s, const TUID i, void **o) { (void)s; (void)i; *o = 0; return kNoInterface; }
static uint32_t h1(void *s) { (void)s; return 1; }
static tresult hb(void *s, uint32_t i) { (void)s; (void)i; return 0; }
static tresult hp(void *s, uint32_t i, double v) { (void)s; (void)i; (void)v; return 0; }
static tresult hr(void *s, int32_t f) { ((H *)s)->restarts++; (void)f; return 0; }
static const IComponentHandlerVtbl hv = {hq, h1, h1, hb, hp, hb, hr};
static int ev_count(void *s) { (void)s; return 1; }
static tresult ev_get(void *s, int32_t i, Event *e) { (void)s; (void)i; memset(e, 0, sizeof *e); e->type = kNoteOnEvent; e->noteOn.pitch = 60; e->noteOn.velocity = 0.8f; return 0; }
static tresult ev_add(void *s, Event *e) { (void)s; (void)e; return 0; }
static const IEventListVtbl evv = {hq, h1, h1, ev_count, ev_get, ev_add};
static void a(char *d, const char16 *s) { int i = 0; for (; s[i] && i < 127; i++) d[i] = (char)s[i]; d[i] = 0; }
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
    const FactoryVtbl **(*gf)(void) = dlsym(lib, "GetPluginFactory");
    const FactoryVtbl **f = gf();
    PClassInfo ci; (*f)->getClassInfo(f, 0, &ci);
    void *obj = 0;
    (*f)->createInstance(f, (const char *)ci.cid, (const char *)iid_IComponent, &obj);
    struct { const ComponentVtbl *v; } *c = obj;
    c->v->initialize(c, 0);
    void *ctl = 0, *pr = 0;
    c->v->queryInterface(c, iid_IEditController, &ctl);
    c->v->queryInterface(c, iid_IAudioProcessor, &pr);
    struct { const ControllerVtbl *v; } *ct = ctl;
    struct { const ProcessorVtbl *v; } *p = pr;
    H h = {&hv, 0};
    ct->v->setComponentHandler(ct, (IComponentHandler *)&h);
    ProcessSetup ps = {0, 0, 512, 44100};
    p->v->setupProcessing(p, &ps);
    c->v->setActive(c, 1);
    int n0 = ct->v->getParameterCount(ct), pid = -1;
    for (int i = 0; i < n0; i++) { ParameterInfo pi; ct->v->getParameterInfo(ct, i, &pi); char t[128]; a(t, pi.title); if (!strcmp(t, "Plugin")) pid = (int)pi.id; }
    printf("before: %d parameters, Plugin id %d\n", n0, pid);
    int steps = 0;
    for (int i = 0; i < n0; i++) { ParameterInfo pi; ct->v->getParameterInfo(ct, i, &pi); if ((int)pi.id == pid) steps = pi.stepCount; }
    int found = -1;
    for (int k = 0; k <= steps; k++) { char16 s16[128]; char s[128]; ct->v->getParamStringByValue(ct, pid, steps ? (double)k / steps : 0, s16); a(s, s16); printf("  plugin %d: %s\n", k, s); if (!strcmp(s, argv[2])) found = k; }
    if (found < 0) { printf("FAIL: %s not offered\n", argv[2]); return 1; }
    ct->v->setParamNormalized(ct, pid, (double)found / steps);
    for (int i = 0; i < 100 && !h.restarts; i++) usleep(100000);
    int n1 = ct->v->getParameterCount(ct);
    printf("after: %d parameters, restartComponent %d times\n", n1, h.restarts);
    float L[512], R[512], *ch[2] = {L, R}; AudioBusBuffers ob = {2, 0, ch};
    IEventList el = {&evv}; ProcessData pd = {0, 0, 512, 0, 1, 0, &ob, 0, 0, &el, 0, 0};
    double e = 0;
    for (int b = 0; b < 200; b++) { p->v->process(p, &pd); pd.inputEvents = b ? 0 : &el; for (int i = 0; i < 512; i++) e += L[i] * L[i]; }
    printf("energy %.4f\n", e);
    int ok = h.restarts && n1 != n0 && e > 0.001;
    printf(ok ? "ok\n" : "FAIL\n");
    return !ok;
}

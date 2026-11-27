/* SetPstates20 the way Afterburner calls it: one P-state (P0), one clock
 * domain, delta = the current delta (no change). Root only. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef void *(*qip_t)(uint32_t);
typedef int (*f0)(void);
typedef int (*f2)(void *, void *);
typedef int (*ferr)(int, char *);

#pragma pack(push, 1)
struct clk { uint32_t domain, type, flags; int32_t delta, dmin, dmax; uint32_t d[5]; };
struct bv { uint32_t domain, flags, uv; int32_t delta, dmin, dmax; };
struct ps { uint32_t id, flags; struct clk clk[8]; struct bv bv[4]; };
struct p20 { uint32_t ver, flags, nps, nclk, nbv; struct ps ps[16]; };
#pragma pack(pop)

int main(int argc, char **argv)
{
    void *lib = dlopen(argv[1], RTLD_NOW);
    qip_t qi = (qip_t)dlsym(lib, "nvapi_QueryInterface");
    ferr e = (ferr)qi(0x6C2D048C);
    char es[64];
    printf("sizeof p20 = 0x%zx\n", sizeof(struct p20));
    ((f0)qi(0x0150E828))();
    void *h[64] = {0}; uint32_t n = 0;
    ((f2)qi(0xE5AC921F))(h, &n);
    static struct p20 cur, set, after;
    cur.ver = sizeof(cur) | 0x10000;
    int st = ((f2)qi(0x6FF81213))(h[0], &cur);
    printf("GetPstates20 %d: nps %u nclk %u nbv %u\n", st, cur.nps, cur.nclk, cur.nbv);
    for (unsigned c = 0; c < cur.nclk; c++)
        printf("  P%u clk %u: domain %u type %u flags %x delta %d range %d..%d\n", cur.ps[0].id, c,
               cur.ps[0].clk[c].domain, cur.ps[0].clk[c].type, cur.ps[0].clk[c].flags,
               cur.ps[0].clk[c].delta, cur.ps[0].clk[c].dmin, cur.ps[0].clk[c].dmax);
    for (unsigned c = 0; c < cur.nclk; c++) {
        memset(&set, 0, sizeof(set));
        set.ver = sizeof(set) | 0x10000;
        set.nps = 1; set.nclk = 1; set.nbv = 0;
        set.ps[0].id = cur.ps[0].id;
        set.ps[0].clk[0].domain = cur.ps[0].clk[c].domain;
        set.ps[0].clk[0].delta = cur.ps[0].clk[c].delta;
        st = ((f2)qi(0x0F4DAE6B))(h[0], &set);
        e(st, es);
        memset(&after, 0, sizeof(after)); after.ver = sizeof(after) | 0x10000;
        ((f2)qi(0x6FF81213))(h[0], &after);
        printf("SetPstates20 P%u domain %u delta %d (same): %d %s; delta now %d\n", set.ps[0].id,
               set.ps[0].clk[0].domain, set.ps[0].clk[0].delta, st, es, after.ps[0].clk[c].delta);
    }
    return 0;
}

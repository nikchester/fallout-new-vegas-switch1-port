// Юнит-тест на Linux для native_math.cpp: каждая из 8 функций через точку входа f_XXXXXXXX (стек и ecx гостя)
// против эталонной формулы на обычных и граничных входах. Запуск: tools/test_native_math.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <vector>
#include "../fnv_recomp.h"
#include "../fnv_recomp_host.h"

extern "C" uintptr_t fnv_guest_base = 0;
extern "C" int fnv_weak_memory = 0;

void f_0043f8d0(FnvCpu&); void f_0062c250(FnvCpu&); void f_004b6190(FnvCpu&); void f_004b6160(FnvCpu&);
void f_004b6100(FnvCpu&); void f_0049da80(FnvCpu&); void f_00b9bbb0(FnvCpu&); void f_00b9f5b0(FnvCpu&);

// ---- заглушки гостя: виртуальные методы объектов-пустышек и AddGeometry ----------------------------------------
static const uint32_t kFnNode = 0x1000, kFnNotNode = 0x1004, kFnGeom = 0x1008, kFnNotGeom = 0x100c, kAddGeometry = 0xb9f480u;
static std::vector<uint32_t> g_added;
void fnv_host_log(const char* t) { printf("log: %s", t); }
uint32_t fnv_guest_call(FnvCpu& s, uint32_t fn, const uint32_t* args, int)
{
    switch (fn)
    {
    case kFnNode: return s.ecx;
    case kFnGeom: return s.ecx;
    case kFnNotNode: case kFnNotGeom: return 0;
    case kAddGeometry: g_added.push_back(args[0]); return 0;
    }
    printf("unexpected guest call %08x\n", fn);
    exit(2);
}

static int g_fail, g_checks;
#define CHECK(c) do { ++g_checks; if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static FnvCpu g_cpu;
static const uint32_t kStack = 0x00800000u;            // esp гостя
static const uint32_t kArena = 0x00900000u;            // данные теста

static void Call(void (*f)(FnvCpu&), uint32_t ecx, std::initializer_list<uint32_t> args)
{
    uint32_t sp = kStack - 4 * (uint32_t)args.size() - 4, i = 0;
    M32(sp) = 0xdeadbeefu;
    for (uint32_t a : args) M32(sp + 4 + 4 * i++) = a;
    g_cpu.esp = sp;
    g_cpu.ecx = ecx;
    g_cpu.depth = 0;
    f(g_cpu);
    CHECK(g_cpu.esp == kStack);                         // ret N снял return address и N аргументов
}

static uint32_t Put(uint32_t at, std::initializer_list<float> v)
{
    uint32_t i = 0;
    for (float f : v) MF(at + 4 * i++) = f;
    return at;
}
static bool Close(double a, double b) { return fabs(a - b) <= 1e-6 * (1.0 + fabs(b)); }

// эталон: long double, без привязки к порядку и округлениям ядра
static long double RefDot(const float* a, const float* b) { return (long double)a[0] * b[0] + (long double)a[1] * b[1] + (long double)a[2] * b[2]; }

static const float kVals[] = {0.f, -0.f, 1.f, -1.f, 0.5f, 3.25f, -7.75f, 1e-20f, -1e-20f, 1e10f, -1e10f, 123456.789f, 3e38f, 1e-38f};

int main()
{
    void* w = mmap(nullptr, (1ull << 32) + (1u << 20), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(w != MAP_FAILED);
    fnv_guest_base = ((uintptr_t)w + 0xffffu) & ~(uintptr_t)0xffffu;
    for (uint32_t a : {0x00700000u, 0x00800000u, 0x00900000u, 0x011f0000u})
        CHECK(mprotect((void*)(fnv_guest_base + a - 0x10000), 0x200000, PROT_READ | PROT_WRITE) == 0);
    memset((void*)FNV_G2H(0x00900000u), 0, 0x100000);

    // Dot / Distance: ST(0), все сочетания значений на осях
    uint32_t A = kArena, B = kArena + 0x20;
    srand(1);
    for (int n = 0; n < 4000; ++n)
    {
        float a[4], b[3];
        for (float& x : a) x = n < 2744 ? kVals[(n / 7 + (&x - a) * 3) % 14] : (rand() / (float)RAND_MAX - 0.5f) * 200.f;
        for (int k = 0; k < 3; ++k) b[k] = n < 2744 ? kVals[(n + k * 5) % 14] : (rand() / (float)RAND_MAX - 0.5f) * 200.f;
        Put(A, {a[0], a[1], a[2], a[3]});
        Put(B, {b[0], b[1], b[2]});
        long double d = RefDot(a, b);
        Call(f_004b6190, A, {B});
        CHECK(g_cpu.depth == 1);
        CHECK(Close(g_cpu.st[0], (double)d) || (isinf((double)d) && isinf(g_cpu.st[0])) || isnan((double)d));
        Call(f_004b6160, A, {B});
        double ref = (double)(d - a[3]);
        CHECK(g_cpu.depth == 1 && (Close(g_cpu.st[0], ref) || isinf(ref) || isnan(ref) || fabs(ref) > 1e18));
    }
    // известные значения
    Put(A, {1, 2, 3, 4}); Put(B, {4, 5, 6});
    Call(f_004b6190, A, {B}); CHECK(g_cpu.st[0] == 32.0);
    Call(f_004b6160, A, {B}); CHECK(g_cpu.st[0] == 28.0);
    Put(B, {NAN, 0, 0}); Call(f_004b6190, A, {B}); CHECK(isnan(g_cpu.st[0]));

    // WhichSide: сфера (центр, радиус), плоскость (нормаль, константа)
    struct Side { float c[3], r, pn[3], pc; uint32_t want; };
    const Side sides[] = {
        {{0, 0, 0}, 1, {0, 0, 1}, 5, 2},        // d = -5 <= -1: за плоскостью
        {{0, 0, 0}, 1, {0, 0, 1}, -5, 1},       // d = 5 >= 1: перед плоскостью
        {{0, 0, 0}, 1, {0, 0, 1}, 0, 0},        // пересекает
        {{0, 0, 0}, 1, {0, 0, 1}, 1, 2},        // граница: d == -r -> 2
        {{0, 0, 0}, 1, {0, 0, 1}, -1, 1},       // граница: d == r -> 1
        {{0, 0, 0}, 0, {0, 0, 1}, 0, 2},        // радиус 0 и d == 0: d <= -0 проверяется первым
        {{1, 2, 3}, 2, {1, 0, 0}, 0, 0},        // d = 1 в (-2, 2)
        {{1, 2, 3}, 0.5f, {0, 1, 0}, 1, 1},     // d = 1 >= 0.5
        {{-3, 0, 0}, 1, {1, 0, 0}, 0, 2},       // d = -3
        {{0, 0, 0}, -1, {0, 0, 1}, 0, 2},       // отрицательный радиус: d (0) <= 1
    };
    for (const Side& t : sides)
    {
        Put(A, {t.c[0], t.c[1], t.c[2], t.r});
        Put(B, {t.pn[0], t.pn[1], t.pn[2], t.pc});
        Call(f_004b6100, A, {B});
        CHECK(g_cpu.eax == t.want);
    }
    Put(A, {NAN, 0, 0, 1}); Put(B, {1, 0, 0, 0}); Call(f_004b6100, A, {B}); CHECK(g_cpu.eax == 0);    // NaN: оба сравнения ложны
    srand(2);
    for (int n = 0; n < 2000; ++n)
    {
        float c[3], p[4], r = fabsf((rand() / (float)RAND_MAX) * 10.f);
        for (float& x : c) x = (rand() / (float)RAND_MAX - 0.5f) * 40.f;
        for (float& x : p) x = (rand() / (float)RAND_MAX - 0.5f) * 40.f;
        Put(A, {c[0], c[1], c[2], r}); Put(B, {p[0], p[1], p[2], p[3]});
        long double d = RefDot(p, c) - p[3];
        uint32_t want = d <= -r - 1e-3L ? 2 : d >= r + 1e-3L ? 1 : (fabsl(fabsl(d) - r) <= 1e-3L ? 99 : 0);
        Call(f_004b6100, A, {B});
        CHECK(want == 99 || g_cpu.eax == want);
    }

    // ClassifyPoint: плоскость в начале this
    struct Cls { float pl[4], pt[3]; uint32_t want; };
    const Cls cls[] = {
        {{0, 0, 1, 0}, {0, 0, -1}, 2}, {{0, 0, 1, 0}, {0, 0, 1}, 1}, {{0, 0, 1, 0}, {5, 5, 0}, 0},
        {{1, 1, 1, 3}, {1, 1, 1}, 0}, {{1, 1, 1, 3}, {2, 2, 2}, 1}, {{1, 1, 1, 3}, {0, 0, 0}, 2},
        {{0, 0, 0, 0}, {1, 2, 3}, 0}, {{0, 0, 0, -1}, {1, 2, 3}, 1}, {{0, 0, 0, 1}, {1, 2, 3}, 2},
        {{1, 0, 0, 0}, {-1e-30f, 0, 0}, 2}, {{NAN, 0, 0, 0}, {1, 0, 0}, 0},
    };
    for (const Cls& t : cls)
    {
        Put(A, {t.pl[0], t.pl[1], t.pl[2], t.pl[3]}); Put(B, {t.pt[0], t.pt[1], t.pt[2]});
        Call(f_0049da80, A, {B});
        CHECK(g_cpu.eax == t.want);
    }

    // NiMatrix3::operator*: результат в [esp+4], eax = результат; в том числе dst == this и dst == other
    srand(3);
    for (int n = 0; n < 500; ++n)
    {
        float a[9], b[9];
        for (int k = 0; k < 9; ++k) { a[k] = (rand() / (float)RAND_MAX - 0.5f) * 4.f; b[k] = (rand() / (float)RAND_MAX - 0.5f) * 4.f; }
        if (n == 0) for (int k = 0; k < 9; ++k) { a[k] = (k % 4 == 0); b[k] = (float)k; }
        uint32_t pa = kArena + 0x100, pb = kArena + 0x140, pd = kArena + 0x180;
        memcpy((void*)FNV_G2H(pa), a, 36); memcpy((void*)FNV_G2H(pb), b, 36);
        memset((void*)FNV_G2H(pd), 0xcc, 36);
        Call(f_0043f8d0, pa, {pd, pb});
        CHECK(g_cpu.eax == pd);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
            {
                long double e = (long double)a[r * 3] * b[c] + (long double)a[r * 3 + 1] * b[3 + c] + (long double)a[r * 3 + 2] * b[6 + c];
                CHECK(Close(MF(pd + 4 * (r * 3 + c)), (double)e));
                if (n == 0 && r == c && c == 0) CHECK(MF(pd) == 0.0f);        // identity-ish первый ряд: 1*0
            }
        float want[9]; memcpy(want, (void*)FNV_G2H(pd), 36);
        Call(f_0043f8d0, pa, {pa, pb});                                          // dst == this
        CHECK(memcmp((void*)FNV_G2H(pa), want, 36) == 0);
        memcpy((void*)FNV_G2H(pa), a, 36);
        Call(f_0043f8d0, pa, {pb, pb});                                          // dst == other
        float want2[9];
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c)
            want2[r * 3 + c] = (float)((double)a[r * 3] * b[c] + (double)a[r * 3 + 1] * b[3 + c] + (double)a[r * 3 + 2] * b[6 + c]);
        CHECK(memcmp((void*)FNV_G2H(pb), want2, 36) == 0);
    }
    {   // единичная матрица слева возвращает правую точно
        float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, m[9] = {1.5f, -2, 3, 4, 5.25f, -6, 7, 8, 9.125f};
        memcpy((void*)FNV_G2H(kArena + 0x100), id, 36); memcpy((void*)FNV_G2H(kArena + 0x140), m, 36);
        Call(f_0043f8d0, kArena + 0x100, {kArena + 0x180, kArena + 0x140});
        CHECK(memcmp((void*)FNV_G2H(kArena + 0x180), m, 36) == 0);
    }

    // NiTransform::operator*: {R, T, s}; res.T = Ta + sa * (Ra * Tb), res.s = sa * sb
    srand(4);
    for (int n = 0; n < 500; ++n)
    {
        float a[13], b[13];
        for (float& x : a) x = (rand() / (float)RAND_MAX - 0.5f) * 4.f;
        for (float& x : b) x = (rand() / (float)RAND_MAX - 0.5f) * 4.f;
        uint32_t pa = kArena + 0x200, pb = kArena + 0x240, pd = kArena + 0x280;
        memcpy((void*)FNV_G2H(pa), a, 52); memcpy((void*)FNV_G2H(pb), b, 52);
        Call(f_0062c250, pa, {pd, pb});
        CHECK(g_cpu.eax == pd);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
            {
                long double e = (long double)a[r * 3] * b[c] + (long double)a[r * 3 + 1] * b[3 + c] + (long double)a[r * 3 + 2] * b[6 + c];
                CHECK(Close(MF(pd + 4 * (r * 3 + c)), (double)e));
            }
        for (int r = 0; r < 3; ++r)
        {
            long double rt = (long double)a[r * 3] * b[9] + (long double)a[r * 3 + 1] * b[10] + (long double)a[r * 3 + 2] * b[11];
            CHECK(Close(MF(pd + 0x24 + 4 * r), (double)(a[9 + r] + (long double)a[12] * rt)));
        }
        CHECK(Close(MF(pd + 0x30), (double)((long double)a[12] * b[12])));
        float want[13]; memcpy(want, (void*)FNV_G2H(pd), 52);
        Call(f_0062c250, pa, {pa, pb});                                          // dst == this
        CHECK(memcmp((void*)FNV_G2H(pa), want, 52) == 0);
    }
    {   // тождество слева
        float id[13] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}, m[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 2};
        memcpy((void*)FNV_G2H(kArena + 0x200), id, 52); memcpy((void*)FNV_G2H(kArena + 0x240), m, 52);
        Call(f_0062c250, kArena + 0x200, {kArena + 0x280, kArena + 0x240});
        CHECK(memcmp((void*)FNV_G2H(kArena + 0x280), m, 52) == 0);
    }

    // ShadowSceneLight: плоскости +0x144, маска +0x1a4. Куб [-1, 1]^3 как шесть плоскостей (нормали внутрь).
    uint32_t L = kArena + 0x1000;
    const float cube[6][4] = {{1, 0, 0, -1}, {-1, 0, 0, -1}, {0, 1, 0, -1}, {0, -1, 0, -1}, {0, 0, 1, -1}, {0, 0, -1, -1}};
    auto setup = [&](uint32_t mask) {
        for (int i = 0; i < 6; ++i) Put(L + 0x144 + 16 * i, {cube[i][0], cube[i][1], cube[i][2], cube[i][3]});
        M32(L + 0x1a4) = mask;
    };
    struct Sph { float c[3], r; bool inside; uint32_t mask_after; };
    const Sph sph[] = {
        {{0, 0, 0}, 0.5f, true, 0x3f},          // внутри, но ни одна плоскость не отсекает целиком: d = 1 > r
        {{0, 0, 0}, 0.5f, true, 0x3f},
        {{3, 0, 0}, 1.0f, false, 0x3f},         // за плоскостью 1 (x <= -1... нормаль (-1,0,0)): d = -3 - 1 = -4 <= -1
        {{0, 0, 0}, 2.0f, true, 0x3f},          // пересекает все: d = 1 < r
        {{0, 0, 0}, 1.0f, true, 0x3f},          // d == r у всех: r <= d -> плоскость выключается
    };
    setup(0x3f); Put(kArena + 0x2000, {0, 0, 0, 0.5f});
    Call(f_00b9bbb0, L, {kArena + 0x2000}); CHECK((g_cpu.eax & 0xff) == 1 && M32(L + 0x1a4) == 0);   // радиус 0.5 < d=... см. ниже
    (void)sph;
    // d для центра (0,0,0): normal·c - (-1) = 1 у каждой плоскости. r = 0.5: r <= d -> все шесть выключаются
    setup(0x3f); Put(kArena + 0x2000, {0, 0, 0, 2.0f});
    Call(f_00b9bbb0, L, {kArena + 0x2000}); CHECK((g_cpu.eax & 0xff) == 1 && M32(L + 0x1a4) == 0x3f);  // r = 2 > d = 1: пересекает, маска цела
    setup(0x3f); Put(kArena + 0x2000, {0, 0, 0, 1.0f});
    Call(f_00b9bbb0, L, {kArena + 0x2000}); CHECK((g_cpu.eax & 0xff) == 1 && M32(L + 0x1a4) == 0);    // r == d: выключаются все
    setup(0x3f); Put(kArena + 0x2000, {10, 0, 0, 1.0f});                                              // x = 10: d0 = 11, d1 = -9 <= -1
    Call(f_00b9bbb0, L, {kArena + 0x2000}); CHECK((g_cpu.eax & 0xff) == 0 && M32(L + 0x1a4) == 0x3e);   // плоскость 0 успела выключиться
    setup(0x3f); Put(kArena + 0x2000, {10, 0, 0, 1.0f});
    M32(L + 0x1a4) = 0x3c;                                                                            // плоскости 0 и 1 неактивны
    Call(f_00b9bbb0, L, {kArena + 0x2000}); CHECK((g_cpu.eax & 0xff) == 1 && M32(L + 0x1a4) == 0);  // остальные: d = 1 (y, z) >= r = 1 -> выключаются
    setup(0); Put(kArena + 0x2000, {100, 100, 100, 0.1f});
    Call(f_00b9bbb0, L, {kArena + 0x2000}); CHECK((g_cpu.eax & 0xff) == 1 && M32(L + 0x1a4) == 0);    // нет активных плоскостей: внутри
    setup(0x3f); Put(kArena + 0x2000, {0, 0, 0, NAN});
    Call(f_00b9bbb0, L, {kArena + 0x2000}); CHECK((g_cpu.eax & 0xff) == 1 && M32(L + 0x1a4) == 0x3f);  // NaN: оба сравнения ложны

    // ParseObjectAddFrustum: сцена-пустышка. Объект: +0 vtable, +0x20 bound*, +0x30 flags; узел: дети в +0x9c.
    uint32_t vt_node = kArena + 0x3000, vt_geom = kArena + 0x3040;     // vtable: [2] IsSceneNode, [6] IsTriBasedGeometry
    M32(vt_node + 8) = kFnNode;  M32(vt_node + 0x18) = kFnNotGeom;
    M32(vt_geom + 8) = kFnNotNode; M32(vt_geom + 0x18) = kFnGeom;
    uint32_t next = kArena + 0x4000;
    auto Obj = [&](bool node, uint32_t flags, float cx, float r, bool has_bound) {
        uint32_t o = next; next += 0x100;
        M32(o) = node ? vt_node : vt_geom;
        M32(o + 0x30) = flags;
        if (has_bound) { uint32_t b = next; next += 0x20; Put(b, {cx, 0, 0, r}); M32(o + 0x20) = b; } else M32(o + 0x20) = 0;
        return o;
    };
    auto Kids = [&](uint32_t node, std::initializer_list<uint32_t> k) {
        uint32_t arr = next; next += 0x40; uint32_t i = 0;
        for (uint32_t c : k) M32(arr + 4 * i++) = c;
        M32(node + 0x9c + 4) = arr; M16(node + 0x9c + 8) = (uint16_t)k.size(); M16(node + 0x9c + 0xa) = (uint16_t)k.size();
    };
    const uint32_t kSkip23 = 1u << 23, kFlag11 = 1u << 11;   // бит 23: объект исключён из теневого обхода
    {
        // корень (узел) с детьми: геометрия внутри, геометрия снаружи, геометрия без bound и без флага 11 (пропускается),
        // геометрия с флагом 11 (всегда берётся), геометрия с AppCulled (бит 0, пропускается), null
        uint32_t root = Obj(true, 0, 0, 5, true);
        uint32_t inside = Obj(false, 0, 0, 0.1f, true);
        uint32_t outside = Obj(false, 0, 50, 0.1f, true);
        uint32_t nobound = Obj(false, 0, 0, 0, false);
        uint32_t forced = Obj(false, kFlag11, 50, 0, false);
        uint32_t culled = Obj(false, 1u, 0, 1, true);
        Kids(root, {inside, 0, outside, nobound, forced, culled});
        setup(0x3f); M32(L + 0x128) = 0;
        g_added.clear();
        Call(f_00b9f5b0, L, {root});
        // плоскости куба: root (r = 5 > d) пересекает; inside (c = 0, r = 0.1): d = 1 >= r: плоскость выключается, потомки не тестируются
        CHECK(M32(L + 0x1a4) == 0x3f);                                  // маска на выходе прежняя
        CHECK(g_added.size() == 2 && g_added[0] == inside && g_added[1] == forced);
        // outside: дочерняя проверка идёт по маске, в которую вернулся родитель; AddGeometry вызывается только для геометрии,
        // которая не отсечена плоскостями и не помечена AppCulled; nobound (радиус 0, нет флага 11) в обход не попадает
        // корень снаружи: отбрасывается целиком
        uint32_t far_root = Obj(true, 0, 50, 1, true);
        Kids(far_root, {inside});
        g_added.clear(); setup(0x3f);
        Call(f_00b9f5b0, L, {far_root});
        CHECK(g_added.empty() && M32(L + 0x1a4) == 0x3f);
        // объект - сама сцена, либо с флагом 23: пропускается
        M32(L + 0x128) = root; g_added.clear();
        Call(f_00b9f5b0, L, {root}); CHECK(g_added.empty());
        M32(L + 0x128) = 0;
        uint32_t noflag = Obj(false, kSkip23, 0, 1, true);
        Call(f_00b9f5b0, L, {noflag}); CHECK(g_added.empty());
        // единичная геометрия: идёт в AddGeometry; без bound подставляется bound по умолчанию 0x011f4288
        MF(0x011f4288u) = 0; MF(0x011f428cu) = 0; MF(0x011f4290u) = 0; MF(0x011f4294u) = 0.5f;
        uint32_t lone = Obj(false, 0, 0, 0, false);
        setup(0x3f);
        Call(f_00b9f5b0, L, {lone}); CHECK(g_added.size() == 1 && g_added[0] == lone);
        MF(0x011f4294u) = 0.5f; MF(0x011f4288u) = 100.0f;                  // подставной bound вне пирамиды
        g_added.clear(); Call(f_00b9f5b0, L, {lone}); CHECK(g_added.empty());
        // нет активных плоскостей: bound не проверяется
        setup(0); g_added.clear(); Call(f_00b9f5b0, L, {lone}); CHECK(g_added.size() == 1 && M32(L + 0x1a4) == 0);
    }

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

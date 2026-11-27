// Ручные версии 8 горячих функций движка в раскладке гостя (NATIVE_FUNCS в tools/recomp.py, docs/RECOMP.md).
// Собирается только с -DFNV_NATIVE_MATH: этот флаг ставит сборка, когда recomp.py запущен с --native
// (out/recomp/native_math.on). Без него файл пуст, и f_XXXXXXXX берутся из перекомпилированного кода.
//
// Раскладки и формулы взяты из публичных источников (fnv_decomp: include/NiPlane.h, NiBound.h, NiTransform.h,
// NiFrustumPlanes.h, ShadowSceneLight.h; src/fallout_shared/globalfunc.cpp, tes.cpp, bsshader/shadowscenelight.cpp).
// Вызовы везде thiscall: this в ecx, аргументы в стеке, callee снимает их (`ret N`); float возвращается в ST(0).
// Вычисления сумм идут в double, как в x87 эмуляторе, а в память (и в локальные float оригинала) пишется float.
//
// Проверка против перекомпилированного оригинала: FNV_NATIVE_CHECK=1 (tools/native_check.py), в сборке без
// FNV_NATIVE_NO_ORACLE. Юнит-тест на Linux: tools/test_native_math.sh.
#ifdef FNV_NATIVE_MATH
#include <stdio.h>
#include <stdlib.h>
#include "fnv_recomp.h"
#include "fnv_recomp_host.h"

namespace
{

// ---- раскладки (смещения в госте) ------------------------------------------------------------------------------
const uint32_t kPlaneStride = 16;          // NiPlane: нормаль xyz, потом константа
const uint32_t kSslPlanes = 0x144;         // ShadowSceneLight::kShadowCameraPlanes (6 NiPlane), затем маска активных
const uint32_t kSslActive = kSslPlanes + 6 * kPlaneStride;     // 0x1a4: m_uiActivePlanes
const uint32_t kSslScene = 0x128;          // spShadowScene
const uint32_t kAvBound = 0x20;            // NiAVObject::m_kWorldBound (указатель на NiBound)
const uint32_t kAvFlags = 0x30;            // NiAVObject::m_uFlags
const uint32_t kNodeChildren = 0x9c;       // NiNode::m_kChildren: vtable, m_pBase +4, m_usMax +8, m_usSize +0xa
const uint32_t kVtIsSceneNode = 8;         // NiObject: vtable[2]
const uint32_t kVtIsTriBasedGeometry = 0x18;   // NiObject: vtable[6]
const uint32_t kDefaultBound = 0x011f4288u;    // NiBound, которым подменяют отсутствующий мировой bound
const uint32_t kAddGeometry = 0x00b9f480u;     // ShadowSceneLight::AddGeometry

inline float F(uint32_t a) { return MF(a); }
inline uint32_t Arg(FnvCpu& s, uint32_t i) { return M32(s.esp + 4 + 4 * i); }
inline void Return(FnvCpu& s, uint32_t nargs) { s.esp += 4 + 4 * nargs; }
inline void FloatResult(FnvCpu& s, double v) { s.st[s.depth++] = v; }

// ---- ядра: чистые функции над гостевой памятью -----------------------------------------------------------------
// x*px + y*py + z*pz слева направо, сумма в double (x87)
inline double Dot3(uint32_t a, uint32_t b)
{
    return (double)F(a) * (double)F(b) + (double)F(a + 4) * (double)F(b + 4) + (double)F(a + 8) * (double)F(b + 8);
}
inline double PlaneDistance(uint32_t plane, uint32_t point) { return Dot3(plane, point) - (double)F(plane + 12); }

// строка r левой матрицы на столбец c правой; матрицы по 9 float, построчно
inline float M3Rc(uint32_t a, uint32_t b, int r, int c)
{
    return (float)((double)F(a + 12 * r) * (double)F(b + 4 * c) + (double)F(a + 12 * r + 4) * (double)F(b + 12 + 4 * c) +
                   (double)F(a + 12 * r + 8) * (double)F(b + 24 + 4 * c));
}
void Mat3Mul(uint32_t dst, uint32_t a, uint32_t b)
{
    float t[9];                                  // результат пишется после чтения: dst может совпасть с a или b
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) t[r * 3 + c] = M3Rc(a, b, r, c);
    memcpy((void*)FNV_G2H(dst), t, sizeof t);
}

// NiTransform: матрица 0x00, сдвиг 0x24, масштаб 0x30. res = {rA*rB, tA + sA * (rA * tB), sA * sB}
void TransformMul(uint32_t dst, uint32_t a, uint32_t b)
{
    float rot[9], t[3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) rot[r * 3 + c] = M3Rc(a, b, r, c);
    float sa = F(a + 0x30), sb = F(b + 0x30);
    float rt[3], st[3];                          // rA * tB, затем sA * (...): между ними значения хранятся как float
    for (int r = 0; r < 3; ++r)
        rt[r] = (float)((double)F(a + 12 * r) * (double)F(b + 0x24) + (double)F(a + 12 * r + 4) * (double)F(b + 0x28) +
                        (double)F(a + 12 * r + 8) * (double)F(b + 0x2c));
    for (int r = 0; r < 3; ++r) st[r] = (float)((double)sa * (double)rt[r]);
    for (int r = 0; r < 3; ++r) t[r] = (float)((double)F(a + 0x24 + 4 * r) + (double)st[r]);
    float s2 = (float)((double)sa * (double)sb);
    memcpy((void*)FNV_G2H(dst), rot, sizeof rot);
    memcpy((void*)FNV_G2H(dst + 0x24), t, sizeof t);
    MF(dst + 0x30) = s2;
}

// 2: сфера целиком за плоскостью, 1: целиком перед ней, 0: пересекает
inline uint32_t WhichSide(uint32_t bound, uint32_t plane)
{
    float d = (float)PlaneDistance(plane, bound);
    float r = F(bound + 12);
    if (d <= -r) return 2;
    if (d >= r) return 1;
    return 0;
}

// плоскость (x, y, z, w) в начале this; знак x*px + y*py + z*pz - w: меньше нуля 2, больше 1, иначе 0 (NaN: 0)
inline uint32_t ClassifyPoint(uint32_t plane, uint32_t point)
{
    float r = (float)(Dot3(plane, point) - (double)F(plane + 12));
    return r < 0.0 ? 2 : r > 0.0 ? 1 : 0;
}

// Проверка сферы по шести плоскостям тени: за любой активной плоскостью - вне; целиком перед плоскостью -
// плоскость больше не нужна для потомков (бит сбрасывается)
bool TestSphereFrustum(uint32_t ssl, uint32_t sphere)
{
    float radius = F(sphere + 12);
    for (uint32_t i = 0; i < 6; ++i)
    {
        if (!((M32(ssl + kSslActive) >> i) & 1u)) continue;
        float d = (float)PlaneDistance(ssl + kSslPlanes + kPlaneStride * i, sphere);
        if (d <= -radius) return false;
        if (radius <= d) M32(ssl + kSslActive) &= ~(1u << i);
    }
    return true;
}

// виртуальный вызов метода без аргументов у объекта obj (vtable[slot]); возвращает eax
uint32_t VCall0(FnvCpu& s, uint32_t obj, uint32_t vt_off)
{
    s.ecx = obj;
    return fnv_guest_call(s, M32(M32(obj) + vt_off), nullptr, 0);
}

// ShadowSceneLight::ParseObjectAddFrustum: обход сцены; объект вне пирамиды отбрасывается вместе с потомками,
// узлы идут в обход детьми, геометрия - в AddGeometry (0x00b9f480). Маска активных плоскостей на выходе прежняя.
void ParseObjectAddFrustum(FnvCpu& s, uint32_t ssl, uint32_t obj)
{
    uint32_t active = M32(ssl + kSslActive);
    uint32_t flags = M32(obj + kAvFlags);
    if (obj == M32(ssl + kSslScene) || !(~(flags >> 23) & 1u)) goto done;
    if (active && !((flags >> 11) & 1u))
    {
        uint32_t b = M32(obj + kAvBound);
        if (!TestSphereFrustum(ssl, b ? b : kDefaultBound)) goto done;
    }
    {
        uint32_t node = VCall0(s, obj, kVtIsSceneNode);
        if (node)
        {
            for (uint32_t i = 0; i < M16(node + kNodeChildren + 0xa); ++i)
            {
                uint32_t child = M32(M32(node + kNodeChildren + 4) + 4 * i);
                if (!child) continue;
                uint32_t cf = M32(child + kAvFlags);
                uint32_t cb = M32(child + kAvBound);
                if ((((cf >> 11) & 1u) || (cb && F(cb + 12) != 0.0f)) && !(cf & 1u)) ParseObjectAddFrustum(s, ssl, child);
            }
        }
        else if (uint32_t geom = VCall0(s, obj, kVtIsTriBasedGeometry))
        {
            s.ecx = ssl;
            uint32_t arg = geom;
            fnv_guest_call(s, kAddGeometry, &arg, 1);
        }
    }
done:
    M32(ssl + kSslActive) = active;
}

// ---- точки входа: f_XXXXXXXX в соглашении гостя -----------------------------------------------------------------
void nm_0043f8d0(FnvCpu& s)     // NiMatrix3::operator*  (this = ecx, [result, kB])  -> eax = result
{
    Mat3Mul(Arg(s, 0), s.ecx, Arg(s, 1));
    s.eax = Arg(s, 0);
    Return(s, 2);
}
void nm_0062c250(FnvCpu& s)     // NiTransform::operator*  (this = ecx, [result, other])  -> eax = result
{
    TransformMul(Arg(s, 0), s.ecx, Arg(s, 1));
    s.eax = Arg(s, 0);
    Return(s, 2);
}
void nm_004b6190(FnvCpu& s)     // NiPoint3::Dot(const NiPoint3*)  -> ST(0)
{
    FloatResult(s, Dot3(s.ecx, Arg(s, 0)));
    Return(s, 1);
}
void nm_004b6160(FnvCpu& s)     // NiPlane::Distance(const NiPoint3&)  -> ST(0)
{
    FloatResult(s, PlaneDistance(s.ecx, Arg(s, 0)));
    Return(s, 1);
}
void nm_004b6100(FnvCpu& s)     // NiBound::WhichSide(const NiPlane*)  -> eax
{
    s.eax = WhichSide(s.ecx, Arg(s, 0));
    Return(s, 1);
}
void nm_0049da80(FnvCpu& s)     // NiCamera::ClassifyPoint(const NiPoint3*)  -> eax
{
    s.eax = ClassifyPoint(s.ecx, Arg(s, 0));
    Return(s, 1);
}
void nm_00b9bbb0(FnvCpu& s)     // ShadowSceneLight::TestSphereFrustum(const NiBound*)  -> al
{
    s.eax = TestSphereFrustum(s.ecx, Arg(s, 0)) ? 1u : 0u;
    Return(s, 1);
}
void nm_00b9f5b0(FnvCpu& s)     // ShadowSceneLight::ParseObjectAddFrustum(NiAVObject*)
{
    ParseObjectAddFrustum(s, s.ecx, Arg(s, 0));
    Return(s, 1);
}

#ifndef FNV_NATIVE_NO_ORACLE
// ---- FNV_NATIVE_CHECK=1: каждый вызов идёт и в оригинал (fo_), и в ручную версию; сравниваются eax, esp, ST(0)
// и память результата (аргумент out: номер стекового аргумента, -1: нет). ParseObjectAddFrustum (pure = false)
// зовёт AddGeometry, повторный вызов задвоил бы побочный эффект: под проверкой она идёт оригиналом и не сверяется.
struct Spec
{
    uint32_t addr;
    void (*orig)(FnvCpu&);
    void (*nat)(FnvCpu&);
    int out;           // стековый аргумент с указателем на результат; -2: область this+ctx_off
    uint32_t out_len;
    uint32_t ctx_off;
    bool st0;          // результат в ST(0)
    bool pure;         // без побочных эффектов: можно звать обе версии
};
uint64_t g_calls[8], g_bad[8];

void Checked(FnvCpu& s, const Spec& sp, int idx)
{
    static const bool on = getenv("FNV_NATIVE_CHECK") && getenv("FNV_NATIVE_CHECK")[0] == '1';
    if (!on) return sp.nat(s);
    if (!sp.pure) return sp.orig(s);
    FnvCpu a = s, b = s;
    uint8_t ref[64], saved[64];
    uint32_t out = sp.out >= 0 ? Arg(s, (uint32_t)sp.out) : sp.out == -2 ? s.ecx + sp.ctx_off : 0;
    if (out) memcpy(saved, (void*)FNV_G2H(out), sp.out_len);   // вход оригинала = вход ручной версии (dst может быть и входом)
    sp.orig(a);
    if (out) { memcpy(ref, (void*)FNV_G2H(out), sp.out_len); memcpy((void*)FNV_G2H(out), saved, sp.out_len); }
    sp.nat(b);
    bool bad = a.eax != b.eax || a.esp != b.esp || a.depth != b.depth ||
               (out && memcmp(ref, (void*)FNV_G2H(out), sp.out_len) != 0) ||
               (sp.st0 && memcmp(&a.st[a.depth - 1], &b.st[b.depth - 1], sizeof(double)) != 0);
    ++g_calls[idx];
    if (bad && ++g_bad[idx] <= 16)
    {
        char m[200];
        snprintf(m, sizeof m, "[native-check] %08x differs (call %llu): eax %08x/%08x esp %08x/%08x%c", sp.addr,
                 (unsigned long long)g_calls[idx], a.eax, b.eax, a.esp, b.esp, 10);
        fnv_host_log(m);
    }
    if ((g_calls[idx] & 0xfffff) == 0)
    {
        char m[200];
        snprintf(m, sizeof m, "[native-check] %08x: %llu calls, %llu differences%c", sp.addr,
                 (unsigned long long)g_calls[idx], (unsigned long long)g_bad[idx], 10);
        fnv_host_log(m);
    }
    s = a;      // продолжает игра с результатом оригинала: проверка ничего не меняет
}
#endif

}   // namespace

#ifdef FNV_NATIVE_NO_ORACLE
#define NM_ENTRY(ADDR, IDX, OUT, LEN, CTX, ST0, PURE) void f_##ADDR(FnvCpu& s) { nm_##ADDR(s); }
#else
#define NM_ENTRY(ADDR, IDX, OUT, LEN, CTX, ST0, PURE) \
    void fo_##ADDR(FnvCpu&); \
    void f_##ADDR(FnvCpu& s) { static const Spec sp = {0x##ADDR, fo_##ADDR, nm_##ADDR, OUT, LEN, CTX, ST0, PURE}; Checked(s, sp, IDX); }
#endif

NM_ENTRY(0043f8d0, 0, 0, 36, 0, false, true)
NM_ENTRY(0062c250, 1, 0, 52, 0, false, true)
NM_ENTRY(004b6190, 2, -1, 0, 0, true, true)
NM_ENTRY(004b6160, 3, -1, 0, 0, true, true)
NM_ENTRY(004b6100, 4, -1, 0, 0, false, true)
NM_ENTRY(0049da80, 5, -1, 0, 0, false, true)
NM_ENTRY(00b9bbb0, 6, -2, 4, 0x1a4, false, true)
NM_ENTRY(00b9f5b0, 7, -1, 0, 0, false, false)

#endif   // FNV_NATIVE_MATH

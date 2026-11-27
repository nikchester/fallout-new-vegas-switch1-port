#!/usr/bin/env python3
"""Whole-game recompiler: every function of FalloutNV.exe as portable C++ (docs/RECOMP.md).

Reuses the Phase 1 lifter's instruction translation (tools/lift_stubs.py) and changes the call boundary. All
functions share one FnvCpu per thread and a real guest stack: `call` pushes the original return address and calls
the target's host function, `ret N` pops it plus N bytes and returns, indirect calls/jumps dispatch on the original
address. Code addresses stay original values everywhere (immediates, jump tables, the data image), and `fs:` is
memory at the thread's TEB block.

Entries: every recipe function start (build/units.json), every relocation target in .text whose fixup lies outside
.text (vtables, EH tables, callback tables), code addresses used as immediates, the exe entry point, and every
direct call/jump target found while lifting.

Output (derived from the player's exe: git-ignored, never committed): out/recomp/recomp_NN.cpp, out/recomp/report.json.

usage: python tools/recomp.py --exe <FalloutNV.exe> [--shards 64] [--only 0xADDR,...]
"""
import argparse
import bisect
import json
import re
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lift_stubs as ls  # noqa: E402
from decomp_path import DECOMP_DIR  # noqa: E402
from relink import Image  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'out/recomp'
# CRT helpers whose register/x87 conventions the Phase 1 lowering handles inline (by the decomp's name)
INLINE_HELPERS = re.compile(r'^(ftol2_sse|ftol2|ftol|allshl|aullshr|allshr|ci(acos|asin|atan|cos|sin|tan|sqrt|log|'
                            r'log10|exp|cosh|sinh|tanh|pow|fmod|atan2))$|chkstk|alloca_probe')


# The exe's CRT math functions (addresses from port/compat/crt_map.json): called inline as C math, because their
# bodies select SSE2 implementations full of instructions nothing else in the game uses
CRT_MATH = {0x00ec6054: ('sqrt', 1), 0x00ec6cde: ('fabs', 1), 0x00ec9010: ('atan', 1), 0x00ec9130: ('fmod', 2),
            0x00ec9610: ('asin', 1), 0x00ec9ac0: ('pow', 2), 0x00ec9e10: ('ceil', 1), 0x00ec9f30: ('cos', 1),
            0x00eca060: ('sin', 1), 0x00eca190: ('acos', 1), 0x00eca590: ('tan', 1), 0x00eca920: ('log', 1),
            0x00ecae30: ('exp', 1), 0x00ecbc80: ('log10', 1)}


# Cdecl routines with host implementations in port/recomp/fnv_intrinsics.cpp. The CRT addresses come from
# port/compat/crt_map.json. The lifted wrapper calls the host routine and pops the guest return address.
INTRINSICS = {0x00ec44d0: 'memmove', 0x00ec7230: 'memmove', 0x00ec61c0: 'memset', 0x00ec4835: 'memcmp',
              0x00ec6130: 'strlen', 0x00ec6370: 'strcpy', 0x00ec6380: 'strcat', 0x00ec6da0: 'strcmp',
              0x00ec7690: 'strchr', 0x00ec6e30: 'strrchr', 0x00ec7750: 'strstr', 0x00ec8a19: 'strncmp',
              0x00ecc7b0: 'strncpy', 0x00ecbf20: 'strpbrk', 0x00ecdb00: 'wcslen',
              0x00af6e50: 'archive_sort'}


# Imports simple enough to emit inline at `call dword ptr [IAT slot]` instead of dispatching to the platform layer
# (stdcall: arguments at [esp], popped here; eax is the result; ecx/edx keep their values, which the callee was free
# to clobber anyway). The same operations as pal_kernel.cpp's k_* versions, which still serve calls through a register.
INLINE_IMPORTS = {
    'InterlockedIncrement': 's.eax = __atomic_add_fetch((uint32_t*)FNV_G(M32(s.esp)), 1u, __ATOMIC_SEQ_CST); s.esp += 4;',
    'InterlockedDecrement': 's.eax = __atomic_sub_fetch((uint32_t*)FNV_G(M32(s.esp)), 1u, __ATOMIC_SEQ_CST); s.esp += 4;',
    'InterlockedExchange': 's.eax = __atomic_exchange_n((uint32_t*)FNV_G(M32(s.esp)), M32(s.esp + 4), __ATOMIC_SEQ_CST); s.esp += 8;',
    'InterlockedExchangeAdd': 's.eax = __atomic_fetch_add((uint32_t*)FNV_G(M32(s.esp)), M32(s.esp + 4), __ATOMIC_SEQ_CST); s.esp += 8;',
    'InterlockedCompareExchange': ('uint32_t e_ = M32(s.esp + 8); __atomic_compare_exchange_n((uint32_t*)FNV_G(M32(s.esp)), &e_, '
                                   'M32(s.esp + 4), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); s.eax = e_; s.esp += 12;'),
    'TlsGetValue': 's.eax = fnv_tls_get(fnv_c(s), M32(s.esp)); s.esp += 4;',
}
IAT_CALL = re.compile(r'RCALLI\(M32\(\(uint32_t\)\(0u \+ 0x([0-9a-f]+)u\)\), 0x[0-9a-f]{8}u\);')


def inline_imports(src, iat):
    """INLINE_IMPORTS at their direct IAT call sites; iat = {slot address: import name}"""
    def sub(m):
        name = iat.get(int(m.group(1), 16))
        return '{ %s }' % INLINE_IMPORTS[name] if name in INLINE_IMPORTS else m.group(0)
    return IAT_CALL.sub(sub, src)


class Recompiler(ls.Lifter):
    def __init__(self, exe, text, starts, sizes, names):
        self.starts = starts                  # sorted function starts (recipe)
        self.sizes = sizes                    # start -> size
        self.names = names                    # address -> decomp name (helpers only need it)
        super().__init__(exe, self._resolve, text, gap_end=self._end, allow=lambda a: text[0] <= a < text[1])

    # ---- context ------------------------------------------------------------------------------------------------
    def _resolve(self, addr):
        n = self.names.get(addr)
        if n and INLINE_HELPERS.search(n.lstrip('_?@').split('@')[0].lower()):
            return ('sym', n)
        return None

    def _end(self, a):
        """End of the code an entry at `a` may reach without a call: its recipe function, else the next start."""
        k = bisect.bisect_right(self.starts, a) - 1
        if k >= 0 and self.starts[k] <= a < self.starts[k] + self.sizes[self.starts[k]]:
            return self.starts[k] + self.sizes[self.starts[k]]
        nxt = self.starts[k + 1] if k + 1 < len(self.starts) else self.text[1]
        return min(nxt, a + 0x10000)

    def lift(self, entry):
        if entry in INTRINSICS:
            self.lifted[entry] = ('void fnv_i_%s(FnvCpu& s);\nvoid f_%08x(FnvCpu& s) { fnv_i_%s(s); s.esp += 4; }  '
                                  '// %s (host)' % (INTRINSICS[entry], entry, INTRINSICS[entry], INTRINSICS[entry]))
            return
        # the base class bounds initializer-region entries by +0x2000; here every entry uses its function's end
        end = self._end(entry)
        hit = self.cache.get((entry, end))
        if hit and self.replay(entry, hit):
            self.cache_new[(entry, end)] = hit
            self.cache_hits += 1
            return
        self._log = []
        before = set(self.unsupported)
        self.lift_uncached(entry, end)
        unsupported = {a: v for a, v in self.unsupported.items() if a not in before}
        self.cache_new[(entry, end)] = (self.lifted[entry], self.ret_pop.get(entry), unsupported, self._log)
        self._log = None

    # ---- code addresses stay original ---------------------------------------------------------------------------
    def code_ptr_kind(self, addr):
        return '0x%08xu' % addr, None

    def code_ptr(self, addr):
        if self._log is not None:
            self._log.append(('c', addr, '0x%08xu' % addr))
        self.want(addr)
        return '0x%08xu' % addr

    def want(self, addr):
        if self.text[0] <= addr < self.text[1] and addr not in self.lifted and addr not in self.queued:
            self.queue.append(addr)
            self.queued.add(addr)

    # ---- operands -----------------------------------------------------------------------------------------------
    def parse(self, op):
        m = re.match(r'^(?:(byte|word|dword|qword|xmmword|tbyte) ptr )?fs:\[(.*)\]$', op.strip())
        if m:                                   # the thread's TEB block in guest memory
            return ('mem', ls.SIZES[m.group(1) or 'dword'], '(uint32_t)(s.fs_base + %s)' % self.mem_addr(m.group(2)))
        return super().parse(op)

    def read(self, o, size=None):
        k, sz, e = o
        size = size or sz
        if k == 'imm':
            if size == 32 and self.is_code(e & 0xffffffff):
                self.code_ptr(e & 0xffffffff)   # a code address handed to data: lift it as an entry
            return '%#xu' % (e & {8: 0xff, 16: 0xffff}.get(size, 0xffffffff))
        if k == 'mem':                          # .text is mapped at its original address: byte tables are memory
            if e == '(uint32_t)(0u + 0x1270a64u)':
                return '0u'                     # the CRT's "use SSE2 math" flag: always take the x87 paths
            return {8: 'M8(%s)', 16: 'M16(%s)', 32: 'M32(%s)'}[size] % e
        return super().read(o, size)

    # ---- the call boundary ----------------------------------------------------------------------------------------
    def extra(self, mnem, ops):
        """Instructions the Phase 1 lifter never needed (they live in CRT code it did not lift). None: not handled."""
        args = [a for a in re.split(r',(?![^\[]*\])', ops) if a.strip()] if ops else []
        if mnem == 'rep ret':                              # AMD's two-byte return
            return 'RETURN(0);'
        if mnem == 'std':
            return 's.df = 1;'
        if mnem == 'cld':
            return 's.df = 0;'
        if mnem in ('clc', 'stc', 'cmc'):
            return {'clc': 's.cf = 0;', 'stc': 's.cf = 1;', 'cmc': 's.cf = !s.cf;'}[mnem]
        m = re.match(r'^(rep )?(movs|stos)([bwd])$', mnem)
        if m and 'xmm' not in ops:                         # string moves and stores, honoring the direction flag
            w = {'b': 1, 'w': 2, 'd': 4}[m.group(3)]
            mm = {1: 'M8', 2: 'M16', 4: 'M32'}[w]
            step = '(s.df ? -%du : %du)' % (w, w)
            if m.group(2) == 'movs':
                one = '%s(s.edi) = %s(s.esi); s.edi += %s; s.esi += %s;' % (mm, mm, step, step)
            else:
                one = '%s(s.edi) = (%s)s.eax; s.edi += %s;' % (mm, {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}[w], step)
            return ('while (s.ecx) { %s s.ecx--; }' % one) if m.group(1) else one
        m = re.match(r'^(repne|repe) (scas|cmps)b$', mnem)
        if m:
            cond = 's.zf' if m.group(1) == 'repne' else '!s.zf'
            if m.group(2) == 'scas':
                one = 'uint32_t a_ = s.eax & 0xffu, b_ = M8(s.edi), r_ = a_ - b_; SUBF(a_, b_, r_, 8); s.edi += (s.df ? -1u : 1u);'
            else:
                one = ('uint32_t a_ = M8(s.esi), b_ = M8(s.edi), r_ = a_ - b_; SUBF(a_, b_, r_, 8); '
                       's.esi += (s.df ? -1u : 1u); s.edi += (s.df ? -1u : 1u);')
            return 'while (s.ecx) { %s s.ecx--; if (%s) break; }' % (one, cond)
        if mnem == 'xlatb':
            return 's.eax = (s.eax & ~0xffu) | M8(s.ebx + (s.eax & 0xffu));'
        if mnem == 'pushal':
            return '{ uint32_t e_ = s.esp; PUSH(s.eax); PUSH(s.ecx); PUSH(s.edx); PUSH(s.ebx); PUSH(e_); PUSH(s.ebp); PUSH(s.esi); PUSH(s.edi); }'
        if mnem in ('rcr', 'rcl') and 1 <= len(args) <= 2:
            P = [self.parse(a) for a in args]
            bits = P[0][1]
            cnt = self.read(P[1]) if len(P) > 1 else '1u'
            if mnem == 'rcr':
                step = 'nc_ = v_ & 1u; v_ = (v_ >> 1) | ((uint32_t)s.cf << %d); s.cf = (int)nc_;' % (bits - 1)
            else:
                step = 'nc_ = (v_ >> %d) & 1u; v_ = ((v_ << 1) | (uint32_t)s.cf) & %#xu; s.cf = (int)nc_;' % (
                    bits - 1, {8: 0xff, 16: 0xffff, 32: 0xffffffff}[bits])
            return '{ uint32_t v_ = %s, nc_, c_ = (%s) & 31u; while (c_--) { %s } %s }' % (
                self.read(P[0]), cnt, step, self.write(P[0], 'v_'))
        if mnem in ('movlpd', 'movlps', 'movhpd', 'movhps') and len(args) == 2:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            off = 0 if mnem.startswith('movl') else 8
            if d[0] == 'xmm':
                return 'memcpy(X(%s) + %d, (const uint8_t*)FNV_G(%s), 8);' % (d[2], off, s_[2])
            return 'memcpy((uint8_t*)FNV_G(%s), X(%s) + %d, 8);' % (d[2], s_[2], off)
        if mnem in ('andpd', 'andnpd', 'orpd', 'xorpd') and len(args) == 2:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)FNV_G(%s)' % s_[2]
            return 'SSEB(X(%s), %s, %d);' % (d[2], src, ('andpd', 'andnpd', 'orpd', 'xorpd').index(mnem))
        if mnem == 'unpcklpd' and len(args) == 2:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)FNV_G(%s)' % s_[2]
            return '{ uint8_t t_[8]; memcpy(t_, %s, 8); memcpy(X(%s) + 8, t_, 8); }' % (src, d[2])
        if mnem == 'cvtdq2ps' and len(args) == 2:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)FNV_G(%s)' % s_[2]
            return ('{ int32_t i_[4]; float f_[4]; memcpy(i_, %s, 16); for (int k_ = 0; k_ < 4; ++k_) f_[k_] = (float)i_[k_]; '
                    'memcpy(X(%s), f_, 16); }') % (src, d[2])
        if mnem == 'movd' and len(args) == 2 and re.match(r'^xmm\d$', args[0].strip()):
            s_ = self.parse(args[1])
            return '{ uint32_t v_ = %s; memset(X(%s), 0, 16); memcpy(X(%s), &v_, 4); }' % (
                self.read(s_), args[0].strip()[3:], args[0].strip()[3:])
        if mnem in ('movntps', 'movntdq') and len(args) == 2:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            return 'memcpy((uint8_t*)FNV_G(%s), X(%s), 16);' % (d[2], s_[2])
        if mnem == 'popal':
            return ('s.edi = POP(); s.esi = POP(); s.ebp = POP(); (void)POP(); s.ebx = POP(); s.edx = POP(); '
                    's.ecx = POP(); s.eax = POP();')
        if mnem in ('fldenv', 'frstor') and len(args) == 1:
            return ''                              # the environment the CRT saved a moment before: nothing to restore
        if mnem in ('psrlq', 'psllq') and len(args) == 2 and re.match(r'^xmm\d$', args[1].strip()):
            d, c = args[0].strip()[3:], args[1].strip()[3:]
            op = '>>' if mnem == 'psrlq' else '<<'
            return ('{ uint64_t q_[2], n_; memcpy(q_, X(%s), 16); memcpy(&n_, X(%s), 8); '
                    'q_[0] = n_ < 64 ? q_[0] %s n_ : 0; q_[1] = n_ < 64 ? q_[1] %s n_ : 0; memcpy(X(%s), q_, 16); }') % (
                d, c, op, op, d)
        if mnem == 'pshufd' and len(args) == 3:
            d, s_ = self.parse(args[0]), self.parse(args[1])
            imm = ls.num(args[2].strip()) & 0xff
            src = 'X(%s)' % s_[2] if s_[0] == 'xmm' else '(const uint8_t*)FNV_G(%s)' % s_[2]
            sel = ', '.join('a_[%d]' % ((imm >> (2 * i)) & 3) for i in range(4))
            return '{ uint32_t a_[4]; memcpy(a_, %s, 16); uint32_t r_[4] = {%s}; memcpy(X(%s), r_, 16); }' % (src, sel, d[2])
        if mnem in ('fnstenv', 'fstenv', 'fnsave', 'fsave') and len(args) == 1:
            d = self.parse(args[0])
            size = 28 if 'env' in mnem else 108
            st = ('{ uint8_t* e_ = (uint8_t*)FNV_G(%s); memset(e_, 0, %d); uint16_t cw_ = 0x027f, sw_ = (uint16_t)FSW(), '
                  'tw_ = s.depth ? 0 : 0xffff; memcpy(e_, &cw_, 2); memcpy(e_ + 4, &sw_, 2); memcpy(e_ + 8, &tw_, 2); }') % (d[2], size)
            return st + (' s.depth = 0;' if 'save' in mnem else '')
        return None

    def stmt(self, addr, n, raw, mnem, ops, labels, entry, end):
        x = self.extra(mnem, ops)
        if x is not None:
            return x
        if mnem == 'call':
            o, nxt = ops.strip(), addr + n
            if o.startswith('dword ptr') or o.startswith('fs:') or o in ls.REG32:
                tgt = ('s.' + o) if o in ls.REG32 else self.read(self.parse(o))
                return 'RCALLI(%s, 0x%08xu);' % (tgt, nxt)
            target = ls.num(o.split()[0])
            m = CRT_MATH.get(target)
            if m:                               # cdecl double math: arguments at [esp], result in st0
                fn, nargs = m
                xs = ', '.join('MD(s.esp + %du)' % (8 * i) for i in range(nargs))
                return 'FPUSH(%s(%s));' % (fn, xs)
            if self.resolve_code(target):       # x87/edx:eax helpers lowered inline by the base class
                return super().stmt(addr, n, raw, mnem, ops, labels, entry, end)
            if not self.is_code(target):
                return 'fnv_trap(0x%08xu, "recomp: call outside .text"); RETURN(0);' % addr
            self.code_ptr(target)
            return 'RCALL(f_%08x, 0x%08xu);' % (target, nxt)
        st = super().stmt(addr, n, raw, mnem, ops, labels, entry, end)
        # tail jumps to a known address become direct calls (TAILCALL keeps dispatching for computed targets)
        return re.sub(r'TAILCALL\(0x([0-9a-f]{8})u\)', lambda m: 'RJMP(f_%s)' % m.group(1), st)

    def lift_uncached(self, entry, end):
        """The base walk, stopping after any unconditional jump as well as ret (the code after one is reachable only
        through a label, which is queued): without it a walk runs on into the next function."""
        self.disassemble(entry, end)
        body, labels, todo, seen = {}, set(), [entry], set()
        queued, queued_n = set(), 0
        while todo:
            a = todo.pop()
            if a not in self.ins and entry <= a < end:
                self.resync(a, end)
            while a in self.ins and a not in seen and a < end:
                seen.add(a)
                n, raw, mnem, ops = self.ins[a]
                try:
                    st = self.stmt(a, n, raw, mnem, ops, labels, entry, end)
                except ls.Unsupported as e:
                    self.unsupported[a] = str(e)
                    st = 'fnv_trap(0x%08xu, "recomp: unsupported instruction"); return;' % a
                body[a] = st
                if len(labels) != queued_n:
                    for t in labels - queued:
                        queued.add(t)
                        if t not in seen:
                            todo.append(t)
                    queued_n = len(labels)
                if mnem in ('ret', 'jmp') or 'RETURN(' in st or 'return;' in st:
                    break
                a += n
                if a not in self.ins and a < end:
                    self.resync(a, end)
        lines = ['void f_%08x(FnvCpu& s)' % entry, '{', '    FNV_ENTER(0x%08xu);' % entry]
        for a in sorted(body):
            if a in labels:
                lines.append('L_%08x:' % a)
            lines.append('    %s  // %08x %s %s' % (body[a], a, self.ins[a][2], self.ins[a][3][:60]))
        lines.append('    fnv_trap(0x%08xu, "recomp: fell off the end of the lifted range");' % entry)
        lines.append('}')
        self.lifted[entry] = '\n'.join(lines)

    # ---- cache and output -----------------------------------------------------------------------------------------
    def cache_path(self):
        import hashlib
        h = hashlib.sha1(Path(ls.__file__).read_bytes() + Path(__file__).read_bytes())
        with open(self.exe, 'rb') as f:
            h.update(hashlib.sha256(f.read()).digest())
        return ROOT / 'out/cache' / ('recomp_%s.pickle' % h.hexdigest()[:16])

    def run(self, entries, use_cache=True):
        import pickle
        path = self.cache_path() if use_cache else None
        if path and path.exists():
            self.cache = pickle.loads(path.read_bytes())
        self.stub_starts = set()                # no stub boundaries: every function is lifted
        self.queue = sorted(entries, reverse=True)
        self.queued = set(entries)
        done = 0
        t0 = time.time()
        while self.queue:
            e = self.queue.pop()
            if e not in self.lifted:
                self.lift(e)
                done += 1
                if done % 5000 == 0:
                    print('  %d lifted, %d queued, %.0f s' % (done, len(self.queue), time.time() - t0), flush=True)
        if path and self.cache_new != self.cache:
            path.parent.mkdir(parents=True, exist_ok=True)
            for old in path.parent.glob('recomp_*.pickle'):
                old.unlink()
            path.write_bytes(pickle.dumps(self.cache_new, protocol=pickle.HIGHEST_PROTOCOL))

    def shards(self, n, x87_regs=True, iat=None, leaf_max=None, stack_slots=True, callee_saved=True, small_max=0, native=()):
        groups = [[] for _ in range(n)]
        for a in sorted(self.lifted):
            groups[zlib.crc32(struct.pack('<I', a)) % n].append(a)
        ref = re.compile(r'\bf_([0-9a-f]{8})\b')
        iat = iat or {}
        texts = {a: local_regs(inline_imports(self.lifted[a], iat)) for a in self.lifted if a not in HOST_FUNCS}
        if x87_regs:                                 # the x87 stack in locals where its depth is fixed
            texts = x87_regs_pass(texts)
        if stack_slots:                              # frame slots in locals where nothing else can reach the frame
            texts = stack_slots_pass(texts)
        if callee_saved:                             # ebx/esi/edi/ebp kept in host registers across calls
            texts = callee_saved_pass(texts)
        for a in set(native) & set(texts):
            texts[a] = texts[a].replace('void f_%08x(FnvCpu& s_)' % a, 'void fo_%08x(FnvCpu& s_)' % a, 1)
        leaf_max = LEAF_MAX if leaf_max is None else leaf_max
        leaves = small_leaves(texts, leaf_max) | small_functions(texts, small_max)
        print('small functions copied into calling shards: %d (leaves up to %d instructions, others up to %d)' % (
            len(leaves), leaf_max, small_max), flush=True)
        out = []
        for k, funcs in enumerate(groups):
            body = '\n'.join(texts[a] for a in funcs if a not in HOST_FUNCS)
            mine = {'%08x' % a for a in funcs}
            # a private copy of each small leaf from another shard that this one calls directly (LEAF_MAX)
            copies = sorted(set(LEAF_CALL.findall(body)) & leaves - mine)
            if copies:
                cs = set(copies)
                body = LEAF_CALL_SUB.sub(lambda m: m.group(1) + ('fl_' if m.group(2) in cs else 'f_') + m.group(2) + m.group(3), body)
            copied = ''.join(texts[int(a, 16)] for a in copies)     # what the copies call needs declaring too
            used = sorted((set(ref.findall(body)) | set(ref.findall(copied))) - mine)
            head = ['// Generated by tools/recomp.py from the player\'s FalloutNV.exe: do not commit (rule 1).',
                    '#include "fnv_recomp.h"', '']
            head += ['void f_%s(FnvCpu& s);' % a for a in used]
            head += ['void f_%08x(FnvCpu& s);' % a for a in funcs]
            for a in copies:
                head.append(texts[int(a, 16)].replace('void f_%s(FnvCpu& s_)' % a, 'static inline void fl_%s(FnvCpu& s_)' % a, 1))
            tail = ['', 'static const uint32_t kAddrs[] = {%s};' % ', '.join('0x%08xu' % a for a in funcs),
                    'static const FnvFn kFns[] = {%s};' % ', '.join('f_%08x' % a for a in funcs),
                    '// in an anonymous namespace: one Reg type per shard. A global `struct Reg` would be the same type in all 64,',
                    '// with an inline constructor each, and an -Oz build keeps one copy: every shard registered shard 0.',
                    'namespace { struct Reg { Reg() { fnv_register_functions(kAddrs, kFns, %d); } } reg_; }' % len(funcs)]
            out.append('\n'.join(head) + '\n' + body + '\n' + '\n'.join(tail) + '\n')
        return out


# Leaf functions (no calls of any kind) of at most this many instructions get a private inline copy in every shard that
# calls them directly, so the compiler can inline them: the game is built with little inlining, and a quarter of its
# direct calls go to getters, empty constructors and the like. The original stays for indirect calls and other shards.
LEAF_MAX = 20
LEAF_CALLS = re.compile(r'\b(RCALL|RCALLI|RCALLK|RCALLIK|RJMP|TAILCALL)\(')
LEAF_CALL = re.compile(r'\b(?:RCALLK?\(|RJMP\()f_([0-9a-f]{8})\b')
LEAF_CALL_SUB = re.compile(r'\b(RCALLK?\(|RJMP\()f_([0-9a-f]{8})([,)])')


def small_leaves(texts, limit):
    """{'%08x'} of lifted functions with no call, jump or dispatch out and at most `limit` instructions"""
    out = set()
    if limit <= 0:
        return out
    for a, t in texts.items():
        if t.startswith('void f_') and t.count('  // ') <= limit and not LEAF_CALLS.search(t):
            out.add('%08x' % a)
    return out


CS_BITS = {'ebx': 8, 'ebp': 32, 'esi': 64, 'edi': 128}
CRT_LO, CRT_HI = 0x00ec0000, 0x00fe0000              # the statically linked MSVC CRT (port/compat/crt_map.json)
RCALL_SITE = re.compile(r'\bRCALL\(f_([0-9a-f]{8}), (0x[0-9a-f]{8}u)\)')
RCALLI_SITE = re.compile(r'\bRCALLI\(([^;]*?), (0x[0-9a-f]{8}u)\);')


def callee_saved_pass(texts):
    """tools/callee_saved.py: calls whose callee keeps ebx/esi/edi/ebp become RCALLK/RCALLIK (fnv_recomp.h)"""
    import callee_saved as cs
    t0 = time.time()
    keeps, inputs = cs.analyze(texts)
    bits = lambda regs: sum(CS_BITS[r] for r in regs)
    n = [0, 0]

    def direct(m):
        a = m.group(1)
        k = keeps.get(a)
        if not k:
            return m.group(0)
        nr = bits(k)
        # always stored before the call: callee_saved.py's input scan is linear and misses a read on a path that
        # jumps over the write (a store skipped on that basis crashed the game). FNV_CALLEE_CHECK verifies NR.
        ns = 0
        n[0] += 1
        return 'RCALLK(f_%s, %s, 0x%xu, 0x%xu)' % (a, m.group(2), ns, nr)

    def indirect(m):
        n[1] += 1
        return 'RCALLIK(%s, %s, 0x0u, 0xe8u);' % (m.group(1), m.group(2))

    out = {}
    for a, t in texts.items():
        if CRT_LO <= a < CRT_HI:                     # the CRT's hand-written assembly (exception unwinding, setjmp,
            out[a] = t                               # thread start) breaks the convention: FNV_CALLEE_CHECK found it
            continue
        t2 = RCALL_SITE.sub(direct, t)
        out[a] = RCALLI_SITE.sub(indirect, t2)
    print('callee-saved registers kept across %d direct and %d indirect calls (%.0f s)' % (n[0], n[1], time.time() - t0),
          flush=True)
    return out


def stack_slots_pass(texts):
    """tools/stack_slots.py over every function"""
    import stack_slots as sl
    t0 = time.time()
    out, done = dict(texts), 0
    helpers = sl.frame_helpers(texts)
    print('frame-sharing helpers: %d' % len(helpers), flush=True)
    for a, t in texts.items():
        m = sl.FUNC_RE.match(t)
        if not m:
            continue
        try:
            new = sl.rewrite(m.group(2), helpers)
        except sl.NotLocal:
            continue
        out[a] = t[:m.start(2)] + new + t[m.end(2):]
        done += 1
    print('frame slots in locals: %d functions (%.0f s)' % (done, time.time() - t0), flush=True)
    return out


def small_functions(texts, limit):
    """{'%08x'} of lifted functions of at most `limit` instructions that make calls (copied like small leaves, so a
    wrapper around another call loses one call layer); not native replacements or bodies that tail-jump"""
    out = set()
    if limit <= 0:
        return out
    for a, t in texts.items():
        if t.startswith('void f_') and t.count('  // ') <= limit and not re.search(r'\b(RJMP|TAILCALL)\(', t):
            out.add('%08x' % a)
    return out


def x87_regs_pass(texts):
    """tools/x87_static.py over every function: return depths first (direct calls use them), then the rewrite"""
    import x87_static as xs
    t0 = time.time()
    bodies = {}
    for a, t in texts.items():
        m = xs.FUNC_RE.match(t)
        if m:
            bodies[a] = m.group(2)
    deltas = xs.all_deltas({'f_%08x' % a: b for a, b in bodies.items()})
    done = 0
    out = dict(texts)
    for a, b in bodies.items():
        try:
            new = xs.rewrite(b, deltas)
        except xs.NotStatic:
            continue
        if new is not None:
            m = xs.FUNC_RE.match(texts[a])
            out[a] = texts[a][:m.start(2)] + new + texts[a][m.end(2):]
            done += 1
    print('x87 stack in locals: %d functions (%.0f s)' % (done, time.time() - t0), flush=True)
    return out


# Functions the platform layer supplies instead of the lifted code (declared and registered as usual, body elsewhere):
#   0x00aa4ee0 BSTimer::Update (pal_kernel.cpp): the original counts whole milliseconds with a 10 ms floor, so above
#              100 fps (a 120 Hz screen) the game clock runs ahead of real time
HOST_FUNCS = {0x00aa4ee0}

# Native replacements in guest layout (port/recomp/native_math.cpp): the lifted body stays as fo_XXXXXXXX, the oracle
# for FNV_NATIVE_CHECK=1, and the native f_XXXXXXXX takes its place everywhere (direct calls and the dispatch table).
#   0x0043f8d0 NiMatrix3::operator*, 0x0062c250 NiTransform::operator*, 0x004b6190 NiPoint3::Dot,
#   0x004b6160 NiPlane::Distance, 0x004b6100 NiBound::WhichSide, 0x0049da80 NiCamera::ClassifyPoint
#   0x00b9bbb0 ShadowSceneLight::TestSphereFrustum (shadow culling: 3.6% of the main thread at Ultra)
# Off by default: the build uses the lifted code until `--native` is given (then native_math.cpp is built with
# -DFNV_NATIVE_MATH, which the build scripts switch on by the out/recomp/native_math.on marker).
NATIVE_FUNCS = {0x0043f8d0, 0x0062c250, 0x004b6190, 0x004b6160, 0x004b6100, 0x0049da80, 0x00b9bbb0, 0x00b9f5b0}

LOCAL_HEAD = re.compile(r'^void (f_[0-9a-f]{8})\(FnvCpu& s\)\n\{\n    FNV_ENTER\((0x[0-9a-f]{8}u)\);\n')
LOCAL_FIELDS = [('eax', r'\bs\.eax\b|\bSAHF\('), ('ecx', r'\bs\.ecx\b'), ('edx', r'\bs\.edx\b'), ('ebx', r'\bs\.ebx\b'),
                ('esp', r'.'), ('ebp', r'\bs\.ebp\b'), ('esi', r'\bs\.esi\b'), ('edi', r'\bs\.edi\b'),
                ('flags', r'\bs\.(zf|sf|cf|of|pf)\b|\b(LOGICF|ADDF|SUBF|SAHF)\('), ('df', r'\bs\.df\b')]


def local_regs(src):
    """A lifted body with its registers in locals (FnvLocal, port/recomp/fnv_recomp.h): `s` becomes the local view
    and the parameter `s_`. The mask names the fields the body uses (macros included: the stack macros use esp,
    the flag macros set all five flags)."""
    m = LOCAL_HEAD.match(src)
    if not m:
        return src                              # host intrinsics: no body of their own
    mask = sum(1 << i for i, (_, pat) in enumerate(LOCAL_FIELDS) if re.search(pat, src[m.end():]))
    return ('void %s(FnvCpu& s_)\n{\n    FNV_ENTER(%s);\n    FnvLocal<%#xu> s(s_);\n' % (m.group(1), m.group(2), mask)
            + src[m.end():])


def entries_of(img, units, text):
    starts = sorted({int(f['address'], 16) for u in units for f in u['functions']})
    sizes = {}
    for u in units:
        for f in u['functions']:
            a = int(f['address'], 16)
            sizes[a] = max(sizes.get(a, 0), f['size'])
    ent = set(starts)
    pe = struct.unpack_from('<I', img.data, 0x3c)[0]
    pe_entry = img.base + struct.unpack_from('<I', img.data, pe + 0x28)[0]
    ent.add(pe_entry)
    from_data = 0
    for site in img.relocs():
        if text[0] <= site < text[1]:
            continue                            # immediates are found while lifting; jump tables are labels
        v = img.u32(site)
        if v is not None and text[0] <= v < text[1]:
            ent.add(v)
            from_data += 1
    return starts, sizes, ent, pe_entry, from_data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', required=True)
    ap.add_argument('--shards', type=int, default=64)
    ap.add_argument('--only', default='', help='comma-separated entries (a quick partial build for testing)')
    ap.add_argument('--no-cache', action='store_true')
    ap.add_argument('--small-max', type=int, default=0, help='also copy functions that make calls, up to this size')
    ap.add_argument('--leaf-max', type=int, default=LEAF_MAX, help='inline copies of leaf functions up to this size (0: off)')
    ap.add_argument('--no-callee-saved', action='store_true', help='every call stores and reloads all registers')
    ap.add_argument('--no-stack-slots', action='store_true', help='frame slots stay in guest memory (tools/stack_slots.py off)')
    ap.add_argument('--no-inline-imports', action='store_true', help='every import call dispatches (INLINE_IMPORTS off)')
    ap.add_argument('--no-x87-regs', action='store_true', help='keep the x87 stack in memory everywhere (tools/x87_static.py off)')
    ap.add_argument('--native', action='store_true', help='use the hand-written versions of NATIVE_FUNCS (port/recomp/native_math.cpp) instead of the lifted code')
    a = ap.parse_args()
    t0 = time.time()
    img = Image(a.exe)
    tsec = next(s for s in img.sections if s['name'] == '.text')
    text = (tsec['start'], tsec['start'] + tsec['vsize'])
    units = json.loads((Path(DECOMP_DIR) / 'build/units.json').read_text())['units']
    names = {}
    for u in units:
        for f in u['functions']:
            names.setdefault(int(f['address'], 16), f['symbol'])
        for k, v in u.get('symbols', {}).items():
            names.setdefault(int(v, 0), k)
    starts, sizes, ent, pe_entry, from_data = entries_of(img, units, text)
    if a.only:
        ent = {int(x, 16) for x in a.only.split(',')}
    print('entries: %d (%d recipe starts, %d from data fixups), entry point %#x' %
          (len(ent), len(starts), from_data, pe_entry), flush=True)
    rc = Recompiler(a.exe, text, starts, sizes, names)
    rc.run(sorted(ent), use_cache=not a.no_cache)
    OUT.mkdir(parents=True, exist_ok=True)
    iat = {} if a.no_inline_imports else {va: n.split('!', 1)[1] for va, n in img.imports().items()}
    shards = rc.shards(a.shards, x87_regs=not a.no_x87_regs, iat=iat, leaf_max=a.leaf_max,
                       stack_slots=not a.no_stack_slots, callee_saved=not a.no_callee_saved, small_max=a.small_max,
                       native=NATIVE_FUNCS if a.native else ())
    marker = OUT / 'native_math.on'                  # tells the build scripts to compile native_math.cpp's bodies
    if a.native:
        marker.write_text('\n'.join('%08x' % x for x in sorted(NATIVE_FUNCS)) + '\n')
    elif marker.exists():
        marker.unlink()
    names_out = {'recomp_%02d.cpp' % k for k in range(len(shards))}
    for old in OUT.glob('recomp_*.cpp'):
        if old.name not in names_out:
            old.unlink()
    for k, src in enumerate(shards):                 # unchanged shards keep their timestamps (make rebuilds less)
        path = OUT / ('recomp_%02d.cpp' % k)
        if not path.exists() or path.read_text() != src:
            path.write_text(src)
    from collections import Counter
    unsup = Counter(v.split(' ')[0] for v in rc.unsupported.values())
    report = dict(entries=len(ent), lifted=len(rc.lifted), traps=len(rc.traps), unsupported=len(rc.unsupported),
                  unsupported_top=unsup.most_common(20), cache_hits=rc.cache_hits, entry_point='%#x' % pe_entry,
                  seconds=round(time.time() - t0))
    (OUT / 'report.json').write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
fuzz_gen.py — random loop programs for differential testing (make fuzz).

    python3 bench/fuzz_gen.py SEED c    -> a random C-minus program
    python3 bench/fuzz_gen.py SEED py   -> what that program must print,
                                          computed by a small reference
                                          interpreter written in Python

The programs mix for/while loops (nested up to three deep), break, globals
changed by a function called inside expressions, array reads and writes,
block-scoped shadowing variables and every relational operator.  The same
seed always gives the same program, so a failure is reproducible.
Semantics modelled: 32-bit wrap-around, comparisons yield 1/0, STRICT
left-to-right evaluation (the language's documented rule).
"""
import random, sys
seed = int(sys.argv[1]); mode = sys.argv[2]   # 'c' or 'py'
r = random.Random(seed)
V = ['a','b','c','d']
# Build an AST once, then render to C-minus or Python.
def expr(d=0):
    if d > 2 or r.random() < 0.3:
        c = r.random()
        if c < 0.4: return ('num', r.randint(-3, 9))
        if c < 0.55: return ('var', 'g')
        if c < 0.65: return ('arr', r.randint(0,3))
        if c < 0.72: return ('call', expr(3))
        return ('var', r.choice(V))
    return ('bin', r.choice(['+','-','*','<','>','<=','>=','==','!=','+','*']), expr(d+1), expr(d+1))
def noa(e):
    # the block's initialiser must not read the NEW (uninitialised) inner a
    if e[0] == 'var' and e[1] == 'a': return ('var', 'b')
    if e[0] == 'bin': return ('bin', e[1], noa(e[2]), noa(e[3]))
    if e[0] == 'call': return ('call', noa(e[1]))
    return e
def stmt(depth, inloop):
    c = r.random()
    if depth < 3 and c < 0.18:
        return ('for', 'i%d' % depth, r.randint(0, 4), [stmt(depth+1, True) for _ in range(r.randint(1,3))])
    if depth < 3 and c < 0.3:
        return ('while', 'i%d' % depth, r.randint(0, 4), [stmt(depth+1, True) for _ in range(r.randint(1,3))])
    if inloop and c < 0.36: return ('wbreak', expr(2))
    if c < 0.42: return ('print', expr())
    if c < 0.5: return ('astore', r.randint(0,3), expr())
    if c < 0.56: return ('assign', 'g', expr())
    if c < 0.62: return ('block', noa(expr()), expr(2))
    return ('assign', r.choice(V), expr())
init = (r.randint(0,5), r.randint(0,5), [r.randint(0,9) for _ in range(4)])
body = [stmt(0, False) for _ in range(r.randint(3, 8))]

def ce(e):
    t = e[0]
    if t == 'num': return str(e[1])
    if t == 'var': return e[1]
    if t == 'arr': return 'arr[%d]' % e[1]
    if t == 'call': return 'f(%s)' % ce(e[1])
    return '(%s %s %s)' % (ce(e[2]), e[1], ce(e[3]))
def cs(s):
    t = s[0]
    if t == 'for':
        lv = s[1]; return 'for (%s = 0; %s < %d; %s = %s + 1) {\n%s\n}' % (lv, lv, s[2], lv, lv, '\n'.join(map(cs, s[3])))
    if t == 'while':
        lv = s[1]; return '%s = %d;\nwhile (%s > 0) {\n%s = %s - 1;\n%s\n}' % (lv, s[2], lv, lv, lv, '\n'.join(map(cs, s[3])))
    if t == 'wbreak': return 'while (%s) { break; }' % ce(s[1])
    if t == 'print': return 'print(%s);' % ce(s[1])
    if t == 'astore': return 'arr[%d] = %s;' % (s[1], ce(s[2]))
    if t == 'assign': return '%s = %s;' % (s[1], ce(s[2]))
    if t == 'block': return '{ int a; a = %s; print(a + %s); }' % (ce(s[1]), ce(s[2]))

if mode == 'c':
    L = ['int g;', 'int arr[4];', 'int f(int x) { g = g + 1; return x * 2 - g; }', 'int main() {',
         '  int a; int b; int c; int d; int i0; int i1; int i2;',
         '  a = %d; b = %d; c = 0; d = 1; g = 0;' % init[:2]]
    for k in range(4): L.append('  arr[%d] = %d;' % (k, init[2][k]))
    L += [cs(s) for s in body]
    L += ['  print(a); print(b); print(c); print(d); print(g);', '  return 0;', '}']
    print('\n'.join(L))
else:
    def w(v):
        v &= 0xFFFFFFFF
        return v - (1 << 32) if v & 0x80000000 else v
    env = {'a': init[0], 'b': init[1], 'c': 0, 'd': 1, 'g': 0, 'i0': 0, 'i1': 0, 'i2': 0}
    arr = list(init[2]); out = []
    def ev(e, loc):
        t = e[0]
        if t == 'num': return e[1]
        if t == 'var': return loc[e[1]] if e[1] in loc else env[e[1]]
        if t == 'arr': return arr[e[1]]
        if t == 'call':
            x = ev(e[1], loc); env['g'] = w(env['g'] + 1); return w(x * 2 - env['g'])
        x = ev(e[2], loc); y = ev(e[3], loc); op = e[1]
        if op == '+': return w(x + y)
        if op == '-': return w(x - y)
        if op == '*': return w(x * y)
        return int({'<': x < y, '>': x > y, '<=': x <= y, '>=': x >= y, '==': x == y, '!=': x != y}[op])
    def ex(s, loc={}):
        t = s[0]
        if t == 'for':
            env[s[1]] = 0
            while env[s[1]] < s[2]:
                for b in s[3]: ex(b)
                env[s[1]] = w(env[s[1]] + 1)
        elif t == 'while':
            env[s[1]] = s[2]
            while env[s[1]] > 0:
                env[s[1]] = w(env[s[1]] - 1)
                for b in s[3]: ex(b)
        elif t == 'wbreak': ev(s[1], {})
        elif t == 'print': out.append(ev(s[1], {}))
        elif t == 'astore': arr[s[1]] = ev(s[2], {})
        elif t == 'assign': env[s[1]] = ev(s[2], {})
        elif t == 'block':
            l = {'a': ev(s[1], {})}; out.append(w(l['a'] + ev(s[2], l)))
    for s in body: ex(s)
    out += [env['a'], env['b'], env['c'], env['d'], env['g']]
    print(' '.join(map(str, out)))

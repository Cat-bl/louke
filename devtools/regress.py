# Louke regression test: replays a movie with the real game and checks that the TAS stays exact.
# Run it before merging changes to the core (needs the game installed; build.bat first).
#
#   python devtools\regress.py --movie <movie.txt> --game <game.exe> [--states] [--export]
#
# Checks:
#   - the movie is replayed --runs times (default 2): every frame must look the same in every run
#     (a hash of the picture of each frame is compared);
#   - --states: one more replay that saves and loads savestates on the way (save slot 2 at 33 %,
#     slot 1 at 52 %, load slot 1 at 65 %, slot 2 at 78 % of the movie); after the loads every
#     frame must still be the same as in the plain replay;
#   - --export: one more replay that exports the whole movie to video; the video must have one
#     frame per movie frame and the game must look the same as in the plain replay.
# Frames that differ for reasons outside the TAS (videos the game plays in real time, such as an
# opening movie) can be left out of the comparison with --ignore 640-790,....
# KOF XIII: its opening movie plays from about frame 640 until skipped or about frame 790, so use
# --ignore 640-800 for movies that start the game normally.
#
# A replay that stops progressing for --hang seconds counts as a hang: the stacks of the game's
# threads are written next to the logs (shots\regress, not part of the repository) and the game
# is ended. Only processes this script started are ended. Exit code 0 when everything passed.
import argparse, ctypes, ctypes.wintypes as wt, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, 'bin', 'louke.log')
LAUNCH = os.path.join(ROOT, 'devtools', 'testlaunch.exe')
WORK = os.path.join(ROOT, 'shots', 'regress')


def movie_frames(path):
    n, body = 0, False
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        if body: n += 1
        elif line == '---': body = True
    return n


def read_log():
    try:
        return open(LOG, encoding='utf-8', errors='replace').read()
    except OSError:
        return ''


def dump_stacks(pid, out):
    """Write every thread of the (32-bit) game process with its code addresses on the stack."""
    k32 = ctypes.WinDLL('kernel32'); psapi = ctypes.WinDLL('psapi')
    k32.OpenProcess.restype = wt.HANDLE; k32.OpenThread.restype = wt.HANDLE
    hp = k32.OpenProcess(0x1F0FFF, False, pid)
    arr = (wt.HMODULE * 1024)(); need = wt.DWORD()
    psapi.EnumProcessModulesEx(wt.HANDLE(hp), arr, ctypes.sizeof(arr), ctypes.byref(need), 1)

    class MI(ctypes.Structure): _fields_ = [('base', ctypes.c_void_p), ('size', wt.DWORD), ('entry', ctypes.c_void_p)]
    mods = []
    for i in range(need.value // ctypes.sizeof(wt.HMODULE)):
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(wt.HANDLE(hp), wt.HMODULE(arr[i]), name, 260)
        mi = MI()
        psapi.GetModuleInformation(wt.HANDLE(hp), wt.HMODULE(arr[i]), ctypes.byref(mi), ctypes.sizeof(mi))
        mods.append((mi.base or 0, mi.size, name.value))

    def where(a):
        for b, s, n in mods:
            if b <= a < b + s: return '%s+%X' % (n, a - b)
        return None

    class CTX(ctypes.Structure):   # WOW64_CONTEXT
        _fields_ = [('ContextFlags', wt.DWORD), ('Dr', wt.DWORD * 6), ('FloatSave', ctypes.c_byte * 112),
                    ('Seg', wt.DWORD * 4), ('Edi', wt.DWORD), ('Esi', wt.DWORD), ('Ebx', wt.DWORD), ('Edx', wt.DWORD),
                    ('Ecx', wt.DWORD), ('Eax', wt.DWORD), ('Ebp', wt.DWORD), ('Eip', wt.DWORD), ('SegCs', wt.DWORD),
                    ('EFlags', wt.DWORD), ('Esp', wt.DWORD), ('SegSs', wt.DWORD), ('Ext', ctypes.c_byte * 512)]

    class TE(ctypes.Structure):
        _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('tid', wt.DWORD), ('pid', wt.DWORD),
                    ('base', wt.LONG), ('delta', wt.LONG), ('flags', wt.DWORD)]
    snap = k32.CreateToolhelp32Snapshot(4, 0)
    te = TE(); te.dwSize = ctypes.sizeof(TE)
    tids = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.pid == pid: tids.append(te.tid)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    with open(out, 'w', encoding='utf-8') as f:
        for tid in tids:
            ht = k32.OpenThread(0x1FFFFF, False, tid)
            k32.Wow64SuspendThread(wt.HANDLE(ht))
            c = CTX(); c.ContextFlags = 0x10007
            got_ctx = k32.Wow64GetThreadContext(wt.HANDLE(ht), ctypes.byref(c))
            buf = (ctypes.c_uint32 * 2048)(); got = ctypes.c_size_t()
            if got_ctx:
                k32.ReadProcessMemory(wt.HANDLE(hp), ctypes.c_void_p(c.Esp), buf, ctypes.sizeof(buf), ctypes.byref(got))
            k32.ResumeThread(wt.HANDLE(ht)); k32.CloseHandle(wt.HANDLE(ht))
            if not got_ctx: f.write('thread %d: no context\n' % tid); continue
            f.write('thread %d eip %s\n' % (tid, where(c.Eip) or hex(c.Eip)))
            shown = 0
            for i in range(got.value // 4):
                w = where(buf[i])
                if w:
                    f.write('    [+%04X] %s\n' % (i * 4, w)); shown += 1
                    if shown >= 40: break
    k32.CloseHandle(wt.HANDLE(hp))


def replay(args, name, env_extra=None, export=None):
    """One replay. Returns (status, hashes {frame: hash}, log text)."""
    movie = os.path.join(WORK, name + '_movie.txt')
    shutil.copyfile(args.movie, movie)              # the movie is saved again at its end
    hashes = os.path.join(WORK, name + '_hash.txt')
    for p in (hashes, export):
        if p and os.path.exists(p): os.remove(p)
    env = dict(os.environ, LOUKE_FRAMEHASH=hashes)
    env.update(env_extra or {})
    cmd = [LAUNCH, 'play', movie]
    if args.game: cmd += ['--game', args.game]
    if export: cmd += ['--export', export, '1']
    print('  %s: replaying ...' % name, flush=True)
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    first = p.stdout.readline().decode(errors='replace').split()
    pid = int(first[1]) if len(first) == 2 and first[0] == 'pid' else None
    t0 = time.time(); last_size, last_change = -1, time.time(); status = 'timeout'
    done_mark = 'export finished' if export else 'movie end reached'
    while time.time() - t0 < args.timeout:
        time.sleep(1)
        txt = read_log()
        if done_mark in txt: status = 'ok'; time.sleep(1); break
        if export and 'export: ' in txt and 'kbit/s' not in txt.split('export: ')[-1].split('\n')[0]: status = 'export failed'; break
        if p.poll() is not None: status = 'game exited'; break
        size = os.path.getsize(hashes) if os.path.exists(hashes) else 0
        if size != last_size: last_size, last_change = size, time.time()
        elif time.time() - t0 > 90 and time.time() - last_change > args.hang:
            status = 'hang'
            if pid:
                out = os.path.join(WORK, name + '_stacks.txt')
                try: dump_stacks(pid, out); print('    thread stacks: ' + out)
                except Exception as e: print('    (stacks not written: %s)' % e)
            break
    if pid: subprocess.run(['taskkill', '/F', '/PID', str(pid)], capture_output=True)
    try: p.wait(timeout=30)
    except subprocess.TimeoutExpired: pass
    txt = read_log()
    with open(os.path.join(WORK, name + '_log.txt'), 'w', encoding='utf-8') as f: f.write(txt)
    h = {}
    if os.path.exists(hashes):
        for line in open(hashes):
            q = line.split()
            if len(q) == 2: h[int(q[0])] = q[1]           # after a load a frame is seen again: the last one counts
    print('    %s, %.0f s, %d frames seen' % (status, time.time() - t0, len(h)))
    if status != 'ok':
        print('    log: ' + os.path.join(WORK, name + '_log.txt'))
    return status, h, txt


def compare(a, b, ignore, label):
    frames = [f for f in sorted(set(a) & set(b)) if not any(lo <= f <= hi for lo, hi in ignore)]
    diff = [f for f in frames if a[f] != b[f]]
    if diff:
        print('  FAIL %s: %d of %d frames differ, between frames %d and %d' % (label, len(diff), len(frames), diff[0], diff[-1]))
        return False
    print('  ok   %s: %d frames identical' % (label, len(frames)))
    return True


def main():
    ap = argparse.ArgumentParser(description='Louke regression test (replays a movie with the real game).')
    ap.add_argument('--movie', required=True, help='movie to replay (not changed)')
    ap.add_argument('--game', help='game executable (default: the one built into testlaunch)')
    ap.add_argument('--runs', type=int, default=2, help='plain replays to compare (default 2)')
    ap.add_argument('--states', action='store_true', help='also replay with savestate saves and loads')
    ap.add_argument('--export', action='store_true', help='also export the movie to video')
    ap.add_argument('--ignore', default='', help='frame ranges left out of comparisons, e.g. 640-790,1000-1010')
    ap.add_argument('--timeout', type=float, default=900, help='seconds per replay (default 900)')
    ap.add_argument('--hang', type=float, default=30, help='seconds without progress that count as a hang (default 30)')
    args = ap.parse_args()
    if not os.path.exists(LAUNCH): sys.exit('devtools\\testlaunch.exe not found: run build.bat first')
    ignore = []
    for r in filter(None, args.ignore.split(',')):
        lo, _, hi = r.partition('-')
        ignore.append((int(lo), int(hi or lo)))
    os.makedirs(WORK, exist_ok=True)
    n = movie_frames(args.movie)
    print('movie: %s (%d frames)' % (args.movie, n))
    ok = True
    plain = []
    for k in range(max(1, args.runs)):
        st, h, _ = replay(args, 'run%d' % (k + 1))
        ok &= st == 'ok'
        plain.append(h)
    for k in range(1, len(plain)):
        ok &= compare(plain[0], plain[k], ignore, 'replay 1 vs replay %d' % (k + 1))
    if args.states:
        plan = 's2@%d,s1@%d,l1@%d,l2@%d' % (n * 33 // 100, n * 52 // 100, n * 65 // 100, n * 78 // 100)
        st, h, txt = replay(args, 'states', {'LOUKE_STATETEST': plan})
        loaded = 'state 1 loaded' in txt and 'state 2 loaded' in txt
        if not loaded: print('  FAIL savestates: not both loads happened (%s)' % plan)
        ok &= st == 'ok' and loaded and compare(plain[0], h, ignore, 'replay with savestates (%s) vs replay 1' % plan)
    if args.export:
        mp4 = os.path.join(WORK, 'export.mp4')
        st, h, txt = replay(args, 'export', export=mp4)
        ok &= st == 'ok' and compare(plain[0], h, ignore, 'replay while exporting vs replay 1')
        want = 'export finished: %d frames' % n
        if want in txt and os.path.exists(mp4) and os.path.getsize(mp4) > 0: print('  ok   video: %d frames (%s)' % (n, mp4))
        else: print('  FAIL video: expected "%s" in the log' % want); ok = False
    print('PASSED' if ok else 'FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()

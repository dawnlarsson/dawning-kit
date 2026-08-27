#!/usr/bin/env python3
"""
What can a test suite not see?

A suite that passes tells you nothing on its own: it passes on the code it
was written against. Change the code by one, and if it still passes, that
change is invisible to it -- and so is the bug that would have looked the
same. This makes those changes one at a time and reports the ones nothing
noticed.

Three outcomes, not two. A mutation is caught when its lane fails, survived
when the lane still passes, and invalid when the result does not build or
does not stop. Folding invalid into either of the others is how this stops
being useful: into survived and the list drowns, into caught and the gaps
hide.

Every mutation is named by the routine it is in and the text it changed,
never by a line number, because these files move under it. A survivor that
has been looked at and dismissed goes in the ledger with the reason, and the
next run separates ones already judged from ones nobody has seen.

    python3 kit/mutate.d/mutate.py --list            what would be tried
    python3 kit/mutate.d/mutate.py --calibrate       the known bugs, caught
    python3 kit/mutate.d/mutate.py                   the whole run
    python3 kit/mutate.d/mutate.py --target text     one target only

Run it where the suite runs.
"""
import argparse, concurrent.futures, hashlib, json, os, re, shutil
import subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LEDGER = os.path.join(ROOT, 'kit', 'mutate.d', 'ledger.json')

#
#       What is mutated, and the lane that would have to notice.
#
#       Mutating one file and running a lane that never touches it proves
#       nothing, so the pairing is written down rather than assumed.
#
TARGETS = {
    'library': ('src/library.c',  'verify'),
    'text':    ('src/sh/text.c',  'text'),
    'shell':   ('src/sh/lex.c',   'shell'),
    'parse':   ('src/sh/parse.c', 'shell'),
    'expand':  ('src/sh/expand.c','shell'),
    'builtin': ('src/sh/builtin.c','shell'),
    'file':    ('src/sh/file.c',  'files'),
}

#
#       The SWAR constants are structural: every byte of them carries a bit
#       pattern the algorithm is built on, and a plus one turns them into a
#       number that means nothing rather than into a different bound. They
#       are excluded here and mutated by their own operator or not at all.
#
STRUCTURAL = {'0x0101010101010101', '0x8080808080808080', '0x7f7f7f7f7f7f7f7f',
              '0x0101', '0x7f7f', '0x8080', '0xff', '0xfff', '0xff8'}

def routines_of(text, path):
    """Where each routine starts, so a mutation can be named by the one it is in."""
    marks = []
    if path.endswith('library.c'):
        for m in re.finditer(r'ASM_FUNC\(([A-Za-z0-9_]+)\)', text):
            marks.append((m.start(), m.group(1)))
    for m in re.finditer(r'^[A-Za-z_][A-Za-z0-9_ *]*?\b([a-z_][a-z0-9_]*)\s*\([^;]*$',
                         text, re.M):
        marks.append((m.start(), m.group(1)))
    marks.sort()
    return marks

def routine_at(marks, pos):
    name = '(file)'
    for start, n in marks:
        if start > pos:
            break
        name = n
    return name

def in_data(line):
    return '.ascii' in line or '.asciz' in line or '.string' in line

def numbers(text, path):
    """Every numeric literal worth changing, with where it is."""
    out = []
    marks = routines_of(text, path)
    lines = text.split('\n')
    offset = 0
    for line in lines:
        base, offset = offset, offset + len(line) + 1
        s = line.strip()
        if in_data(line) or s.startswith('//') or s.startswith('#include'):
            continue
        # a number after $ or # is an immediate; a bare one is an offset or a
        # count. A digit inside a register name is neither, and the lookbehind
        # is what keeps x0, %r10, t4 and w9 out of this.
        for m in re.finditer(r'([$#])(-?0x[0-9a-fA-F]+|-?\d+)'
                             r'|(?<![\w#$.])(-?0x[0-9a-fA-F]+|-?\d+)(?![\w])',
                             line):
            raw = m.group(2) or m.group(3)
            if raw in STRUCTURAL or raw.lstrip('-') in STRUCTURAL:
                continue
            try:
                value = int(raw, 16) if raw.lower().startswith(('0x', '-0x')) else int(raw)
            except ValueError:
                continue
            if abs(value) > 0xffff:          # a mask or a magic number, not a bound
                continue
            span = (base + m.start(2 if m.group(2) else 3),
                    base + m.end(2 if m.group(2) else 3))
            out.append((span, raw, value, routine_at(marks, base)))
    return out

def plus_minus(text, path):
    """The off by one operator, which is the one the real bugs were."""
    for (a, b), raw, value, routine in numbers(text, path):
        for delta in (1, -1):
            new = value + delta
            if raw.lower().startswith(('0x', '-0x')):
                shown = ('-0x%x' % -new) if new < 0 else ('0x%x' % new)
            else:
                shown = str(new)
            yield {'operator': 'plusminus', 'routine': routine,
                   'was': raw, 'now': shown, 'at': a, 'end': b}

FLIPS = [('jne', 'je'), ('je', 'jne'), ('jae', 'jb'), ('jb', 'jae'),
         ('jbe', 'ja'), ('ja', 'jbe'), ('jz', 'jnz'), ('jnz', 'jz'),
         ('b.eq', 'b.ne'), ('b.ne', 'b.eq'), ('b.lo', 'b.hs'), ('b.hs', 'b.lo'),
         ('cbz', 'cbnz'), ('cbnz', 'cbz'),
         ('beqz', 'bnez'), ('bnez', 'beqz'), ('beq', 'bne'), ('bne', 'beq'),
         ('bltu', 'bgeu'), ('bgeu', 'bltu')]

def branches(text, path):
    """Flip the sense of a branch: the other half of an off by one."""
    marks = routines_of(text, path)
    for was, now in FLIPS:
        for m in re.finditer(r'(?<![\w.])' + re.escape(was) + r'(?=\s)', text):
            line_start = text.rfind('\n', 0, m.start()) + 1
            line = text[line_start:text.find('\n', m.start())]
            if in_data(line) or line.strip().startswith('//'):
                continue
            yield {'operator': 'branch', 'routine': routine_at(marks, m.start()),
                   'was': was, 'now': now, 'at': m.start(), 'end': m.end()}

RELATIONS = [('<=', '<'), ('<', '<='), ('>=', '>'), ('>', '>='),
             ('==', '!='), ('!=', '==')]

def relations(text, path):
    """The same idea in C: a bound that is off by one is a comparison."""
    if path.endswith('library.c'):
        return
    marks = routines_of(text, path)
    for was, now in RELATIONS:
        for m in re.finditer(r'(?<![<>=!])' + re.escape(was) + r'(?![<>=])', text):
            line_start = text.rfind('\n', 0, m.start()) + 1
            end = text.find('\n', m.start())
            line = text[line_start:end if end > 0 else len(text)]
            if line.strip().startswith('//') or line.strip().startswith('*'):
                continue
            yield {'operator': 'relation', 'routine': routine_at(marks, m.start()),
                   'was': was, 'now': now, 'at': m.start(), 'end': m.end()}


def targets_of_branches(text, path):
    """
    Where a branch goes, not whether it is taken.

    This is the operator that would have found the string_find bug: the
    branch was right, its destination was one label out, and the scan
    resumed where the candidate stopped instead of where it began. A
    condition flip could never produce that.
    """
    marks = routines_of(text, path)
    for m in re.finditer(r'(?<![\w.])(\d)([fb])(?![\w])', text):
        line_start = text.rfind('\n', 0, m.start()) + 1
        end = text.find('\n', m.start())
        line = text[line_start:end if end > 0 else len(text)]
        if in_data(line) or line.strip().startswith('//'):
            continue
        # backwards instead of forwards, and the label either side of it
        for now in (m.group(1) + ('b' if m.group(2) == 'f' else 'f'),
                    str(int(m.group(1)) + 1) + m.group(2)):
            if now == m.group(0):
                continue
            yield {'operator': 'target', 'routine': routine_at(marks, m.start()),
                   'was': m.group(0), 'now': now, 'at': m.start(), 'end': m.end()}


SWAR = ['0x0101010101010101', '0x8080808080808080', '0x7f7f7f7f7f7f7f7f']

def swar_constants(text, path):
    """
    One SWAR constant for another.

    The strrchr bug was exactly this shape: the three instruction zero test
    where the five instruction one was needed, which reads as the wrong
    constant beside the right instructions. Plus one on these means nothing,
    so they are swapped for each other instead.
    """
    marks = routines_of(text, path)
    for was in SWAR:
        for m in re.finditer(re.escape(was), text):
            line_start = text.rfind('\n', 0, m.start()) + 1
            end = text.find('\n', m.start())
            line = text[line_start:end if end > 0 else len(text)]
            if in_data(line) or line.strip().startswith('//'):
                continue
            for now in SWAR:
                if now == was:
                    continue
                yield {'operator': 'swar', 'routine': routine_at(marks, m.start()),
                       'was': was, 'now': now, 'at': m.start(), 'end': m.end()}


OPERATORS = {'plusminus': plus_minus, 'branch': branches,
             'relation': relations, 'target': targets_of_branches,
             'swar': swar_constants}

def identify(target, m, text):
    """A name that survives the file moving: the routine and the text around it."""
    context = text[max(0, m['at'] - 40):m['at'] + 40]
    key = f"{target}|{m['routine']}|{m['operator']}|{m['was']}->{m['now']}|" \
          f"{hashlib.sha1(context.encode()).hexdigest()[:8]}"
    return key

def build(target, operators, only_routine=None):
    path, lane = TARGETS[target]
    text = open(os.path.join(ROOT, path)).read()
    seen, out = set(), []
    for name in operators:
        for m in OPERATORS[name](text, path):
            if only_routine and m['routine'] != only_routine:
                continue
            m['target'], m['lane'], m['path'] = target, lane, path
            m['id'] = identify(target, m, text)
            start = text.rfind('\n', 0, m['at']) + 1
            stop = text.find('\n', m['at'])
            m['line'] = text[start:stop if stop > 0 else len(text)].strip()
            m['lineno'] = text.count('\n', 0, m['at']) + 1
            if m['id'] in seen:
                continue
            seen.add(m['id'])
            out.append(m)
    return out, text

class Workers:
    """
    One tree per worker, made once and reused, on a real disk.

    Copying the repository for every mutation is most of the wall clock when
    the lane itself takes a second and a half. Each worker keeps its own
    tree, and a mutation is one file written into it.

    Not under /tmp. That is a tmpfs on the machine this runs on, it is shared
    with whatever else is building there, and a mutation can produce a
    program that writes without stopping -- one run filled thirty gigabytes
    of somebody else's RAM before anything noticed. Each worker gets its own
    directory beside the repository instead, and the lane's own scratch is
    pointed inside it so that cleaning the worker cleans everything it made.
    """
    def __init__(self, count, where):
        self.free = []
        self.made = []
        os.makedirs(where, exist_ok=True)
        for _ in range(count):
            work = tempfile.mkdtemp(prefix='w.', dir=where)
            subprocess.run(['rsync', '-a', '--exclude', '.git', '--exclude', 'dist',
                            '--exclude', 'linux', '--exclude', 'fs',
                            ROOT + '/', work + '/'], check=True, capture_output=True)
            self.free.append(work)
            self.made.append(work)
        self.lock = __import__('threading').Lock()

    def take(self):
        with self.lock:
            return self.free.pop()

    def give(self, work):
        with self.lock:
            self.free.append(work)

    def clean(self):
        for work in self.made:
            shutil.rmtree(work, ignore_errors=True)


BUILDS = {
    'src/library.c': [
        ['sh', '-c',
         'printf \'#include "library.c"\\n\' > $0/tu.c && '
         'for t in x86_64 aarch64 riscv64; do '
         '  clang --target=$t-unknown-linux-gnu -c $0/tu.c -o /dev/null '
         '    -I$0/src -nostdlib -ffreestanding -fno-builtin -O2 '
         '    -fno-stack-protector -fno-PIE || exit 1; '
         'done'],
    ],
}

def builds(work, path, env=None):
    """
    Does the mutated file still assemble?

    Asked separately, because a mutation that does not build is not a gap in
    the tests and should not be counted as one either way. It is also far
    cheaper to find out here than by running a whole lane.
    """
    recipe = BUILDS.get(path)
    if recipe is None:
        r = subprocess.run(['gcc', '-fsyntax-only', '-I' + os.path.join(work, 'src'),
                            os.path.join(work, path)],
                           capture_output=True, cwd=work, env=env)
        return r.returncode == 0
    for cmd in recipe:
        r = subprocess.run(cmd + [work], capture_output=True, cwd=work, env=env)
        if r.returncode != 0:
            return False
    return True



def run_group(argv, cwd, env, timeout):
    """
    Run it in its own process group, and kill the group.

    A mutation can produce a program that never stops, and killing the shell
    that started it leaves the program behind: orphaned, still spinning, still
    writing. Twenty six of them survived one interrupted run here and two were
    filling a disk through file handles nothing could see, because the files
    were already unlinked. Only the parent had been killed.

    So every lane gets its own session, and the timeout and every other way
    out kill the whole group by its negative pid rather than the one process
    that happens to be at the top of it.
    """
    child = subprocess.Popen(argv, cwd=cwd, env=env, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             start_new_session=True)
    try:
        out, err = child.communicate(timeout=timeout)
        return subprocess.CompletedProcess(argv, child.returncode, out, err)
    except subprocess.TimeoutExpired:
        _end(child)
        raise
    except BaseException:
        _end(child)
        raise


def _end(child):
    import signal, time
    for how in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(os.getpgid(child.pid), how)
        except (ProcessLookupError, PermissionError):
            return
        try:
            child.wait(timeout=5)
            return
        except subprocess.TimeoutExpired:
            time.sleep(0.2)


def run_one(m, text, timeout, workers):
    work = workers.take()
    try:
        mutated = text[:m['at']] + m['now'] + text[m['end']:]
        target = os.path.join(work, m['path'])
        open(target, 'w').write(mutated)
        # Every compiler and every lane writes its scratch inside the worker.
        # A compiler that cannot write its assembly file fails exactly the way
        # a broken mutation does, so a full /tmp elsewhere on the machine would
        # quietly turn a whole run into "invalid" without ever saying why.
        scratch = os.path.join(work, 'scratch')
        os.makedirs(scratch, exist_ok=True)
        env = dict(os.environ, TMPDIR=scratch, TEMP=scratch, TMP=scratch)
        try:
            if not builds(work, m['path'], env):
                return 'invalid', 'did not build'
            r = run_group(['sh', 'src/test/run', m['lane']], work, env, timeout)
        except subprocess.TimeoutExpired:
            return 'invalid', 'did not stop'
        finally:
            shutil.copyfile(os.path.join(ROOT, m['path']), target)
            # A mutant can leave a running program's output behind, and a
            # mutant that never stops leaves a lot of it.
            shutil.rmtree(os.path.join(work, 'scratch'), ignore_errors=True)
        if r.returncode == 0 and 'everything agrees' in r.stdout:
            return 'survived', ''
        if r.returncode == 0:
            return 'invalid', 'lane did not report'
        return 'caught', ''
    finally:
        workers.give(work)


def load_ledger():
    try:
        return json.load(open(LEDGER))
    except (OSError, ValueError):
        return {}

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--target', action='append')
    p.add_argument('--operator', action='append',
                   choices=sorted(OPERATORS), default=None)
    p.add_argument('--routine')
    p.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    p.add_argument('--timeout', type=int, default=45)
    p.add_argument('--work', default=os.path.join(ROOT, '..', 'mutate-work'),
                   help='where the worker trees go; not /tmp on purpose')
    p.add_argument('--limit', type=int)
    p.add_argument('--list', action='store_true')
    args = p.parse_args()

    targets = args.target or list(TARGETS)
    operators = args.operator or ['plusminus']

    plan = []
    texts = {}
    for t in targets:
        got, text = build(t, operators, args.routine)
        texts[t] = text
        plan += got

    if args.limit:
        plan = plan[:args.limit]

    if args.list:
        for m in plan:
            print(f"{m['target']:8} {m['routine']:26} {m['operator']:10} "
                  f"{m['was']:>10} -> {m['now']}")
        print(f"\n{len(plan)} mutations over {len(targets)} targets")
        return 0

    ledger = load_ledger()
    print(f"{len(plan)} mutations, {args.jobs} at a time")

    caught = survived = invalid = 0
    new_survivors, known_survivors = [], []

    workers = Workers(args.jobs, os.path.abspath(args.work))
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_one, m, texts[m['target']], args.timeout, workers): m
                   for m in plan}
        for done in concurrent.futures.as_completed(futures):
            m = futures[done]
            verdict, why = done.result()
            if verdict == 'caught':
                caught += 1
            elif verdict == 'invalid':
                invalid += 1
            else:
                survived += 1
                (known_survivors if m['id'] in ledger else new_survivors).append(m)
            n = caught + survived + invalid
            if n % 25 == 0:
                print(f"  {n}/{len(plan)}", flush=True)

    workers.clean()

    print(f"\n  caught   {caught}")
    print(f"  invalid  {invalid}   (did not build, or did not stop)")
    print(f"  survived {survived}   ({len(new_survivors)} nobody has judged)")

    if known_survivors:
        print(f"\nsurvivors already in the ledger:")
        for m in sorted(known_survivors, key=lambda x: (x['target'], x['routine'])):
            print(f"  {m['path']}:{m['lineno']}  {m['routine']}  "
                  f"{m['was']} -> {m['now']}")
            print(f"      {ledger[m['id']].get('why', 'no reason written down')}")

    if new_survivors:
        print(f"\nnobody has judged these -- each is a change no test could see:")
        for m in sorted(new_survivors, key=lambda x: (x['target'], x['routine'], x['lineno'])):
            print(f"  {m['path']}:{m['lineno']}  {m['routine']}  "
                  f"{m['was']} -> {m['now']}")
            print(f"      {m['line'][:104]}")
            print(f"      {m['id']}")

    return 1 if new_survivors else 0

sys.exit(main())

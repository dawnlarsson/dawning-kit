#!/usr/bin/env python3
"""Build a pinned source atlas. Static evidence is never a correctness proof."""
import argparse
import csv
import hashlib
import html
import itertools
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'kit'))
import function_audit as audit
sys.path.insert(0, str(ROOT / 'kit/compact'))
import inventory as lexer

ROLES = {'entrypoint', 'orchestration', 'parsing', 'formatting', 'algorithm',
         'storage_lifetime', 'lookup_collection', 'io_protocol', 'ui_geometry',
         'platform_abi', 'adapter', 'verification', 'predicate', 'arithmetic',
         'error_reporting', 'state_transition'}
BASES = {'body_reviewed', 'family_rule', 'alias_or_generator'}
KEYWORDS = set('auto break case char const continue default do double else enum '
               'extern float for goto if inline int long register restrict '
               'return short signed sizeof static struct switch typedef union '
               'unsigned void volatile while _Atomic _Bool _Generic _Noreturn '
               'address_to address_of positive bipolar p8 p16 p32 p64 b8 b16 '
               'b32 b64 bool fn null true false end'.split())


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True).strip()


def area(path):
    if path.startswith('test/'):
        return 'Tests and benchmarks'
    if path.startswith('kit/'):
        return 'Development tooling'
    if path == 'src/library.c':
        return 'Assembly library'
    if path.startswith('src/platform/'):
        return 'Platform and ABI'
    if path.startswith('src/standard/'):
        return 'C standard library'
    if path in ('src/library.common.c', 'src/compiler_memory.c'):
        return 'Common C helpers'
    if path.startswith('src/canvas/'):
        return 'Canvas'
    if path in ('src/core.c', 'src/spark.c') or path.startswith('kernel/'):
        return 'Kernel and Spark'
    if path.startswith('src/net/'):
        return 'Network protocols'
    if path.startswith('src/bowl/'):
        return 'Bowl runtime'
    if path.startswith('programs/'):
        return 'Program entries'
    if path.startswith('src/sh/'):
        stem = Path(path).stem
        if stem in {'text', 'file', 'tools', 'util_linux', 'process_tools',
                    'awk', 'cksum', 'checksum', 'regex_graph'} or stem.startswith('storage_'):
            return 'Utility implementations'
        if stem in {'term', 'pty', 'screen', 'edit', 'monitor', 'net', 'system'}:
            return 'Terminal, editor and system tools'
        return 'Shell language and runtime'
    return 'Other source'


def load_source(path):
    text = (ROOT / path).read_text()
    tokens, directives = lexer.lex(text)
    lines = text.splitlines()
    code = set()
    for token in tokens:
        code.update(range(token.line, token.line + token.value.count('\n') + 1))
    for directive in directives:
        code.update(range(directive.line,
                          directive.line + directive.text.count('\n') + 1))
    if path.endswith('.asm'):
        code = {i for i, line in enumerate(lines, 1)
                if line.strip() and not line.lstrip().startswith(('#', '//'))}
    return {'text': text, 'lines': lines, 'tokens': tokens,
            'directives': directives, 'code': code,
            'token_lines': [t.line for t in tokens],
            'sha256': hashlib.sha256(text.encode()).hexdigest()}


def extents(source, definitions):
    """Use sealed declarator locations; balance bodies without preprocessing."""
    tokens = source['tokens']
    found = {}
    by_location = {(d.name, d.line): d for d in definitions if d.kind == 'body'}
    header = []
    depth = 0
    current = None
    for index, token in enumerate(tokens):
        if token.value == '{':
            if depth == 0:
                name = lexer.function_header(header)
                if name is not None and (name.value, name.line) in by_location:
                    limit = next(i for i, x in enumerate(header) if x is name)
                    cut = 0
                    i = 0
                    while i < limit - 1:
                        if (header[i].kind == 'identifier' and
                            (header[i].value.isupper() or
                             header[i].value == 'var_list_entry') and
                            header[i + 1].value == '('):
                            _, close = audit.macro_arguments(header, i + 1)
                            if close < limit and close > i + 1:
                                cut = close + 1
                                i = close
                        i += 1
                    current = (name.value, name.line, header[cut:], index)
            depth += 1
        elif token.value == '}':
            depth -= 1
            if depth == 0:
                if current:
                    name, line, signature, begin = current
                    found[(name, line)] = (signature, tokens[begin:index + 1])
                current = None
                header = []
        elif depth == 0:
            if token.value == ';':
                header = []
            else:
                header.append(token)
    # K&R definitions can have argument declarations between ')' and '{'.
    for key in by_location.keys() - found.keys():
        name, line = key
        start = next(i for i, t in enumerate(tokens)
                     if t.value == name and t.line == line and
                     i + 1 < len(tokens) and tokens[i + 1].value == '(')
        _, close = audit.macro_arguments(tokens, start + 1)
        opening = next(i for i in range(close + 1, len(tokens))
                       if tokens[i].value == '{')
        depth = 1
        closing = opening + 1
        while depth:
            depth += (tokens[closing].value == '{') - (tokens[closing].value == '}')
            closing += 1
        left = start
        while left and tokens[left - 1].value not in (';', '}', '{'):
            left -= 1
        found[key] = (tokens[left:opening], tokens[opening:closing])
    assert found.keys() == by_location.keys()
    return found


def span_metrics(source, spans):
    covered = set()
    for first, last in spans:
        covered.update(range(first, last + 1))
    covered = {i for i in covered if 0 < i <= len(source['lines'])}
    return {'loc': len(covered),
            'nonblank': sum(bool(source['lines'][i - 1].strip()) for i in covered),
            'code_lines': len(covered & source['code'])}


def features(tokens):
    calls = set()
    indirect = set()
    def member(index):
        return (index > 0 and tokens[index - 1].value in ('.', '->') or
                index > 1 and tokens[index - 2].value == '-' and
                tokens[index - 1].value == '>')
    for i, token in enumerate(tokens[:-1]):
        if (token.kind == 'identifier' and tokens[i + 1].value == '(' and
                token.value not in lexer.NON_NAMES):
            if member(i):
                indirect.add(token.value)
            else:
                calls.add(token.value)
    depth = peak = 0
    for token in tokens:
        depth += (token.value == '{') - (token.value == '}')
        peak = max(peak, depth)
    return {'call_names': sorted(calls), 'indirect_call_names': sorted(indirect),
            'branches': sum(t.value in ('if', 'switch', '?') for t in tokens),
            'loops': sum(t.value in ('for', 'while') for t in tokens),
            'returns': sum(t.value == 'return' for t in tokens),
            'max_brace_depth': peak,
            '_references': {t.value for i, t in enumerate(tokens)
                            if t.kind == 'identifier' and not member(i)}}


def c_rows(path, source, definitions):
    bodies = extents(source, definitions)
    rows = []
    for definition in definitions:
        line, name, kind = definition.line, definition.name, definition.kind
        body = []
        if kind == 'body':
            signature, body = bodies[(name, line)]
            first = signature[0].line if signature else line
            last = body[-1].line
            snippet = ' '.join(t.value for t in signature)
            measure_tokens = signature + body
        else:
            if kind == 'generated':
                wanted = {macro: templates for (file, macro), templates in
                          audit.GENERATORS.items() if file == path}
                for start, token in enumerate(source['tokens']):
                    if token.line == line and token.value in wanted:
                        arguments, stop = audit.macro_arguments(source['tokens'], start + 1)
                        if arguments and name in {template.format(arguments[0][0].value)
                                                  for template in wanted[token.value]}:
                            break
                else:
                    raise ValueError(f'Cannot locate generator for {path}:{line}:{name}')
            else:
                name_at = next(i for i, token in enumerate(source['tokens'][:-1])
                               if token.line == line and token.value == name and
                               source['tokens'][i + 1].value == '(')
                start = name_at
                while start and source['tokens'][start - 1].value not in (';', '}', '{'):
                    start -= 1
                stop = name_at
                while stop < len(source['tokens']) - 1 and source['tokens'][stop].value != ';':
                    stop += 1
            measure_tokens = source['tokens'][start:stop + 1]
            first = measure_tokens[0].line
            last = measure_tokens[-1].line if measure_tokens else line
            snippet = source['text'][measure_tokens[0].start:
                                     measure_tokens[-1].start + len(measure_tokens[-1].value)]
        spans = [[first, last]]
        row = {'id': f'{path}:{name}:{line}', 'file': path, 'name': name,
               'line': line, 'start': first, 'end': last, 'kind': kind,
               'spans': spans, 'signature': snippet, 'tokens': len(measure_tokens),
               **span_metrics(source, spans), **features(body)}
        row['body_sha256'] = hashlib.sha256(' '.join(t.value for t in measure_tokens).encode()).hexdigest()
        row['_body_tokens'] = body
        row['_token_positions'] = {t.start for t in measure_tokens}
        if kind == 'alias':
            match = re.search(r'alias\s*\(\s*"([^"]+)"', snippet)
            if match:
                row['alias_of'] = match[1]
        elif kind == 'generated':
            row['generator'] = measure_tokens[0].value if measure_tokens else ''
            row['_references'] = {t.value for t in measure_tokens if t.kind == 'identifier'} - {name}
        rows.append(row)
    return rows


def assembly_rows(sources):
    """Semantic library names with per-architecture source ranges; aliases stay aliases."""
    all_rows = {}
    for path, source in sources.items():
        if path != 'src/library.c' and not path.startswith('src/platform/'):
            continue
        tokens = source['tokens']
        events = [(d.start, 0, d) for d in source['directives']]
        for i, t in enumerate(tokens[:-3]):
            if (t.value in set(lexer.ASM_DECLARERS) | set(lexer.ASM_ENDERS) |
                    set(lexer.ASM_ALIASES)) and tokens[i + 1].value == '(':
                _, end = audit.macro_arguments(tokens, i + 1)
                events.append((t.start, 1, (t.value, tokens[i + 2].value,
                              t.line, tokens[end].line,
                              tokens[i + 4].value if t.value in lexer.ASM_ALIASES else None)))
        current, stack, opened = None, [], {}
        for _, event_kind, event in sorted(events, key=lambda x: (x[0], x[1])):
            if not event_kind:
                current = lexer.arch_transition(current, stack, event)
                continue
            macro, name, first, last, target = event
            if macro in lexer.ASM_ENDERS:
                begin = opened.pop((current, name))
                all_rows[name]['architecture_spans'].append({'arch': current, 'start': begin, 'end': last})
                continue
            row = all_rows.setdefault(name, {'id': f'{path}:{name}:asm',
                'file': path, 'name': name, 'line': first, 'kind': 'assembly',
                'architecture_spans': [], 'call_names': [], 'indirect_call_names': [],
                '_references': set(), '_body_tokens': [], 'signature': name,
                'branches': 0, 'loops': 0, 'returns': 0, 'max_brace_depth': 0})
            if macro in lexer.ASM_ALIASES:
                row['architecture_spans'].append({'arch': current or 'API alias', 'start': first, 'end': last})
                row['alias_of'] = target
                if current is None:
                    row['kind'] = 'assembly_alias'
            else:
                opened[(current, name)] = first
        assert not opened
    for path, source in sources.items():
        if not path.endswith('.asm'):
            continue
        arch, opening = None, None
        for line, text in enumerate(source['lines'], 1):
            match = re.match(r'\s*#>\s*arch\s+(\S+)', text)
            if match:
                arch = match[1]
            match = re.match(r'\s*SYM_FUNC_START\(([^)]+)\)', text)
            if match and arch not in (None, 'other', 'shared'):
                opening = (match[1], line)
            match = re.match(r'\s*SYM_FUNC_END\(([^)]+)\)', text)
            if match and opening:
                name, first = opening
                assert name == match[1]
                key = path + ':' + name
                row = all_rows.setdefault(key, {'id': key + ':asm', 'file': path,
                    'name': name, 'line': first, 'kind': 'assembly',
                    'architecture_spans': [], 'call_names': [], 'indirect_call_names': [],
                    '_references': set(), '_body_tokens': [], 'signature': name,
                    'branches': 0, 'loops': 0, 'returns': 0, 'max_brace_depth': 0})
                row['architecture_spans'].append({'arch': arch, 'start': first, 'end': line})
                opening = None
        assert opening is None
    for row in all_rows.values():
        source = sources[row['file']]
        row['spans'] = [[s['start'], s['end']] for s in row['architecture_spans']]
        row['start'] = min(s[0] for s in row['spans'])
        row['end'] = max(s[1] for s in row['spans'])
        row.update(span_metrics(source, row['spans']))
        row['tokens'] = 0  # C lexer tokens cannot measure assembly instruction size.
    return list(all_rows.values())


def classify(rows, allow_incomplete):
    annotations, families, pins = {}, {}, {}
    for path in sorted((HERE / 'annotations').glob('*.json')):
        data = json.loads(path.read_text())
        if data.get('source_digest') and data['source_digest'] != audit.source_digest():
            raise ValueError(f'Stale source digest in {path.name}; refresh classifications against the changed source before rebuilding')
        for family in data['families']:
            assert family['id'] not in families, family['id']
            families[family['id']] = family
        for row in data['functions']:
            assert row['id'] not in annotations, row['id']
            assert row['role'] in ROLES and row['review_basis'] in BASES, row
            annotations[row['id']] = row
        pins[path.name] = data.get('source_digest')
    needed = {r['id'] for r in rows if r['production']}
    missing, extra = needed - annotations.keys(), annotations.keys() - needed
    if (missing or extra) and not allow_incomplete:
        raise ValueError(f'Annotation coverage: {len(missing)} missing, {len(extra)} stale; examples {sorted(missing)[:4]} {sorted(extra)[:4]}')
    for row in rows:
        if row['id'] in annotations:
            row.update({k: v for k, v in annotations[row['id']].items() if k != 'id'})
            if row.get('declaration', {}).get('signature'):
                row['signature'] = row['declaration']['signature']
        else:
            if not row['production']:
                family = 'support.' + Path(row['file']).stem
                title = Path(row['file']).stem.replace('_', ' ')
                description = f'Test or benchmark code in {row["file"]}; ordinary source definitions only.'
                role, basis = 'verification', 'family_rule'
            else:
                family, title = 'unclassified', 'Needs classification'
                description = 'No current annotation supplied.'
                role, basis = 'orchestration', 'family_rule'
            families.setdefault(family, {'id': family, 'title': title,
                'description': description, 'design_question': 'Inspect callers and contracts before proposing a fold.',
                'constraints': ['Classification is not a behavior or equivalence proof.']})
            row.update(family=family, role=role, responsibility=f'{row["name"]}: {description}',
                       review_basis=basis, confidence='low', contract_notes=[])
        assert row['family'] in families, row
    return list(families.values()), {'missing': sorted(missing), 'stale': sorted(extra), 'annotation_pins': pins}


def connections(rows, sources):
    by_name = defaultdict(list)
    for i, row in enumerate(rows):
        row['index'] = i
        by_name[row['name']].append(i)
        row.update(callees=[], callers=[], references=[], referenced_by=[], unresolved_calls=[])
    for i, row in enumerate(rows):
        for name in row['call_names'] + ([row['alias_of']] if row.get('alias_of') else []):
            candidates = by_name.get(name, [])
            local = [j for j in candidates if rows[j]['file'] == row['file']]
            same_scope = [j for j in candidates if rows[j]['production'] == row['production']]
            candidates = local or same_scope or candidates
            if not candidates:
                row['unresolved_calls'].append(name)
            for j in candidates:
                row['callees'].append(j)
                rows[j]['callers'].append(i)
        for name in row['_references'] - set(row['call_names']) - {row['name']}:
            candidates = by_name.get(name, [])
            local = [j for j in candidates if rows[j]['file'] == row['file']]
            for j in local or [j for j in candidates if rows[j]['production']]:
                row['references'].append(j)
                rows[j]['referenced_by'].append(i)
    for row in rows:
        for key in ('callees', 'callers', 'references', 'referenced_by'):
            row[key] = sorted(set(row[key]))
        row['caller_count'] = len(row['callers'])
        row['production_caller_count'] = sum(rows[j]['production'] for j in row['callers'])
        row['callee_count'] = len(row['callees'])
        row['library_callees'] = [j for j in row['callees'] if rows[j]['file'] in
            {'src/library.c', 'src/library.common.c', 'src/compiler_memory.c'} or
            (rows[j]['kind'].startswith('assembly') and rows[j]['file'].startswith('src/platform/'))]
        row['shared_helper_count'] = len(row['library_callees'])
    # References in dispatch tables/global initializers are separate from calls.
    occupied = defaultdict(set)
    for row in rows:
        if row['kind'] == 'body':
            occupied[row['file']].update(row.get('_token_positions', set()))
    external = defaultdict(set)
    for path, source in sources.items():
        for token in source['tokens']:
            if token.start not in occupied[path] and token.value in by_name:
                external[token.value].add(f'{path}:{token.line}')
    for row in rows:
        row['outside_body_references'] = sorted(external[row['name']])


def similarities(rows):
    """Conservative whole-body lead scan; no semantic equivalence claim."""
    shapes, grams, exact, inverted = {}, {}, defaultdict(list), defaultdict(list)
    for row in rows:
        if not row['production'] or row['kind'] != 'body' or row['tokens'] < 60:
            continue
        names, shape = {}, []
        for token in row['_body_tokens']:
            value = token.value
            if token.kind == 'identifier' and value not in KEYWORDS:
                value = names.setdefault(value, 'ID' + str(len(names)))
            shape.append(value)
        i = row['index']
        shapes[i] = shape
        exact[tuple(shape)].append(i)
        if len(shape) >= 100:
            blind = ['ID' if re.fullmatch(r'ID\d+', value) else value for value in shape]
            grams[i] = {tuple(blind[j:j + 7]) for j in range(len(blind) - 6)}
            for gram in grams[i]:
                inverted[gram].append(i)
    candidate_counts = Counter()
    for group in inverted.values():
        if 1 < len(group) <= 80:
            candidate_counts.update(itertools.combinations(group, 2))
    pairs = []
    for (a, b), overlap in candidate_counts.items():
        if overlap < 12 or min(len(shapes[a]), len(shapes[b])) / max(len(shapes[a]), len(shapes[b])) < .65:
            continue
        score = len(grams[a] & grams[b]) / len(grams[a] | grams[b])
        if score >= .65:
            pairs.append({'left': a, 'right': b, 'similarity': round(score, 4),
                          'combined_loc': rows[a]['loc'] + rows[b]['loc'],
                          'cross_family': rows[a]['family'] != rows[b]['family']})
    groups = [{'functions': group, 'loc': sum(rows[i]['loc'] for i in group)}
              for group in exact.values() if len(group) > 1]
    return {'exact_renamed_groups': sorted(groups, key=lambda g: -g['loc']),
            'near_pairs': sorted(pairs, key=lambda p: (-p['combined_loc'], -p['similarity'])),
            'method': 'Whole ordinary production C bodies; exact groups use consistent identifier renaming and >=60 source tokens. Near pairs use blind identifier renaming, >=100 body tokens, length ratio >=0.65 and seven-token shingle Jaccard >=0.65; candidate generation requires 12 shared shingles occurring in <=80 functions. Keywords and literals remain. Function/type/field names are renamed, so this produces leads, not safe folds. No NiCad run, no block matching, no macro expansion, no assembly comparison; scores are not savings.'}


def aggregate(rows, files, families, sources):
    owned = defaultdict(set)
    owned_tokens = defaultdict(set)
    for row in rows:
        own = set()
        for first, last in row['spans']:
            own.update(range(first, last + 1))
        available = own - owned[row['file']]
        row['attributed_loc'] = len(available)
        row['attributed_code_lines'] = len(available & sources[row['file']]['code'])
        positions = row.get('_token_positions', set())
        row['attributed_tokens'] = len(positions - owned_tokens[row['file']])
        owned_tokens[row['file']].update(positions)
        owned[row['file']].update(own)
    for file in files:
        file['function_attributed_loc'] = len(owned[file['file']])
        file['outside_function_loc'] = file['physical_loc'] - file['function_attributed_loc']
        outside = set(range(1, file['physical_loc'] + 1)) - owned[file['file']]
        file['outside_function_code_lines'] = len(outside & sources[file['file']]['code'])
        file['outside_function_blank_lines'] = sum(not sources[file['file']]['lines'][i-1].strip() for i in outside)
        file['outside_function_comment_lines'] = file['outside_function_loc'] - file['outside_function_code_lines'] - file['outside_function_blank_lines']
        assert file['outside_function_loc'] >= 0, file
    by_family = defaultdict(list)
    for row in rows:
        by_family[row['family']].append(row)
    for family in families:
        members = by_family[family['id']]
        family.update(functions=[r['index'] for r in members],
            attributed_loc=sum(r['attributed_loc'] for r in members),
            tokens=sum(r['attributed_tokens'] for r in members),
            body_reviewed=sum(r['review_basis'] == 'body_reviewed' for r in members),
            files=sorted({r['file'] for r in members}),
            production=any(r['production'] for r in members))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'artifacts/function-map-2026-09-08')
    parser.add_argument('--allow-incomplete', action='store_true')
    parser.add_argument('--no-similarity', action='store_true')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    production = {p.relative_to(ROOT).as_posix() for p in audit.production_sources()}
    production |= {p.relative_to(ROOT).as_posix() for p in audit.audited_sources() if p.suffix == '.asm'}
    paths = set(p for p in git('ls-files').splitlines() if Path(p).suffix in {'.c', '.h', '.inc', '.asm'}) | production
    definitions = audit.inventory()
    by_file = defaultdict(list)
    for definition in definitions:
        by_file[definition.path].append(definition)
    sources, files, rows = {}, [], []
    for path in sorted(paths):
        if path.startswith('.claude/') or not (ROOT / path).is_file():
            continue
        source = load_source(path)
        sources[path] = source
        if path not in production and not path.endswith('.asm'):
            by_file[path] = audit.ordinary_definitions(ROOT / path) + audit.alias_definitions(ROOT / path)
        rows.extend(c_rows(path, source, by_file[path]))
        files.append({'file': path, 'area': area(path), 'production': path in production,
            'physical_loc': len(source['lines']),
            'nonblank_loc': sum(bool(x.strip()) for x in source['lines']),
            'code_bearing_loc': len({i for i in source['code'] if i <= len(source['lines']) and source['lines'][i-1].strip()}),
            'tokens': 0 if path.endswith('.asm') else len(source['tokens']),
            'sha256': source['sha256']})
    rows.extend(assembly_rows({p: s for p, s in sources.items() if p in production}))
    rows.sort(key=lambda r: (r['file'], r['line'], r['name']))
    for row in rows:
        row.update(production=row['file'] in production, area=area(row['file']))
    families, coverage = classify(rows, args.allow_incomplete)
    connections(rows, sources)
    aggregate(rows, files, families, sources)
    clones = similarities(rows) if not args.no_similarity else {}
    prod = [r for r in rows if r['production']]
    expected = {f'{d.path}:{d.name}:{d.line}' for d in definitions}
    actual = {r['id'] for r in prod if r['kind'] in {'body', 'generated', 'alias'}}
    assert expected == actual and len(actual) == len(definitions)
    assert len({r['id'] for r in rows}) == len(rows)
    library_names, _, library_aliases = audit.library_routines()
    floor = [r for r in prod if r['kind'].startswith('assembly') and
             (r['file'] == 'src/library.c' or r['file'].startswith('src/platform/'))]
    required_floor = set(library_names) | set(library_aliases)
    assert required_floor <= {r['name'] for r in floor}
    coverage['additional_platform_symbols'] = sorted({r['name'] for r in floor} - required_floor)
    summary = {'production_entries': len(prod), 'production_c_entries': len(actual),
        'production_files': len(production), 'support_entries': len(rows) - len(prod),
        'production_physical_loc': sum(f['physical_loc'] for f in files if f['production']),
        'production_function_attributed_loc': sum(r['attributed_loc'] for r in prod),
        'production_body_reviewed_loc': sum(r['attributed_loc'] for r in prod if r['review_basis'] == 'body_reviewed'),
        'production_body_reviewed_tokens': sum(r['attributed_tokens'] for r in prod if r['review_basis'] == 'body_reviewed'),
        'production_review_basis': dict(Counter(r['review_basis'] for r in prod)),
        'production_kind': dict(Counter(r['kind'] for r in prod)),
        'production_families': sum(f['production'] for f in families)}
    for row in rows:
        row.pop('_references', None)
        row.pop('_body_tokens', None)
        row.pop('_token_positions', None)
    result = {'schema_version': 1, 'commit': git('rev-parse', 'HEAD'),
        'source_digest': audit.source_digest(), 'summary': summary,
        'coverage': coverage, 'files': files, 'families': families, 'functions': rows,
        'similarity': clones,
        'methodology': {
            'scope': 'Complete sealed production C inventory plus semantic library/Canvas/kernel assembly symbols and additional platform signal/setjmp symbols outside the sealed library inclusion graph. Support layer contains tracked ordinary C bodies and aliases, not expanded test-generating macros.',
            'classification': 'Curated semantic families and explicit review basis per function. Rule-derived is not manual body review. No label establishes correctness, necessity or optimality.',
            'size': 'Raw source, all configuration variants. Function spans include comments/blanks. Attributed LOC assigns each source line once, including shared generator invocation lines once. Macro templates and other source outside symbol spans remain a visible residual. C lexer tokens exclude directives; assembly token count deliberately omitted.',
            'connections': 'Possible named call edges from raw lexical syntax, preferring same-file definitions; configuration alternatives may yield multiple targets. Function-pointer members are listed separately. Identifier references include potential callbacks; outside-body references include declarations/tables. No preprocessor, type resolution, dataflow, compiled reachability or dead-code proof.',
            'shared_helpers': 'Possible calls resolving to library.c, library.common.c, compiler_memory.c or platform assembly symbols (including the ABI/syscall floor). Absence is not evidence that a function ought to use them.',
            'coverage': 'Production ID equality against kit/function_audit.py is asserted. Test/benchmark generator expansions are outside the support-layer inventory.'}}
    encoded = json.dumps(result, separators=(',', ':'), ensure_ascii=False)
    (args.out / 'functions.json').write_text(encoded + '\n')
    (args.out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    with (args.out / 'functions.csv').open('w', newline='') as output:
        fields = ['id','file','name','line','end','kind','production','area','family','role',
                  'responsibility','review_basis','confidence','loc','attributed_loc','code_lines',
                  'tokens','attributed_tokens','branches','loops','caller_count',
                  'production_caller_count','callee_count','shared_helper_count']
        writer = csv.DictWriter(output, fields, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(rows)
    template = HERE / 'atlas.html'
    if template.exists():
        rendered_html = template.read_text().replace('__ATLAS_DATA__', encoded.replace('<', '\\u003c'))
        (args.out / 'index.html').write_text(rendered_html)
    source_style = 'body{margin:20px;font:13px/1.5 ui-monospace,monospace;color-scheme:light dark}h1{font:18px system-ui}a{color:inherit}.line{white-space:pre-wrap;overflow-wrap:anywhere;display:block}.line:target{background:#fcce6266}.n{display:inline-block;width:5em;user-select:none;color:#888;text-decoration:none}header{position:sticky;top:0;background:Canvas;padding:8px 0}'
    for path, source in sources.items():
        destination = args.out / 'sources' / (path + '.html')
        destination.parent.mkdir(parents=True, exist_ok=True)
        rendered = '\n'.join(f'<span class="line" id="L{i}"><a class="n" href="#L{i}">{i}</a>{html.escape(line)}</span>'
                             for i, line in enumerate(source['lines'], 1))
        destination.write_text('<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
                              f'<title>{html.escape(path)}</title><style>{source_style}</style><header><h1>{html.escape(path)}</h1>'
                              f'<small>Source snapshot {source["sha256"]}</small></header><pre>{rendered}</pre>')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()

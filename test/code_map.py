#!/usr/bin/env python3
"""Source-atlas regression fixtures; no compiler or production mutation needed.

Run with: python3 test/code_map.py -v
The examples assert source facts, rather than a second implementation of the
atlas parser. These checks do not turn lexical edges into semantic call edges.
"""
import contextlib
import copy
import csv
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import textwrap
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    'code_map_build', Path(__file__).resolve().parents[1] / 'kit/code_map/build.py')
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)


class AtlasFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.here = self.root / 'kit/code_map'
        (self.here / 'annotations').mkdir(parents=True)
        for owner, attribute, value in (
                (build, 'ROOT', self.root), (build.audit, 'ROOT', self.root),
                (build, 'HERE', self.here)):
            replacement = patch.object(owner, attribute, value)
            replacement.start()
            self.addCleanup(replacement.stop)

    def source(self, text, path='src/fixture.c'):
        destination = self.root / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(textwrap.dedent(text).lstrip('\n'))
        return build.load_source(path)

    def rows(self, text, path='src/fixture.c', definitions=None):
        source = self.source(text, path)
        if definitions is None:
            definitions = (build.audit.ordinary_definitions(self.root / path) +
                           build.audit.alias_definitions(self.root / path))
        else:
            definitions = [build.audit.Definition(path, line, name, kind)
                           for name, line, kind in definitions]
        rows = build.c_rows(path, source, definitions)
        for index, row in enumerate(rows):
            row.update(index=index, production=path.startswith('src/'),
                       family='fixture.engine', review_basis='body_reviewed')
        return source, rows

    def family(self):
        return {'id': 'fixture.engine', 'title': 'Fixture engine',
                'description': 'Small isolated source fixtures.',
                'design_question': 'Are source facts represented faithfully?',
                'constraints': ['No semantic-equivalence claim.']}

    def annotations(self, rows, name='fixture.json'):
        functions = [{
            'id': row['id'], 'family': 'fixture.engine', 'role': 'algorithm',
            'responsibility': 'Execute the fixture operation.',
            'review_basis': 'body_reviewed', 'confidence': 'high',
            'contract_notes': ['Fixture only.']}
            for row in rows]
        data = {'schema_version': 1, 'scope': sorted({r['file'] for r in rows}),
                'families': [self.family()], 'functions': functions}
        path = self.here / 'annotations' / name
        path.write_text(json.dumps(data))
        return path, data

    def aggregate(self, source, rows, path='src/fixture.c'):
        files = [{'file': path, 'physical_loc': len(source['lines'])}]
        families = [self.family()]
        build.aggregate(rows, files, families, {path: source})
        return files[0], families[0]


class SourceBoundaryTests(AtlasFixture):
    def test_braceless_loops_comments_and_literals_preserve_body_boundary(self):
        source, rows = self.rows('''
            static const char *text = "not_a_body() { while (;;) }";
            static int first(int n)
            {
                /* fake() { return; } */
                while (n > 2) n--;
                for (; n > 1; --n) consume(n);
                return n;
            }
            int second(void) { return 7; }
        ''')
        self.assertEqual([(r['name'], r['start'], r['end']) for r in rows],
                         [('first', 2, 8), ('second', 9, 9)])
        self.assertEqual(rows[0]['loops'], 2)
        self.assertEqual(rows[0]['call_names'], ['consume'])
        self.assertNotIn(4, source['code'])
        self.assertEqual(rows[0]['max_brace_depth'], 1)

    def test_loop_count_counts_do_while_once_including_nested_loops(self):
        _, rows = self.rows('''
            int count(int n) {
                do { do n--; while (n > 4); } while (n > 2);
                while (n > 1) n--;
                for (; n; --n) visit(n);
                return n;
            }
        ''')
        self.assertEqual(rows[0]['loops'], 4)

    def test_knr_header_retains_multiline_storage_and_return_type(self):
        _, rows = self.rows('''
            static
            int
            old(a, b)
            int a;
            char *b;
            {
                return a + b[0];
            }
        ''')
        self.assertEqual([(r['name'], r['line']) for r in rows], [('old', 3)])
        self.assertEqual(rows[0]['start'], 1)
        self.assertTrue(rows[0]['signature'].startswith('static int old'))
        self.assertEqual(rows[0]['loc'], 8)

    def test_separate_macro_invocation_does_not_join_function_header(self):
        _, rows = self.rows('''
            CONFIGURE(example)
            static int handler(int n)
            {
                return n;
            }
        ''')
        self.assertEqual([(r['name'], r['start'], r['end']) for r in rows],
                         [('handler', 2, 5)])
        self.assertNotIn('CONFIGURE', rows[0]['signature'])

    def test_preprocessor_variants_remain_distinct_with_exact_spans(self):
        source, rows = self.rows('''
            #if FAST
            int choose(void) { return 1; }
            #else
            int choose(void) { return 2; }
            #endif
        ''')
        self.assertEqual([(r['name'], r['line'], r['loc']) for r in rows],
                         [('choose', 2, 1), ('choose', 4, 1)])
        file, _ = self.aggregate(source, rows)
        self.assertEqual(file['function_attributed_loc'], 2)
        self.assertEqual(file['outside_function_loc'], 3)

    def test_multiline_directive_and_comments_have_distinct_code_accounting(self):
        source, rows = self.rows('''
            #define VALUE(x) \\
                ((x) + 1)

            /* retained explanation */
            int read_value(void)
            {
                // a body comment
                return VALUE(2);
            }
        ''')
        self.assertEqual(source['code'], {1, 2, 5, 6, 8, 9})
        self.assertEqual(rows[0]['loc'], 5)
        self.assertEqual(rows[0]['code_lines'], 4)
        file, _ = self.aggregate(source, rows)
        self.assertEqual(file['outside_function_loc'], 4)


class AttributionTests(AtlasFixture):
    def test_aliases_on_one_line_keep_their_targets_and_tokens(self):
        source, rows = self.rows(
            'int first(void) __attribute__((alias("base_one"))); '
            'int second(void) __attribute__((alias("base_two")));\n')
        self.assertEqual({r['name']: r.get('alias_of') for r in rows},
                         {'first': 'base_one', 'second': 'base_two'})
        self.assertTrue(rows[0]['_token_positions'].isdisjoint(
            rows[1]['_token_positions']))
        file, family = self.aggregate(source, rows)
        self.assertEqual(file['function_attributed_loc'], 1)
        self.assertEqual(sum(r['attributed_loc'] for r in rows), 1)
        self.assertEqual(sum(r['attributed_code_lines'] for r in rows), 1)
        self.assertEqual(family['tokens'], len(source['tokens']))

    def test_multiline_alias_includes_full_declarator(self):
        _, rows = self.rows('''
            extern
            int
            public_name(void)
                __attribute__((alias("private_name")));
        ''')
        self.assertEqual(rows[0]['alias_of'], 'private_name')
        self.assertEqual((rows[0]['start'], rows[0]['end']), (1, 4))
        self.assertEqual(rows[0]['tokens'], 16)

    def test_shared_generator_invocation_is_charged_once(self):
        with patch.dict(build.audit.GENERATORS, {
                ('src/fixture.c', 'MAKE_PAIR'): ('{}_read', '{}_write')}):
            source, rows = self.rows('''
                #define MAKE_PAIR(name) /* expansion intentionally outside symbols */
                MAKE_PAIR(
                    thing)
            ''', definitions=[('thing_read', 2, 'generated'),
                              ('thing_write', 2, 'generated')])
        self.assertEqual(rows[0]['_token_positions'], rows[1]['_token_positions'])
        file, family = self.aggregate(source, rows)
        self.assertEqual([r['attributed_loc'] for r in rows], [2, 0])
        self.assertEqual(file['outside_function_loc'], 1)
        self.assertEqual(family['tokens'], 4)

    def test_distinct_same_line_generators_have_distinct_token_ownership(self):
        with patch.dict(build.audit.GENERATORS, {
                ('src/fixture.c', 'MAKE'): ('{}',)}):
            source, rows = self.rows(
                'MAKE(first) MAKE(second)\n',
                definitions=[('first', 1, 'generated'), ('second', 1, 'generated')])
        self.assertTrue(rows[0]['_token_positions'].isdisjoint(
            rows[1]['_token_positions']))
        _, family = self.aggregate(source, rows)
        self.assertEqual(family['attributed_loc'], 1)
        self.assertEqual(family['tokens'], 8)
        self.assertEqual(sum(r['attributed_tokens'] for r in rows),
                         len(source['tokens']))

    def test_shared_body_line_deduplicates_loc_but_preserves_both_bodies(self):
        source, rows = self.rows(
            'int first(void) { return 1; } int second(void) { return 2; }\n')
        self.assertEqual(len(rows), 2)
        file, family = self.aggregate(source, rows)
        self.assertEqual([r['loc'] for r in rows], [1, 1])
        self.assertEqual(sum(r['attributed_loc'] for r in rows), 1)
        self.assertEqual(family['tokens'], len(source['tokens']))
        self.assertEqual(file['outside_function_loc'], 0)

    def test_architecture_bodies_and_api_alias_have_disjoint_source_costs(self):
        path = 'src/library.c'
        source = self.source('''
            #if X64
            ASM_FUNC(copy, void, (void))
                "ret"
            ASM_END(copy)
            #elif ARM64
            ASM_FUNC(copy, void, (void))
                "ret"
            ASM_END(copy)
            #endif
            ASM_ALIAS(public_copy, copy)
        ''', path)
        rows = build.assembly_rows({path: source})
        for i, row in enumerate(rows):
            row.update(index=i, family='fixture.engine', production=True,
                       review_basis='alias_or_generator')
        self.assertEqual({r['name'] for r in rows}, {'copy', 'public_copy'})
        routine = next(r for r in rows if r['name'] == 'copy')
        alias = next(r for r in rows if r['name'] == 'public_copy')
        self.assertEqual(routine['spans'], [[2, 4], [6, 8]])
        self.assertEqual(alias['alias_of'], 'copy')
        file, family = self.aggregate(source, rows, path)
        self.assertEqual(file['function_attributed_loc'], 7)
        self.assertEqual(file['outside_function_loc'], 3)
        self.assertEqual(family['tokens'], 0)


class ConnectionTests(AtlasFixture):
    def connected(self, sources):
        all_sources, rows = {}, []
        for path, text in sources.items():
            source, added = self.rows(text, path)
            all_sources[path] = source
            rows.extend(added)
        build.connections(rows, all_sources)
        return rows

    @staticmethod
    def targets(rows, row, key):
        return {(rows[i]['file'], rows[i]['name'], rows[i]['line'])
                for i in row[key]}

    def test_same_file_overrides_cross_file_and_retains_configuration_variants(self):
        rows = self.connected({
            'src/a.c': '#if FAST\nint pick(void) { return 1; }\n#else\n'
                       'int pick(void) { return 2; }\n#endif\n'
                       'int use(void) { return pick(); }\n',
            'src/b.c': 'int pick(void) { return 3; }\n'})
        caller = next(r for r in rows if r['name'] == 'use')
        self.assertEqual(self.targets(rows, caller, 'callees'),
                         {('src/a.c', 'pick', 2), ('src/a.c', 'pick', 4)})

    def test_cross_file_possible_calls_preserve_production_and_support_scope(self):
        rows = self.connected({
            'src/user.c': 'int use(void) { return shared(); }\n',
            'src/a.c': 'int shared(void) { return 1; }\n',
            'src/b.c': 'int shared(void) { return 2; }\n',
            'kit/test.c': 'int shared(void) { return 3; }\n'
                          'int verify(void) { return shared(); }\n'})
        caller = next(r for r in rows if r['name'] == 'use')
        self.assertEqual(self.targets(rows, caller, 'callees'),
                         {('src/a.c', 'shared', 1), ('src/b.c', 'shared', 1)})
        check = next(r for r in rows if r['name'] == 'verify')
        self.assertEqual(self.targets(rows, check, 'callees'),
                         {('kit/test.c', 'shared', 1)})

    def test_member_callbacks_are_indirect_not_global_calls_or_references(self):
        rows = self.connected({'src/fixture.c': '''
            int hook(void) { return 1; }
            int use(struct callbacks *pointer, struct callbacks value) {
                return pointer->hook() + value.hook();
            }
        '''})
        caller = next(r for r in rows if r['name'] == 'use')
        self.assertEqual(caller['indirect_call_names'], ['hook'])
        self.assertEqual(caller['callees'], [])
        self.assertEqual(caller['references'], [])
        self.assertEqual(caller['unresolved_calls'], [])

    def test_member_spelling_does_not_hide_a_separate_callback_reference(self):
        rows = self.connected({'src/fixture.c': '''
            int hook(void) { return 1; }
            int use(struct callbacks *pointer) {
                register_callback(hook);
                return pointer->hook();
            }
        '''})
        caller = next(r for r in rows if r['name'] == 'use')
        self.assertEqual(self.targets(rows, caller, 'references'),
                         {('src/fixture.c', 'hook', 1)})
        self.assertEqual(caller['callees'], [])
        self.assertEqual(caller['unresolved_calls'], ['register_callback'])

    def test_initializer_after_body_on_same_line_is_an_outside_reference(self):
        rows = self.connected({'src/fixture.c':
            'int target(void) { return 1; }\n'
            'int use(void) { return 0; } int (*selected)(void) = target;\n'})
        target = next(r for r in rows if r['name'] == 'target')
        self.assertIn('src/fixture.c:2', target['outside_body_references'])

    def test_alias_edge_resolves_target_without_inventing_an_extra_body(self):
        rows = self.connected({'src/fixture.c':
            'int target(void) { return 1; }\n'
            'int public_name(void) __attribute__((alias("target")));\n'})
        alias = next(r for r in rows if r['name'] == 'public_name')
        self.assertEqual(alias['kind'], 'alias')
        self.assertEqual(self.targets(rows, alias, 'callees'),
                         {('src/fixture.c', 'target', 1)})


class ClassifierAndExportTests(AtlasFixture):
    def test_classifier_requires_exact_production_ids(self):
        _, rows = self.rows('int first(void) { return 1; }\n'
                            'int second(void) { return 2; }\n')
        path, data = self.annotations(rows)
        _, coverage = build.classify(copy.deepcopy(rows), False)
        self.assertEqual(coverage['missing'], [])
        self.assertEqual(coverage['stale'], [])
        data['functions'].pop()
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, '1 missing'):
            build.classify(copy.deepcopy(rows), False)
        data['functions'][0]['id'] = 'src/removed.c:stale:1'
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, '1 stale'):
            build.classify(copy.deepcopy(rows), False)

    def test_classifier_rejects_duplicate_annotations_and_unknown_family(self):
        _, rows = self.rows('int first(void) { return 1; }\n')
        path, data = self.annotations(rows)
        data['functions'].append(copy.deepcopy(data['functions'][0]))
        path.write_text(json.dumps(data))
        with self.assertRaises(AssertionError):
            build.classify(copy.deepcopy(rows), False)
        data['functions'].pop()
        data['functions'][0]['family'] = 'missing.family'
        path.write_text(json.dumps(data))
        with self.assertRaises(AssertionError):
            build.classify(copy.deepcopy(rows), False)

    def test_assembly_symbols_require_annotations_too(self):
        _, rows = self.rows('int first(void) { return 1; }\n')
        rows[0].update(kind='assembly', id='src/library.c:first:asm')
        with self.assertRaisesRegex(ValueError, '1 missing'):
            build.classify(copy.deepcopy(rows), False)
        self.annotations(rows)
        _, coverage = build.classify(rows, False)
        self.assertEqual(coverage['missing'], [])

    def test_supplied_stale_source_digest_cannot_reuse_same_function_ids(self):
        _, rows = self.rows('int first(void) { return 1; }\n')
        path, data = self.annotations(rows)
        data['source_digest'] = 'old-source-digest'
        path.write_text(json.dumps(data))
        with patch.object(build.audit, 'source_digest', return_value='current-source-digest'):
            for allow_incomplete in (False, True):
                with self.subTest(allow_incomplete=allow_incomplete):
                    with self.assertRaisesRegex(ValueError, 'Stale source digest'):
                        build.classify(copy.deepcopy(rows), allow_incomplete)
            data['source_digest'] = 'current-source-digest'
            path.write_text(json.dumps(data))
            _, coverage = build.classify(rows, False)
        self.assertEqual(coverage['annotation_pins'],
                         {'fixture.json': 'current-source-digest'})

    def test_incomplete_mode_exposes_missing_ids_and_keeps_review_basis_honest(self):
        _, rows = self.rows('int first(void) { return 1; }\n')
        _, coverage = build.classify(rows, True)
        self.assertEqual(coverage['missing'], [rows[0]['id']])
        self.assertEqual(rows[0]['family'], 'unclassified')
        self.assertEqual(rows[0]['review_basis'], 'family_rule')
        self.assertEqual(rows[0]['confidence'], 'low')

    def test_export_has_one_csv_header_and_conserved_physical_loc(self):
        source, rows = self.rows('''
            #define CONSTANT 3
            /* a file-level comment */
            int global = 3;
            int first(void) { return CONSTANT; }
            int second(void) { return first(); }
        ''')
        self.annotations(rows)
        definitions = build.audit.ordinary_definitions(self.root / 'src/fixture.c')
        output = self.root / 'out'
        with patch.object(build.audit, 'production_sources', return_value=[self.root / 'src/fixture.c']), \
             patch.object(build.audit, 'audited_sources', return_value=[self.root / 'src/fixture.c']), \
             patch.object(build.audit, 'inventory', return_value=definitions), \
             patch.object(build.audit, 'library_routines', return_value=([], [], [])), \
             patch.object(build.audit, 'source_digest', return_value='fixture-digest'), \
             patch.object(build, 'git', side_effect=lambda *a: 'src/fixture.c' if a == ('ls-files',) else 'fixture-commit'), \
             patch('sys.argv', ['build.py', '--out', str(output), '--no-similarity']), \
             contextlib.redirect_stdout(io.StringIO()):
            build.main()
        atlas = json.loads((output / 'functions.json').read_text())
        with (output / 'functions.csv').open(newline='') as stream:
            exported = list(csv.DictReader(stream))
        self.assertEqual(len(exported), 2)
        self.assertEqual({row['id'] for row in exported}, {r['id'] for r in rows})
        self.assertEqual(atlas['summary']['production_entries'], 2)
        file = atlas['files'][0]
        self.assertEqual(file['physical_loc'],
                         file['function_attributed_loc'] + file['outside_function_loc'])
        self.assertEqual(sum(r['attributed_tokens'] for r in atlas['functions']),
                         len(source['tokens']) - 5)
        self.assertEqual(sum(r['attributed_code_lines'] for r in atlas['functions']), 2)
        self.assertEqual(file['code_bearing_loc'], 4)


class SimilarityTests(AtlasFixture):
    def test_renamed_groups_are_syntactic_leads_and_keep_literals(self):
        def operation(name, call, literal):
            updates = '\n'.join(f'if (value > {n}) value += {call}(value);'
                                for n in range(6))
            return f'int {name}(int value) {{ {updates} return value + {literal}; }}\n'
        _, rows = self.rows(operation('first', 'read', 1) +
                            operation('second', 'destroy', 1) +
                            operation('third', 'read', 2))
        result = build.similarities(rows)
        exact = [{rows[i]['name'] for i in g['functions']}
                 for g in result['exact_renamed_groups']]
        self.assertEqual(exact, [{'first', 'second'}])
        self.assertIn('leads', result['method'])
        self.assertIn('No NiCad run', result['method'])

    def test_small_or_support_bodies_are_outside_similarity_scope(self):
        _, rows = self.rows('int one(void) { return 1; }\n'
                            'int two(void) { return 1; }\n')
        result = build.similarities(rows)
        self.assertEqual(result['exact_renamed_groups'], [])
        for row in rows:
            row.update(production=False, tokens=1000)
        result = build.similarities(rows)
        self.assertEqual(result['exact_renamed_groups'], [])
        self.assertEqual(result['near_pairs'], [])


if __name__ == '__main__':
    unittest.main()

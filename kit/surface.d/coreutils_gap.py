#!/usr/bin/env python3
"""Pin the GNU coreutils applet denominator against the shell dispatch."""

import json
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
LEDGER = pathlib.Path(__file__).with_name('coreutils-9.11.json')
BUILTINS = ROOT / 'src/sh/builtin.c'
TOOLS = ROOT / 'src/sh/tools.inc'


def ordered_set(ledger, name):
    values = ledger.get(name)
    if not isinstance(values, list) or values != sorted(set(values)):
        raise ValueError('%s must be a sorted list without duplicates' % name)
    return set(values)


def table_names(source, name):
    match = re.search(r'\b%s\[\]\s*=\s*\{(.*?)^\};' % re.escape(name),
                      source, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError('cannot find dispatch table %s' % name)
    return set(re.findall(r'^\s*\{"([^"]+)",\s*[A-Za-z_]\w*\},',
                          match.group(1), re.MULTILINE))


def tool_names(source):
    return set(re.findall(
        r'^SHELL_TOOL\((?:GENERAL|UTIL_(?:BIN|SBIN)),\s*([A-Za-z_]\w*),',
        source, re.MULTILINE))


def main():
    try:
        ledger = json.loads(LEDGER.read_text(encoding='utf-8'))
        commands = ordered_set(ledger, 'commands')
        expected = ordered_set(ledger, 'absent')
        if not expected <= commands:
            raise ValueError('absent names must belong to commands')

        builtins = BUILTINS.read_text(encoding='utf-8')
        tools = TOOLS.read_text(encoding='utf-8')
        dispatched = table_names(builtins, 'shell_commands') | tool_names(tools)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print('coreutils gap: %s' % error, file=sys.stderr)
        return 1

    absent = commands - dispatched
    if absent != expected:
        added = sorted(absent - expected)
        filled = sorted(expected - absent)
        if added:
            print('coreutils gap: newly absent: %s' % ' '.join(added),
                  file=sys.stderr)
        if filled:
            print('coreutils gap: now dispatched: %s' % ' '.join(filled),
                  file=sys.stderr)
        return 1

    print('coreutils %s: %d dispatched, %d absent of %d' %
          (ledger['version'], len(commands - absent), len(absent),
           len(commands)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Enable state, listing and cached PATH dispatch in a freshly built shell."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
source = (Path(__file__).resolve().parents[2]/'src/sh/builtin.c').read_text()
registry = source.split('shell_command shell_commands[] = {',1)[1].split('\n};',1)[0]
names = re.findall(r'\{"([^"\n]+)",',registry)
results=[]
with tempfile.TemporaryDirectory(prefix='dawning-enable-regressions-') as tmp:
    executable=Path(tmp)/'echo'
    executable.write_text('#!/bin/sh\nprintf "external:%s\\n" "$*"\n')
    executable.chmod(0o755)
    environment=dict(os.environ,PATH=tmp)
    def check(label,script,wanted,status=0):
        run=subprocess.run([str(binary),'-c',script],env=environment,capture_output=True,text=True,timeout=10)
        result={'name':label,'script':script,'stdout':run.stdout,'stderr':run.stderr,'status':run.returncode,
                'pass':run.stdout==wanted and run.returncode==status}
        results.append(result)
    check('cached literal honors nonliteral disable',
          'switch=enable; for item in first second; do echo "$item"; $switch -n echo; done',
          'first\nexternal:second\n')
    check('child disable remains local','(enable -n echo; echo child); builtin echo parent',
          'external:child\nparent\n')
    check('type and command see external fallback',
          'enable -n echo; type -t echo; command -v echo; command echo fallback; builtin echo hidden; printf "%s\\n" "$?"',
          f'file\n{executable}\nexternal:fallback\n1\n')
    check('invalid name does not prevent later operands',
          'enable -n missing_name echo; printf "%s\\n" "$?"; echo fallback; enable echo; echo restored',
          '1\nexternal:fallback\nrestored\n')
    check('aliases share handlers independently','enable -n complete; bind; printf "%s\\n" "$?"; true; printf "%s\\n" "$?"; compopt; printf "%s\\n" "$?"',
          '0\n0\n1\n')
    for option in ('','-p','-n','-a','-np','-ap'):
        off='n' in option
        every='a' in option
        expected=''.join(f'enable {"-n " if name=="echo" else ""}{name}\n' for name in names
                         if every or ((name=='echo')==off))
        check(f'ordered listing {option or "default"}',f'enable -n echo; enable {option}',expected)
report={'binary':str(binary),'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
        'cases':len(results),'passed':sum(result['pass'] for result in results),'results':results}
print(json.dumps(report,indent=2))
assert report['passed']==report['cases']

#!/usr/bin/env python3
# Run from any directory: python3 test/audit_text_sed.py [output-directory].
# With no output directory, generated sources, binaries and reports are temporary.
# Uses current production source and hosted adapters; no full applet is executed.
"""Run current sed commit/return code with injected I/O and owned real files.

This is a hosted extraction, not the full applet parser or input engine.  The
filesystem adapter maps production raw-error calls to libc rename/link/unlink.
"""
import hashlib
import json
import os
import pathlib
import platform
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
_owned_output = None if len(sys.argv) > 1 else tempfile.TemporaryDirectory(prefix='audit-text-sed-')
OUT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(_owned_output.name)
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'src/sh/text.c').read_text()
spans = []
def capture(start, stop, offset=0):
    a = source.index(start, offset)
    b = source.index(stop, a)
    block = source[a:b]
    spans.append({'start':source[:a].count('\n')+1,'end':source[:b].count('\n')+1,
                  'sha256':hashlib.sha256(block.encode()).hexdigest()})
    return block
helper = capture('static bool sed_commit(', 'static b32 text_sed()')
done = capture('static b32 text_done(b32 code)', '/*\n        A complaint')
tail = capture('                if (written >= 0)\n', '\n}\n\n/*\n        sort', source.index('static b32 text_sed()'))
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
typedef int b32;
typedef int64_t bipolar;
typedef uint64_t positive;
typedef char p8;
typedef char *string_address;
#define TEXT_PATH_MAX 4096
#define AT_FDCWD -100
#define null NULL
static b32 text_status, sed_file_count;
static bool text_out_failed, sed_io_failed, sed_space_full;
static string_address sed_failed;
static struct { bool failed; } text_input;
static const char *text_name = "sed";
static string_address sed_in_place;
static struct { bipolar handle; } sed_files[1];
static int backup_fail, replace_fail, restore_fail, close_fail, flush_fail;
static int backup_calls, replace_calls, restore_calls, remove_calls, diagnostics;
static int real_files, race, failures_at, current, input_count, requested_exit;
static void text_flush(void) { if (flush_fail) text_out_failed = true; }
static void text_out_to(positive handle) { (void)handle; text_flush(); }
static bipolar system_close(bipolar fd) { (void)fd; return close_fail ? -EIO : 0; }
static bipolar system_remove_at(int dir, const char *path, int flags) {
    (void)dir; (void)flags; remove_calls++;
    return real_files && unlink(path) ? -errno : 0;
}
static bipolar system_rename_at(int a, const char *from, int b, const char *to, int f) {
    (void)a; (void)b; (void)f;
    bool replacement=!strncmp(from,"temporary",9);
    if (replacement) replace_calls++; else backup_calls++;
    if (current == failures_at && (replacement ? replace_fail : backup_fail)) return -EACCES;
    return real_files && rename(from,to) ? -errno : 0;
}
static bipolar system_link_at(int a, const char *from, int b, const char *to, int f) {
    (void)a; (void)b; (void)f; restore_calls++;
    if (race && real_files) {
        FILE *stream=fopen(to,"wx"); if (!stream) exit(65);
        fputs("concurrent\n",stream); fclose(stream);
    }
    if (restore_fail) return -EACCES;
    return real_files && link(from,to) ? -errno : 0;
}
static const char *file_reason(bipolar code) { return strerror((int)-code); }
static void text_error(const char *name, const char *reason) {
    (void)name; if (!reason || !*reason) abort(); diagnostics++;
}
static b32 text_refuse(const char *name, const char *reason, b32 code) {
    text_error(name, reason); return code;
}
static positive string_length(const char *s) { return strlen(s); }
static bool string_equals(const char *a, const char *b) { return !strcmp(a,b); }
static void memory_copy(void *to, const void *from, positive n) { memcpy(to,from,n); }
static void memory_copy_apart_end(char *to, const char *from, positive n) {
    memcpy(to,from,n); to[n] = '\0';
}
'''
entry = r'''
static b32 extracted_commit(void) {
    b32 leaving = requested_exit;
    for (b32 i = 0; i < input_count; i++) {
        current=i;
        p8 name[32], temporary[32];
        snprintf(name,sizeof name,"file%d",i);
        snprintf(temporary,sizeof temporary,"temporary%d",i);
        bipolar written = 9;
'''
runner = r'''
}
int main(int argc, char **argv) {
    if (argc!=18) return 64;
    real_files=atoi(argv[1]); sed_in_place=argv[2];
    backup_fail=atoi(argv[3]); replace_fail=atoi(argv[4]); restore_fail=atoi(argv[5]);
    close_fail=atoi(argv[6]); flush_fail=atoi(argv[7]); text_input.failed=atoi(argv[8]);
    sed_space_full=atoi(argv[9]); sed_io_failed=atoi(argv[11]);
    sed_failed=atoi(argv[10]) ? "no previous regular expression" : NULL;
    requested_exit=atoi(argv[12]); input_count=atoi(argv[13]); text_status=atoi(argv[14]);
    race=atoi(argv[15]); failures_at=atoi(argv[16]);
    if (atoi(argv[17])) {
        positive n=(positive)atoi(argv[17]);
        sed_in_place=malloc(n+1); memset(sed_in_place,'x',n); sed_in_place[n]='\0';
    }
    int status=extracted_commit();
    printf("{\"status\":%d,\"backup_calls\":%d,\"replacement_calls\":%d,"
           "\"restore_calls\":%d,\"remove_calls\":%d,\"diagnostics\":%d}\n",
           status,backup_calls,replace_calls,restore_calls,remove_calls,diagnostics);
    if (atoi(argv[17])) free(sed_in_place);
    return 0;
}
'''
code=prefix+done+helper+entry+tail+runner
c_path=OUT/'C03-regression.c'
exe=OUT/'C03-regression'
c_path.write_text(code)
command=['cc','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(c_path),'-o',str(exe)]
build=subprocess.run(command,text=True,capture_output=True)
(OUT/'C03-build.log').write_text(build.stdout+build.stderr)
assert build.returncode==0,build.stderr
results=[]
def run(label, *, real=False, suffix='.bak', backup=False, replace=False, restore=False,
        close=False, write=False, read=False, space=False, regex=False, io=False,
        leaving=-1, inputs=1, prior=0, race=False, failure_at=0, suffix_length=0,
        backup_directory=False, symlink=False, expected=(0,1,1,0,0,0)):
    args=[int(real),suffix,int(backup),int(replace),int(restore),int(close),int(write),
          int(read),int(space),int(regex),int(io),leaving,inputs,prior,int(race),failure_at,suffix_length]
    with tempfile.TemporaryDirectory(prefix='C03-owned-') as directory:
        base=pathlib.Path(directory)
        if real:
            for i in range(inputs):
                (base/f'file{i}').write_bytes(b'old\n')
                (base/f'temporary{i}').write_bytes(b'new\n')
            if backup_directory: (base/'file0.bak').mkdir()
            if symlink: (base/'link0').symlink_to('file0')
        ran=subprocess.run([str(exe),*map(str,args)],cwd=directory,capture_output=True,text=True)
        assert ran.returncode==0 and not ran.stderr,(label,ran.returncode,ran.stderr)
        result=json.loads(ran.stdout)
        got=tuple(result[k] for k in ('status','backup_calls','replacement_calls','restore_calls','remove_calls','diagnostics'))
        assert got==expected,(label,got,expected)
        files={}
        if real:
            for p in sorted(base.iterdir()):
                files[p.name]={'symlink':os.readlink(p)} if p.is_symlink() else {'directory':True} if p.is_dir() else {'hex':p.read_bytes().hex()}
            for i in range(inputs):
                if i<=failure_at or not (backup or replace):
                    assert f'temporary{i}' not in files,(label,files)
            if expected[0]==0:
                assert files['file0']['hex']==b'new\n'.hex()
                if suffix: assert files['file0.bak']['hex']==b'old\n'.hex()
            elif backup or backup_directory or not suffix:
                assert files['file0']['hex']==b'old\n'.hex(),(label,files)
            elif race:
                assert files['file0']['hex']==b'concurrent\n'.hex()
                assert files['file0.bak']['hex']==b'old\n'.hex()
            elif restore:
                assert 'file0' not in files
                assert files['file0.bak']['hex']==b'old\n'.hex()
            else:
                assert files['file0']['hex']==b'old\n'.hex()
                assert files['file0.bak']['hex']==b'old\n'.hex()
            if symlink: assert files['link0']=={'symlink':'file0'}
        result.update(case=label,real_files=real,arguments=args,files=files,passed=True)
        results.append(result)

run('success_backup')
run('success_no_backup',suffix='',expected=(0,0,1,0,0,0))
run('backup_failure',backup=True,expected=(4,1,0,0,1,1))
run('replacement_failure',replace=True,expected=(4,1,1,1,1,1))
run('replacement_recovery_failure',replace=True,restore=True,expected=(4,1,1,1,1,2))
run('replacement_failure_no_backup',suffix='',replace=True,expected=(4,0,1,0,1,1))
for label,option in [('close','close'),('write','write')]:
    run(label+'_failure',**{option:True},expected=(4,0,0,0,1,1))
for option in ['read','space']:
    run(option+'_failure',**{option:True},expected=(4,0,0,0,1,0))
run('script_io_failure',io=True,expected=(4,0,0,0,1,1))
run('regex_failure',regex=True,expected=(1,0,0,0,1,1))
for leaving in [0,7]:
    run('backup_failure_quit_'+str(leaving),backup=True,leaving=leaving,expected=(4,1,0,0,1,1))
    run('replace_failure_quit_'+str(leaving),replace=True,leaving=leaving,expected=(4,1,1,1,1,1))
    run('prior_error_quit_'+str(leaving),leaving=leaving,prior=2,expected=(2,1,1,0,0,0))
run('successful_quit_7',leaving=7,expected=(7,1,1,0,0,0))
# name is five bytes: length+suffix==4095 fits with its NUL; 4096 does not.
run('backup_path_last_fitting',suffix_length=4090)
run('backup_path_first_rejected',suffix_length=4091,expected=(4,0,0,0,1,1))
run('backup_path_far_rejected',suffix_length=8192,expected=(4,0,0,0,1,1))
run('multiple_success',inputs=2,expected=(0,2,2,0,0,0))
run('multiple_failure_first',inputs=2,backup=True,expected=(4,1,0,0,1,1))
run('multiple_failure_second',inputs=2,backup=True,failure_at=1,expected=(4,2,1,0,1,1))
run('real_success_backup',real=True)
run('real_success_no_backup',real=True,suffix='',expected=(0,0,1,0,0,0))
run('real_owned_backup_directory',real=True,backup_directory=True,expected=(4,1,0,0,1,1))
run('real_injected_backup_failure',real=True,backup=True,expected=(4,1,0,0,1,1))
run('real_injected_replacement_failure',real=True,replace=True,expected=(4,1,1,1,1,1))
run('real_injected_recovery_failure',real=True,replace=True,restore=True,expected=(4,1,1,1,1,2))
run('real_concurrent_path_preserved',real=True,replace=True,race=True,expected=(4,1,1,1,1,2))
run('real_replacement_failure_no_backup',real=True,suffix='',replace=True,expected=(4,0,1,0,1,1))
run('real_resolved_symlink_target_success',real=True,symlink=True)
run('real_resolved_symlink_target_recovery',real=True,symlink=True,replace=True,expected=(4,1,1,1,1,1))
report=dict(source_sha256=hashlib.sha256(source.encode()).hexdigest(),extracted_spans=spans,
            host=platform.platform(),compiler_command=command,build_status=build.returncode,
            binary_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),cases_run=len(results),results=results,
            limitations='Hosted current commit helper and entry suffix. Direct state injection bypasses parsing, substitution, real read/write/close, loop setup and --follow-symlinks resolution. Real-files cases exercise libc rename/link/unlink in owned temporary directories. Recovery race is injected at the restoration boundary. No full applet run or GNU comparison claimed.')
(OUT/'C03-results.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'cases':len(results),'all_passed':True}))

#!/usr/bin/env python3
"""Generate a freestanding no-device C16 fixture from the current entry path.

Only the census is mocked. The exact applet prefix handles arguments, presets,
column requirements, selection and rows; output formatting is not exercised.
Compile the generated C with the same flags as other freestanding shell tests.
"""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1]
source = (root/'src/sh/util_linux.c').read_text()
start = source.index('static b32 util_linux_lsblk()')
stop = source.index('        ul_lsblk_bytes = ', start)
prefix = source[start:stop].replace('util_linux_lsblk()', 'fixture_lsblk()').replace('ul_lsblk_take(identity, permissions, metadata)', 'fixture_take(identity, permissions, metadata)')
prefix += '        return 0;\n}\n'
code = r'''
#include "src/compiler_memory.c"
#include "src/spark.c"
#include "src/sh/shell.c"
#include "test/counted.inc"
static ul_lsblk_device fixture_devices[3], fixture_rows[3];
static bool got_identity, got_permissions, got_metadata;
static bool fixture_take(bool identity, bool permissions, bool metadata)
{
        got_identity=identity; got_permissions=permissions; got_metadata=metadata;
        memory_zero(fixture_devices,sizeof(fixture_devices)); memory_zero(fixture_rows,sizeof(fixture_rows));
        ul_lsblk=(ul_lsblk_snapshot){.devices=fixture_devices,.rows=fixture_rows,.capacity=3,.count=3};
        fixture_devices[0]=(ul_lsblk_device){.kname="sda",.size=8192,.scsi=metadata};
        fixture_devices[1]=(ul_lsblk_device){.kname="nvme0n1",.size=8192};
        fixture_devices[2]=(ul_lsblk_device){.kname="sdb",.size=4096,.scsi=metadata};
        return true;
}
'''+prefix+r'''
b32 main(void)
{
        const string_address selections[]={"NAME","KNAME","+SIZE","NAME,VENDOR","NAME,UUID","NAME,OWNER",null};
        for(positive scsi=0;scsi<2;scsi++)
        for(positive list=0;list<2;list++)
        for(positive order=0;order<2;order++)
        for(positive selection=0;selection<array_count(selections);selection++)
        {
                string_address args[8]; positive argc=0;
                args[argc++]="lsblk";
                if(scsi && !order) args[argc++]="-S";
                if(list) args[argc++]="-l";
                if(selections[selection]) {
                        args[argc++]="-o"; args[argc++]=selections[selection];
                }
                if(scsi && order) args[argc++]="--scsi";
                program_arguments_use(args,argc);
                check("mocked lsblk option path succeeds",!fixture_lsblk());
                check("SCSI filtering seeds acquisition regardless of columns",
                      got_metadata == (scsi || selection==3));
                check("other column requirements remain independent",
                      got_identity == (selection==4) && got_permissions == (selection==5));
                check("mixed census retains exactly selected roots",ul_lsblk.row_count==(scsi?2:3));
                check("first selected root survives",string_equals(fixture_rows[0].kname,"sda"));
                if(scsi) check("last SCSI root survives",string_equals(fixture_rows[1].kname,"sdb"));
        }
        return test_report(null);
}
'''
Path(sys.argv[1]).write_text(code.replace('#include "', f'#include "{root}/'))

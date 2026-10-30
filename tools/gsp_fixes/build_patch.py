#!/usr/bin/env python3
"""Adapt pinned GSP 1.9 input/backdrop members without changing managed components."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component',type=Path,required=True)
    parser.add_argument('--archive',type=Path,required=True)
    parser.add_argument('--commands',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--ar',required=True)
    args=parser.parse_args()
    here=Path(__file__).resolve().parent
    manifest=json.loads((here/'manifest.json').read_text())
    if digest(args.archive)!=manifest['input_archive_sha256']:
        raise RuntimeError('GSP archive changed: review/update the pinned product patch before rebuilding')
    source=here/'upstream'
    for relative,expected in manifest['original_source_sha256'].items():
        expected=manifest['patched_source_sha256'].get(relative,expected)
        if digest(source/relative)!=expected:raise RuntimeError('Unreviewed patch input: '+relative)
    for relative,expected in manifest['public_header_sha256'].items():
        if digest(args.component/relative)!=expected:raise RuntimeError('GSP header ABI changed: '+relative)
    commands=json.loads(args.commands.read_text())
    entries=[c for c in commands if c['file'].endswith('/esp_gsp_prebuilt_anchor.c')]
    if len(entries)!=1:raise RuntimeError('Expected exactly one actual GSP prebuilt compile context')
    context=entries[0]
    original=context.get('arguments') or shlex.split(context['command'])
    flags=[];i=0
    while i<len(original):
        flag=original[i]
        if flag in ('-o','-MF','-MT','-MQ'):
            i+=2;continue
        if flag in ('-c','-MD','-MMD') or flag==context['file']:
            i+=1;continue
        flags.append(flag);i+=1
    args.output.parent.mkdir(parents=True,exist_ok=True)
    includes=[source/'priv_include']+[source/'src'/s for s in
        ['ui','runtime','effect','accel','renderer','backend/esp_lcd','platform/esp_idf','codec','port']]
    flags += ['-I'+str(p)for p in includes]
    flags += ['-O2','-fno-lto','-fno-fast-math','-UNDEBUG','-UGSP_ENABLE_FREETYPE','-DGSP_ENABLE_FREETYPE=1']
    objects=[]
    for relative in manifest['compile_sources']:
        obj=args.output.parent/(Path(relative).name+'.obj')
        subprocess.run([*flags,'-c',str(source/relative),'-o',str(obj)],cwd=context['directory'],check=True)
        objects.append(obj)
    staging=args.output.with_suffix('.staging.a')
    shutil.copyfile(args.archive,staging)
    subprocess.run([args.ar,'d',str(staging),*[p.name for p in objects]],check=True)
    subprocess.run([args.ar,'r',str(staging),*[str(p)for p in objects]],check=True)
    subprocess.run([args.ar,'s',str(staging)],check=True)
    staging.replace(args.output)
    receipt=dict(input_archive_sha256=digest(args.archive),output_archive_sha256=digest(args.output),
                 manifest_sha256=digest(here/'manifest.json'),compiler_context=context,
                 compile_flags=flags,objects={p.name:digest(p)for p in objects})
    args.output.with_suffix('.json').write_text(json.dumps(receipt,indent=2)+'\n')


if __name__=='__main__':main()

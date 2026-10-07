#!/usr/bin/env python3
"""Verify declared DVD catalog bounds, independently of enabled PGC controls."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

ADDITIONAL = r'''
  dvd.started = 1; vm.state.domain = DVD_DOMAIN_VTSTitle;
  vtsi_mat.nr_of_vts_audio_streams = 7;
  pgc.audio_control[2] = pgc.audio_control[6] = 0x8000;
  CHECK(dvdnav_get_number_of_streams(&dvd, DVD_AUDIO_STREAM) == 2);
  CHECK(dvdnav_get_number_of_stream_attributes(&dvd, DVD_AUDIO_STREAM) == 7);
  vtsi_mat.nr_of_vts_subp_streams = 32;
  CHECK(dvdnav_get_number_of_stream_attributes(&dvd, DVD_SUBTITLE_STREAM) == 32);
  vtsi_mat.nr_of_vts_subp_streams = 33;
  CHECK(dvdnav_get_number_of_stream_attributes(&dvd, DVD_SUBTITLE_STREAM) == -1);
  vtsi_mat.nr_of_vts_audio_streams = 9;
  CHECK(dvdnav_get_number_of_stream_attributes(&dvd, DVD_AUDIO_STREAM) == -1);
  vm.state.domain = DVD_DOMAIN_VMGM;
  CHECK(dvdnav_get_number_of_stream_attributes(&dvd, DVD_AUDIO_STREAM) == 0);
  CHECK(dvdnav_get_number_of_stream_attributes(&dvd, DVD_SUBTITLE_STREAM) == 1);
  vm.state.domain = DVD_DOMAIN_FirstPlay;
  CHECK(dvdnav_get_number_of_stream_attributes(&dvd, DVD_SUBTITLE_STREAM) == 1);
  vm.state.domain = DVD_DOMAIN_VTSMenu;
  CHECK(dvdnav_get_number_of_stream_attributes(&dvd, DVD_AUDIO_STREAM) == 0);
  CHECK(dvdnav_get_number_of_stream_attributes(&dvd, DVD_SUBTITLE_STREAM) == 1);
  dvd.started = 0;
  CHECK(dvdnav_get_number_of_stream_attributes(&dvd, DVD_AUDIO_STREAM) == -1);
  CHECK(dvdnav_get_number_of_stream_attributes(&dvd, 2) == -1);
  CHECK(lock_depth == 0);
'''

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source', type=Path)
    ap.add_argument('--cc', default='cc')
    args = ap.parse_args()
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('logical_selection', here / 'test-libdvdnav-logical-selection.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    api = (args.source / 'src/dvdnav.c').read_text(encoding='utf-8')
    vm = (args.source / 'src/vm/vmget.c').read_text(encoding='utf-8')
    functions = [module.function(vm, name) for name in (
        'vm_get_audio_stream', 'vm_get_subp_stream', 'vm_get_audio_active_stream',
        'vm_get_subp_active_stream')]
    functions += [module.function(api, name) for name in (
        'get_number_of_stream_attributes', 'dvdnav_get_active_logical_stream',
        'dvdnav_get_number_of_streams', 'dvdnav_get_number_of_stream_attributes')]
    fixture = (here / 'dvdnav-logical-selection.c').read_text(encoding='utf-8')
    fixture = fixture.replace('int nr_of_', 'uint8_t nr_of_')
    fixture = fixture.replace('  printf("%d checks, %d failures', ADDITIONAL + '\n  printf("%d checks, %d failures')
    with tempfile.TemporaryDirectory(prefix='dvdnav-stream-attributes-') as directory:
        work = Path(directory)
        (work / 'dvdnav-logical-selection-under-test.h').write_text('\n\n'.join(functions), encoding='utf-8')
        test = work / 'test.c'
        test.write_text(fixture, encoding='utf-8')
        exe = work / ('test.exe' if os.name == 'nt' else 'test')
        if Path(args.cc).stem.lower() in ('cl', 'clang-cl'):
            flags = ['/nologo', '/std:c17', '/W4', '/WX', str(test), f'/Fe:{exe}', f'/Fo:{work / "test.obj"}']
        else:
            flags = ['-std=c17', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(exe)]
        subprocess.run([args.cc] + flags, cwd=work, check=True, timeout=60)
        subprocess.run([str(exe)], cwd=work, check=True, timeout=30)

if __name__ == '__main__':
    main()

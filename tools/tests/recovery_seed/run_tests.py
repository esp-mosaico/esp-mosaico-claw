#!/usr/bin/env python3
"""Exercise the production POSIX seeding functions and deterministic marker."""
from pathlib import Path
import os
import subprocess
import tempfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
source=(ROOT/'main/app_fs.c').read_text()
# Compile the unmodified production file-I/O functions, without board mount/task
# dependencies. The normal IDF build checks the complete translation unit.
functions=source[source.index('static esp_err_t copy_file('):source.index('static esp_err_t build_named_path(')]
with tempfile.TemporaryDirectory(prefix='recovery-seed-') as directory:
    work=Path(directory);seed=work/'seed';data=work/'data'
    (seed/'memory').mkdir(parents=True);(seed/'skills/weather').mkdir(parents=True)
    (data/'memory').mkdir(parents=True)
    (seed/'memory/user.md').write_text('factory default')
    (seed/'skills/weather/SKILL.md').write_text('bundled skill')
    (data/'memory/user.md').write_text('user customized memory')
    program=work/'seed.c'
    program.write_text('''#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_SIZE 0x104
#define APP_FS_COPY_BUFFER_SIZE 512
#define APP_FS_RECOVERY_PATH_SIZE 256
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_RETURN_ON_FALSE(x,e,...) do {if(!(x))return e;}while(0)
#define ESP_GOTO_ON_FALSE(x,e,label,...) do {if(!(x)){ret=e;goto label;}}while(0)
'''+functions+'''\nint main(int argc,char **argv) {assert(argc==3);
assert(recover_files(argv[1],argv[2])==ESP_OK);
assert(recover_files(argv[1],argv[2])==ESP_OK);return 0;}\n''')
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined',str(program),'-o',str(work/'seed-test')],check=True)
    subprocess.run([str(work/'seed-test'),str(seed),str(data)],check=True)
    assert (data/'memory/user.md').read_text()=='user customized memory'
    assert (data/'skills/weather/SKILL.md').read_text()=='bundled skill'
    print('PASS repeated/upgrade seeding preserves existing user memory and fills nested missing files')
    script=ROOT/'tools/cmake/generate_recovery_version.cmake'
    def marker():
        subprocess.run(['cmake',f'-DOUTPUT_FILE={work}/marker',f'-DSEED_ROOT={seed}','-P',str(script)],check=True)
        return (work/'marker').read_bytes()
    first=marker();assert len(first)==32 and marker()==first
    (seed/'memory/user.md').write_text('new factory default');assert marker()!=first
    subprocess.run([str(work/'seed-test'),str(seed),str(data)],check=True)
    assert (data/'memory/user.md').read_text()=='user customized memory'
    print('PASS only seed content changes its 32-byte marker; a new seed still preserves user changes')

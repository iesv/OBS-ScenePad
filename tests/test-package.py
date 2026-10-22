"""Validate binary-only release packages and their installers."""
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import sys
import tempfile
import time
from zipfile import ZipFile

archive = Path(sys.argv[1]).resolve()
windows = archive.name.endswith('Windows-x64.zip')
dll_name = 'obs-scenepad/bin/64bit/obs-scenepad.dll'
resources = 'obs-scenepad/data' if windows else 'obs-scenepad.plugin/Contents/Resources'
installer_name = 'install_OBS-ScenePad.bat' if windows else 'install_OBS-ScenePad.sh'
version = re.fullmatch(r'OBS-ScenePad-(\d+\.\d+\.\d+)-(Windows-x64|macOS-arm64).zip', archive.name).group(1)
with ZipFile(archive) as release:
    names = release.namelist()
    assert [n for n in names if n.lower().endswith(('.bat', '.sh'))] == [installer_name], names
    assert not any(Path(n).suffix.lower() in {'.cpp', '.hpp', '.h', '.in', '.py', '.ps1', '.pdb', '.a', '.lib'} for n in names)
    assert 'README.md' in names
    assert release.read('README.md').isascii()
    installer_bytes = release.read(installer_name)
    assert installer_bytes.isascii(), 'Installer text must be English'
    if windows:
        bat = installer_bytes
        dll = release.read(dll_name)
        assert f'ScenePad {version}'.encode() in bat and b'@PLUGIN_VERSION@' not in bat
        assert not bat.startswith(b'\xef\xbb\xbf') and len(bat) < 16000
        assert hashlib.sha256(dll).hexdigest().encode() in bat
        assert dll[:2] == b'MZ'
        pe = struct.unpack_from('<I', dll, 0x3c)[0]
        assert dll[pe:pe+4] == b'PE\0\0' and struct.unpack_from('<H', dll, pe+4)[0] == 0x8664
    else:
        info = plistlib.loads(release.read('obs-scenepad.plugin/Contents/Info.plist'))
        assert info['CFBundleShortVersionString'] == info['CFBundleVersion'] == version
        assert release.getinfo(installer_name).external_attr >> 16 & 0o111
        assert b'xattr -dr com.apple.quarantine "$target"' in installer_bytes
    keys = []
    for locale in ('en-US', 'zh-CN', 'zh-TW'):
        packaged = release.read(f'{resources}/locale/{locale}.ini')
        assert packaged == (Path(__file__).resolve().parents[1] / 'data/locale' / f'{locale}.ini').read_bytes(), locale
        content = packaged.decode('utf-8')
        parsed = dict((k, json.loads(v)) for k, v in re.findall(r'^([A-Za-z0-9]+)=(".*")\r?$', content, re.M))
        assert parsed and all(parsed.values()), locale
        keys.append(set(parsed))
    assert keys[0] == keys[1] == keys[2]
print('PASS: binary-only package, installer, version, and three complete locales')

if os.name == 'nt':
    with tempfile.TemporaryDirectory(prefix='scenepad-package-', dir=archive.parent) as directory:
        folder = Path(directory)
        unpacked = folder / '安装 package space'
        with ZipFile(archive) as release:
            release.extractall(unpacked)
        installer = unpacked / 'install_OBS-ScenePad.bat'
        destination = folder / '复制 output'
        env = dict(os.environ, SCENEPAD_TEST_BAT=str(installer), SCENEPAD_TEST_TARGET=str(destination))
        command = [r'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe', '-NoProfile', '-Command',
                   '& $env:SCENEPAD_TEST_BAT /copy-to $env:SCENEPAD_TEST_TARGET; exit $LASTEXITCODE']
        start = time.monotonic()
        proc = subprocess.run(command, env=env, capture_output=True, timeout=25)
        elapsed = time.monotonic() - start
        assert proc.returncode == 0, proc.stdout + proc.stderr
        assert 5 <= elapsed < 25, elapsed
        assert re.search(rb'\r5[^\r\n]*\r4[^\r\n]*\r3[^\r\n]*\r2[^\r\n]*\r1', proc.stdout), repr(proc.stdout)
        installed = destination / dll_name
        assert installed.read_bytes() == dll
        for locale in ('en-US', 'zh-CN', 'zh-TW'):
            relative = f'obs-scenepad/data/locale/{locale}.ini'
            assert (destination / relative).read_bytes() == (unpacked / relative).read_bytes()
        print(f'PASS: BAT copies plugin and resources from Unicode/spaces path; same-line countdown; {elapsed:.2f}s')
        proc = subprocess.run(command, env=env, capture_output=True, timeout=25)
        assert proc.returncode == 0 and installed.read_bytes() == dll
        assert not (destination / 'obs-scenepad/obs-scenepad').exists()
        print('PASS: repeat install/update keeps the correct directory layout')
        installer_copy = folder / 'orphan.bat'
        installer_copy.write_bytes(bat)
        env.update(SCENEPAD_TEST_BAT=str(installer_copy), SCENEPAD_TEST_TARGET=str(folder / 'missing-target'))
        proc = subprocess.run(command, env=env, capture_output=True, timeout=25)
        assert proc.returncode == 1 and not (folder / 'missing-target').exists()
        print('PASS: missing plugin folder is reported; no partial installation is created')

elif sys.platform == 'darwin':
    with tempfile.TemporaryDirectory(prefix='scenepad-package-', dir=archive.parent) as directory:
        folder = Path(directory)
        unpacked = folder / '安装 package space'
        with ZipFile(archive) as release:
            release.extractall(unpacked)
            for entry in release.infolist():
                os.chmod(unpacked / entry.filename, entry.external_attr >> 16 & 0o777)
        source = unpacked / 'obs-scenepad.plugin'
        binary = source / 'Contents/MacOS/obs-scenepad'
        architectures = subprocess.check_output(['lipo', '-archs', str(binary)], text=True).split()
        assert set(architectures) == {'arm64'}, architectures
        dependencies = subprocess.check_output(['otool', '-L', str(binary)], text=True)
        for line in dependencies.splitlines()[1:]:
            name = line.strip().split(' (')[0]
            assert name.startswith(('@rpath/', '/System/Library/', '/usr/lib/')), name
        subprocess.run(['xattr', '-w', 'com.apple.quarantine', '0081;00000000;ScenePad;', str(binary)], check=True)
        destination = folder / '复制 output'
        command = ['/bin/bash', str(unpacked / installer_name), '--copy-to', str(destination)]
        subprocess.run(command, check=True, capture_output=True, timeout=20)
        target = destination / 'obs-scenepad.plugin'
        assert (target / 'Contents/MacOS/obs-scenepad').read_bytes() == binary.read_bytes()
        attributes = subprocess.check_output(['/usr/bin/xattr', '-r', str(target)], text=True)
        assert 'com.apple.quarantine' not in attributes
        subprocess.run(['codesign', '--verify', '--strict', str(target)], check=True)
        subprocess.run(command, check=True, capture_output=True, timeout=20)
        print('PASS: ARM64 Mach-O, portable dependencies, copy/update, quarantine removal, and ad-hoc signature')
print('PASS: package verification complete')

"""Package installed binaries for GitHub Actions."""
import hashlib
import os
from pathlib import Path
import re
import sys
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

root = Path(__file__).resolve().parent.parent
platform = sys.argv[1]
staging = Path(sys.argv[2]).resolve()
version = re.search(r'project\(obs-scenepad VERSION ([\d.]+)', (root / 'CMakeLists.txt').read_text(encoding='utf-8')).group(1)
if os.environ.get('GITHUB_REF_TYPE') == 'tag':
    assert os.environ['GITHUB_REF_NAME'] == version, 'Release tag must match the plugin version'
if platform == 'windows':
    plugin = staging / 'obs-scenepad'
    binary = plugin / 'bin/64bit/obs-scenepad.dll'
    template = (root / 'installers/install_OBS-ScenePad.bat.in').read_text(encoding='utf-8')
    installer = template.replace('@PLUGIN_VERSION@', version).replace('@DLL_SHA256@', hashlib.sha256(binary.read_bytes()).hexdigest())
    installer_name = 'install_OBS-ScenePad.bat'
    installer_bytes = installer.replace('\r\n', '\n').replace('\n', '\r\n').encode('utf-8')
    suffix = 'Windows-x64'
elif platform == 'macos':
    plugin = staging / 'obs-scenepad.plugin'
    binary = plugin / 'Contents/MacOS/obs-scenepad'
    installer_name = 'install_OBS-ScenePad.sh'
    installer_bytes = (root / 'installers' / installer_name).read_bytes()
    suffix = 'macOS-arm64'
else:
    raise SystemExit('Unsupported platform')
assert binary.is_file()
output = root / 'dist'
output.mkdir(exist_ok=True)
archive = output / f'OBS-ScenePad-{version}-{suffix}.zip'
with ZipFile(archive, 'w', compression=ZIP_DEFLATED) as release:
    info = ZipInfo(installer_name)
    info.create_system = 3
    info.external_attr = 0o100755 << 16
    release.writestr(info, installer_bytes, compress_type=ZIP_DEFLATED)
    release.write(root / 'README.md', 'README.md')
    for file in sorted(plugin.rglob('*')):
        if file.is_file():
            assert file.suffix.lower() not in {'.cpp', '.hpp', '.h', '.in', '.py', '.ps1', '.pdb', '.a', '.lib'}
            release.write(file, file.relative_to(staging).as_posix())
print(archive)

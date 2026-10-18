"""Build the app and logger while retaining the exact upstream root payloads.

The fixed base APK digest is published by upstream for tag v1.16-vr. No device
data or signing key is copied into the source tree. Gradle uses the user's
standard Android debug keystore when localDebugSigning is requested.
"""
import argparse, hashlib, json, os, subprocess, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE_SHA256 = 'e2a0ff6984697cc140628d3fab6eda54caf1d2638f3e5f9f63f7aeee27f6a16d'
parser = argparse.ArgumentParser()
parser.add_argument('--base-apk', type=Path, required=True)
parser.add_argument('--sdk', type=Path, default=Path(os.environ.get('ANDROID_HOME', Path.home()/'AppData/Local/Android/Sdk')))
parser.add_argument('--ndk', type=Path)
args = parser.parse_args()
assert hashlib.sha256(args.base_apk.read_bytes()).hexdigest() == BASE_SHA256, 'Unexpected base APK'
sdk = args.sdk.resolve()
ndk = args.ndk or sdk/'ndk/27.2.12479018'
host = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64'
compiler = ndk/'toolchains/llvm/prebuilt'/host/'bin'/('aarch64-linux-android32-clang.cmd' if os.name == 'nt' else 'aarch64-linux-android32-clang')
out = ROOT/'out/logfix'
libs = out/'native/arm64-v8a'
libs.mkdir(parents=True, exist_ok=True)
payloads = {}
with zipfile.ZipFile(args.base_apk) as archive:
    for name in ['libdfroot.so', 'libbootstrap.so', 'libvrext.so']:
        data = archive.read('lib/arm64-v8a/'+name)
        (libs/name).write_bytes(data)
        payloads[name] = hashlib.sha256(data).hexdigest()
subprocess.run([str(compiler), '-O2', '-Wall', '-Wextra', '-Werror', '-fPIE', '-pie',
                '-Wl,-z,max-page-size=16384', str(ROOT/'app/src/main/jni/kmsglog.c'),
                '-o', str(libs/'libkmsglog.so')], check=True)
(ROOT/'local.properties').write_text('sdk.dir='+sdk.as_posix()+'\n', encoding='utf8')
command = [str(ROOT/('gradlew.bat' if os.name == 'nt' else 'gradlew')),
           ':app:assembleRelease', '-PlocalDebugSigning=true',
           '-PnativePayloadDir='+str(libs.parent), '--console=plain']
subprocess.run(command, cwd=ROOT, check=True)
apk = ROOT/'app/build/outputs/apk/release/dirtyfrag.apk'
with zipfile.ZipFile(apk) as archive:
    for name, sha in payloads.items():
        assert hashlib.sha256(archive.read('lib/arm64-v8a/'+name)).hexdigest() == sha, name+' changed'
    new_logger = hashlib.sha256((libs/'libkmsglog.so').read_bytes()).hexdigest()
    assert hashlib.sha256(archive.read('lib/arm64-v8a/libkmsglog.so')).hexdigest() == new_logger
(out/'manifest.json').write_text(json.dumps({'baseApkSha256':BASE_SHA256,
    'retainedNativePayloads':payloads, 'loggerSha256':new_logger,
    'apkSha256':hashlib.sha256(apk.read_bytes()).hexdigest()}, indent=2)+'\n', 'utf8')
print(apk)

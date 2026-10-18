"""Compile native logger regression tests and run in a temporary Android shell.

Usage: python tools/test_kmsglog.py --ssh root@PHONE --port PORT
Tests use private temporary files, never /dev/kmsg or the root loader.
"""
import argparse, hashlib, json, os, subprocess
from pathlib import Path

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--ssh',required=True)
parser.add_argument('--port',default='22')
parser.add_argument('--ndk',type=Path)
args=parser.parse_args()
sdk=Path(os.environ.get('ANDROID_HOME',Path.home()/'AppData/Local/Android/Sdk'))
ndk=args.ndk or sdk/'ndk/27.2.12479018'
host='windows-x86_64' if os.name=='nt' else 'linux-x86_64'
compiler=ndk/'toolchains/llvm/prebuilt'/host/'bin'/('aarch64-linux-android32-clang.cmd' if os.name=='nt' else 'aarch64-linux-android32-clang')
out=root/'out/logfix';out.mkdir(parents=True,exist_ok=True)
binary=out/'kmsglog-tests'
subprocess.run([str(compiler),'-O2','-Wall','-Wextra','-Werror','-fstack-protector-strong',
                '-fsanitize=bounds','-fsanitize-trap=bounds',str(root/'tools/kmsglog_test.c'),'-o',str(binary)],check=True)
data=binary.read_bytes();sha=hashlib.sha256(data).hexdigest()
remote='/data/local/tmp/df-kmsglog-regression-'+sha[:12]
ssh=['ssh','-o','BatchMode=yes','-o','ConnectTimeout=5','-p',args.port,args.ssh]
def call(command,data=None):
    result=subprocess.run(ssh+[command],input=data,capture_output=True,timeout=30)
    if result.returncode:raise RuntimeError(result.stdout.decode('utf8','replace')+result.stderr.decode('utf8','replace'))
    return result.stdout.decode('utf8','replace')
try:
    call('cat > '+remote,data)
    assert call('sha256sum '+remote).split()[0]==sha
    result=call('chmod 700 '+remote+' && '+remote)
    assert 'PASS all kmsglog regression tests' in result,result
    print(result,end='')
    (out/'test-results.json').write_text(json.dumps({'binarySha256':sha,'scope':'Android native syscall fault injection; isolated temporary files','output':result},indent=2)+'\n','utf8')
finally:
    call('rm -f '+remote)

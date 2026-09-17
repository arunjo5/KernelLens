#!/usr/bin/env python3
"""Copy KernelLens source files to the local Linux VM."""
import argparse
from pathlib import Path
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VM = ROOT / '.local' / 'vm-arm64'
parser = argparse.ArgumentParser(description=__doc__)
parser.parse_args()

def command(*items):
    subprocess.run([str(item) for item in items], check=True)

def transfer(archive, filename):
    command('scp', '-i', VM/'id_ed25519', '-P', '22222', '-o',
            f'UserKnownHostsFile={VM / "known_hosts"}', archive, f'bench@127.0.0.1:/home/bench/{filename}')

with tempfile.TemporaryDirectory(dir=ROOT/'.local', prefix='sync-') as temporary:
    temporary = Path(temporary)
    archive = temporary/'tracer.tar.gz'
    with tarfile.open(archive, 'w:gz') as bundle:
        for name in ('CMakeLists.txt','CMakePresets.json','README.md','.gitignore','bpf','cmake','include','src','tests','scripts'):
            path = ROOT/name
            if path.exists():
                bundle.add(path, arcname=name, filter=lambda info: None if '__pycache__' in info.name else info)
    transfer(archive, 'tracer-source.tar.gz')
    command('python3', ROOT/'scripts/vm.py', 'ssh', 'mkdir -p /home/bench/latency-tracer && tar -xzf /home/bench/tracer-source.tar.gz -C /home/bench/latency-tracer')

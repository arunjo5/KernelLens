#!/usr/bin/env python3
"""Native ARM64 Ubuntu VM on Apple Silicon. Requires qemu and pycdlib."""
import argparse
import hashlib
import io
from pathlib import Path
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VM = ROOT / '.local' / 'vm-arm64'
BASE_URL = 'https://cloud-images.ubuntu.com/noble/20260911/'
IMAGE_SHA256 = '7b682958a67ff5de068e36de6af8b75fa645d296af5a70d6500527f6a33781db'

def run(*args):
    subprocess.run([str(x) for x in args], check=True)

def prepare():
    import pycdlib
    VM.mkdir(parents=True, exist_ok=True)
    base = VM / 'ubuntu-arm64.img'
    if not base.exists():
        urllib.request.urlretrieve(BASE_URL + 'noble-server-cloudimg-arm64.img', base)
    sums = VM / 'SHA256SUMS'
    if not sums.exists():
        urllib.request.urlretrieve(BASE_URL + 'SHA256SUMS', sums)
    expected = next(line.split()[0] for line in sums.read_text().splitlines()
                    if line.endswith('*noble-server-cloudimg-arm64.img'))
    actual = hashlib.file_digest(base.open('rb'), 'sha256').hexdigest()
    if expected != actual or actual != IMAGE_SHA256:
        raise SystemExit('Cloud image checksum mismatch')
    key = VM / 'id_ed25519'
    if not key.exists():
        run('ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', key)
    disk = VM / 'disk.qcow2'
    if not disk.exists():
        run('qemu-img', 'create', '-f', 'qcow2', '-F', 'qcow2', '-b', base, disk, '30G')
    user_data = '''#cloud-config
hostname: latency-bench
users:
  - name: bench
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys:
      - ''' + key.with_suffix('.pub').read_text().strip() + '''
ssh_pwauth: false
disable_root: true
package_update: false
'''
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=3, joliet=3, rock_ridge='1.09', vol_ident='cidata')
    for name, data in [('user-data', user_data), ('meta-data', 'instance-id: latency-local-v1\nlocal-hostname: latency-bench\n')]:
        content = data.encode()
        iso.add_fp(io.BytesIO(content), len(content), iso_path='/' + name.upper().replace('-', '_') + ';1', joliet_path='/' + name, rr_name=name)
    iso.write(str(VM / 'seed.iso'))
    iso.close()
    print(f'Image verified: {actual}')

def start():
    if (VM / 'qemu.pid').exists():
        raise SystemExit('VM pidfile exists; check running VM before removing it')
    run('qemu-system-aarch64', '-name', 'latency-bench', '-machine', 'virt,gic-version=3',
        '-accel', 'hvf', '-cpu', 'host', '-smp', '4', '-m', '8192',
        '-bios', '/opt/homebrew/share/qemu/edk2-aarch64-code.fd',
        '-drive', f'file={VM / "disk.qcow2"},format=qcow2,if=virtio,cache=none',
        '-drive', f'file={VM / "seed.iso"},format=raw,media=cdrom,readonly=on',
        '-netdev', 'user,id=n0,hostfwd=tcp:127.0.0.1:22222-:22',
        '-device', 'virtio-net-pci,netdev=n0', '-display', 'none',
        '-serial', f'file:{VM / "serial.log"}', '-daemonize', '-pidfile', VM / 'qemu.pid')

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('action', choices=['prepare', 'start', 'ssh', 'stop'])
parser.add_argument('command', nargs=argparse.REMAINDER)
args = parser.parse_args()
if args.action == 'prepare':
    prepare()
elif args.action == 'start':
    start()
else:
    options = ['-i', str(VM / 'id_ed25519'), '-p', '22222', '-o', 'ConnectTimeout=8',
               '-o', 'StrictHostKeyChecking=accept-new', '-o', f'UserKnownHostsFile={VM / "known_hosts"}',
               'bench@127.0.0.1']
    run('ssh', *options, *(args.command if args.action == 'ssh' else ['sudo', 'poweroff']))

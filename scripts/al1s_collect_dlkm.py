#!/usr/bin/env python3
"""Publish only a stripped msm_drm.ko; keep verification details in the log."""
import argparse, hashlib, json, re, shutil, struct, subprocess
from pathlib import Path

def allocated_sections(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise SystemExit('Expected a little-endian ARM64 ELF64 module: ' + str(path))
    offset = struct.unpack_from('<Q', data, 40)[0]
    size, count, strings_index = struct.unpack_from('<HHH', data, 58)
    if size != 64 or not count or strings_index >= count:
        raise SystemExit('Unsupported ELF section table')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * size) for i in range(count)]
    strings = sections[strings_index]
    names = data[strings[4]:strings[4] + strings[5]]
    result = {}
    for section in sections:
        if section[2] & 2:  # SHF_ALLOC: everything needed by the module loader.
            name = names[section[0]:].split(b'\0', 1)[0].decode()
            payload = b'' if section[1] == 8 else data[section[4]:section[4] + section[5]]
            result[name] = (section[1], section[2], section[5], hashlib.sha256(payload).hexdigest())
    return result

p=argparse.ArgumentParser()
p.add_argument('workspace',type=Path)
p.add_argument('output',type=Path)
a=p.parse_args()
module=a.workspace/'kernel_platform/out/al1s-display/msm_drm.ko'
if not module.is_file():
    raise SystemExit('External display DDK output missing: '+str(module))
data=module.read_bytes()
if not data.startswith(b'\x7fELF') or b'al1s_originos_init' not in data:
    raise SystemExit('msm_drm.ko is not an ELF containing the AL1S adapter')
constants = (a.workspace/'kernel_platform/common/build.config.constants').read_text()
version = re.search(r'^CLANG_VERSION=[\"\']?(r[0-9a-z]+)', constants, re.M)
if not version:
    raise SystemExit('CLANG_VERSION missing from common/build.config.constants')
strip = a.workspace/'kernel_platform/prebuilts/clang/host/linux-x86'/('clang-' + version.group(1))/'bin/llvm-strip'
if not strip.is_file():
    raise SystemExit('Matching LLVM strip tool missing: ' + str(strip))
before = allocated_sections(module)
a.output.mkdir(parents=True,exist_ok=True)
if any(a.output.iterdir()):
    raise SystemExit('Artifact output must be empty: ' + str(a.output))
destination = a.output/'msm_drm.ko'
shutil.copy2(module, destination)
subprocess.run([str(strip), '--strip-debug', str(destination)], check=True)
if allocated_sections(destination) != before:
    raise SystemExit('Stripping changed allocated module sections')
with destination.open('rb') as f:
    sha = hashlib.file_digest(f, 'sha256').hexdigest()
record = {'artifact_type': 'msm_drm_module_only', 'complete_dsu_image': False,
          'source': str(module.relative_to(a.workspace)), 'strip_tool': str(strip),
          'original_bytes': len(data), 'bytes': destination.stat().st_size,
          'sha256': sha, 'allocated_sections_verified': len(before)}
print(json.dumps(record,indent=2))

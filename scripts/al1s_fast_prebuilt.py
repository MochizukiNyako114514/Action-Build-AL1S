#!/usr/bin/env python3
"""Feed upstream Build Kernel FAST artifacts into the vendor mixed module build.

No kernel compilation happens here. Only sun_perf's base_kernel is replaced;
the display DDK still uses the official vendor configuration and headers.
"""
import argparse
import hashlib
import json
import re
import shutil
import tarfile
import tempfile
from pathlib import Path, PurePosixPath

p = argparse.ArgumentParser()
p.add_argument('workspace', type=Path)
p.add_argument('--analysis-only', action='store_true', help='Generate inert inputs for aquery only; never compile with these')
a = p.parse_args()
platform = a.workspace.resolve() / 'kernel_platform'
# FAST uses make O=out, which creates common/out/source -> common. Keep the
# entire output tree out of Bazel globs, including headers and nested symlinks.
ignore = platform / '.bazelignore'
ignore_text = ignore.read_text() if ignore.exists() else ''
if 'common/out' not in {line.strip() for line in ignore_text.splitlines()}:
    ignore.write_text(ignore_text.rstrip() + '\ncommon/out\n')
out = platform / 'common/out'
package = platform / 'build/kernel/kleaf/al1s_fast_prebuilt'
macro = platform / 'msm-kernel/msm_kernel_la.bzl'
label = '//build/kernel/kleaf/al1s_fast_prebuilt:kernel'
required = ['vmlinux', 'System.map', 'Module.symvers', 'vmlinux.symvers',
            'modules.builtin', 'modules.builtin.modinfo', 'modules.order', '.config',
            'include/config/kernel.release', 'arch/arm64/boot/Image']
if a.analysis_only:
    scratch = tempfile.TemporaryDirectory(prefix='al1s-analysis-inputs-')
    out = Path(scratch.name)
    for name in required:
        f = out / name
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text('')
    (out / 'Module.symvers').write_text(''.join(
        '0x00000000\t' + name + '\tvmlinux\tEXPORT_SYMBOL\n'
        for name in ['filp_open', 'kernel_read', 'filp_close']))
    (out / 'include/config/kernel.release').write_text('6.6.118-AL1S-analysis-only\n')
constants = (platform / 'common/build.config.constants').read_text()
version = re.search(r'^CLANG_VERSION=[\"\']?(r[0-9a-z]+)', constants, re.M)
if not version:
    raise SystemExit('Cannot determine the FAST compiler constraint from build.config.constants')
clang_constraint = '//prebuilts/clang/host/linux-x86/kleaf:' + version.group(1)
for name in required:
    if not (out / name).is_file():
        raise SystemExit('Build Kernel FAST artifact missing: ' + str(out / name))
symbols = {line.split()[1] for line in (out / 'Module.symvers').read_text().splitlines()
           if len(line.split()) >= 2}
for name in ['filp_open', 'kernel_read', 'filp_close']:
    if name not in symbols:
        raise SystemExit('Build Kernel FAST did not export required symbol: ' + name)
text = macro.read_text()
anchor = '        base_kernel = base_kernel,\n'
if text.count(anchor) != 1 or label in text:
    raise SystemExit('Unexpected or already patched vendor kernel_build definition')
module_paths = []
for line in (out / 'modules.order').read_text().splitlines():
    item = PurePosixPath(line)
    if item.is_absolute() or '..' in item.parts:
        raise SystemExit('Unexpected module path: ' + line)
    if item.suffix == '.o':
        item = item.with_suffix('.ko')
    if not (out / str(item)).is_file():
        raise SystemExit('FAST module output missing: ' + str(item))
    module_paths.append(str(item))
if package.exists():
    raise SystemExit('Prebuilt package already exists; use a fresh build workspace')
package.mkdir()
record = {'base_kernel_source': 'upstream Build Kernel FAST common/out',
          'vendor_base_kernel': label, 'analysis_only': a.analysis_only,
          'clang_constraint': clang_constraint, 'files': {}}
for name in required:
    source = out / name
    destination = package / source.name
    shutil.copy2(source, destination)
    with source.open('rb') as f:
        record['files'][source.name] = hashlib.file_digest(f, 'sha256').hexdigest()
# These are real artifacts from the FAST build, not placeholder module archives.
release = (out / 'include/config/kernel.release').read_text().strip()
if not release or '/' in release:
    raise SystemExit('Invalid FAST kernel release')
with tarfile.open(package / 'unstripped_modules.tar.gz', 'w:gz') as archive:
    directory = tarfile.TarInfo('unstripped')
    directory.type = tarfile.DIRTYPE
    directory.mode = 0o755
    archive.addfile(directory)
    for name in module_paths:
        archive.add(out / name, arcname='unstripped/' + name, recursive=False)
with tarfile.open(package / 'modules_staging_dir.tar.gz', 'w:gz') as archive:
    for name in module_paths:
        archive.add(out / name, arcname=f'lib/modules/{release}/kernel/{name}', recursive=False)
    for name in ['modules.order', 'modules.builtin', 'modules.builtin.modinfo']:
        archive.add(out / name, arcname=f'lib/modules/{release}/{name}', recursive=False)
sources = [Path(name).name for name in required]
build = '''load("//build/kernel/kleaf/impl:kernel_filegroup.bzl", "kernel_filegroup")

platform(
    name = "kernel_platform_target",
    constraint_values = ["@platforms//os:android", "@platforms//cpu:arm64", %s],
)
platform(
    name = "kernel_platform_exec",
    constraint_values = ["@platforms//os:linux", "@platforms//cpu:x86_64", %s],
)
kernel_filegroup(
    name = "kernel",
    srcs = %s,
    deps = ["unstripped_modules.tar.gz", "modules_staging_dir.tar.gz"],
    kernel_release = "kernel.release",
    all_module_names = %s,
    target_platform = ":kernel_platform_target",
    exec_platform = ":kernel_platform_exec",
    visibility = ["//visibility:public"],
)
''' % (json.dumps(clang_constraint), json.dumps(clang_constraint), json.dumps(sources), json.dumps(module_paths))
(package / 'BUILD.bazel').write_text(build)
# Keep the original common symbol-list target; it only supplies text files.
text = text.replace(anchor,
    f'        base_kernel = "{label}" if target == "sun_perf" else base_kernel,\n')
macro.write_text(text)
# The upstream full builder normally creates these before invoking Bazel.
for name, target in [('msm_kernel_extensions.bzl', '../msm-kernel/msm_kernel_extensions.bzl'),
                     ('abl_extensions.bzl', '../bootable/bootloader/edk2/abl_extensions.bzl')]:
    link = platform / 'build' / name
    if not link.exists():
        if link.is_symlink():
            link.unlink()
        link.symlink_to(target)
(a.workspace / 'al1s-fast-prebuilt.json').write_text(json.dumps(record, indent=2))
print('AL1S: analysis-only placeholders' if a.analysis_only else
      'AL1S: vendor modules use the completed Build Kernel FAST kernel; no second GKI build.')

#!/usr/bin/env python3
"""Analyze the real vendor build graph before spending time on the FAST kernel."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

workspace = Path(sys.argv[1]).resolve(strict=True)
platform = workspace / 'kernel_platform'
macro = platform / 'msm-kernel/msm_kernel_la.bzl'
package = platform / 'build/kernel/kleaf/al1s_fast_prebuilt'
record = workspace / 'al1s-fast-prebuilt.json'
if package.exists() or package.is_symlink() or record.exists():
    raise SystemExit('Preflight requires a clean generated prebuilt package')
original = macro.read_bytes()
try:
    result = subprocess.run([
        'bash', str(Path(__file__).with_name('al1s_build_display.sh')),
        str(workspace), 'sun', 'perf', 'true'],
        env=dict(os.environ, AL1S_ANALYSIS_ONLY='true'))
    raise SystemExit(result.returncode)
finally:
    macro.write_bytes(original)
    # Only remove the exact generated package; never follow a changed symlink.
    if package.exists():
        if package.is_symlink() or package.resolve().parent != (platform / 'build/kernel/kleaf').resolve():
            raise RuntimeError('Generated package path changed unexpectedly')
        shutil.rmtree(package)
    record.unlink(missing_ok=True)
    graph = workspace / 'al1s-module-actions.json'
    if graph.exists():
        graph.replace(workspace / 'al1s-preflight-actions.json')

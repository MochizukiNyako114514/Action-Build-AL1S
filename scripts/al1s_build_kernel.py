#!/usr/bin/env python3
"""Use the official builder lifecycle with an exact, matching 4K kernel target."""
import argparse
import importlib.util
import logging
import os
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('workspace', type=Path)
p.add_argument('platform', choices=['sun'])
p.add_argument('variant', choices=['perf'])
p.add_argument('fast', choices=['true', 'false'])
a = p.parse_args()
workspace = a.workspace.resolve()
os.chdir(workspace)
# Resolve the upstream symlink so its __file__-based workspace remains correct.
source = (workspace / 'kernel_platform/build_with_bazel.py').resolve(strict=True)
spec = importlib.util.spec_from_file_location('al1s_official_builder', source)
upstream = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upstream)
logging.basicConfig(level=logging.DEBUG, format='[AL1S] %(levelname)s: %(message)s')

class ExactKernelBuilder(upstream.BazelBuilder):
    def get_build_targets(self):
        # The upstream fuzzy query includes sun16k, doubling unnecessary work.
        label = f'//msm-kernel:{a.platform}_{a.variant}_dist'
        return [upstream.Target(self.workspace, a.platform, a.variant, label, self.out_dir)]

builder = ExactKernelBuilder(
    [(a.platform, a.variant)], list(upstream.DEFAULT_SKIP_LIST), None,
    str(workspace / 'bazel-cache'), False, False,
    ['--config=fast'] if a.fast == 'true' else [])
print(f'AL1S: exact 4K kernel target, Kleaf fast={a.fast}', flush=True)
builder.build()

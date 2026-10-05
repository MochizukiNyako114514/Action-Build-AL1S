#!/usr/bin/env bash
set -euo pipefail
kernel_root=$(realpath "$1")
# Pinned upstream NoMount, not a similarly named third-party suite.
nomount_commit=6b1be186322d4e0bdc465cf27f6fc0d3679087c6
nomount_source="$kernel_root/NoMount"
git clone --filter=blob:none --no-checkout https://github.com/maxsteeel/nomount.git "$nomount_source"
git -C "$nomount_source" checkout --detach "$nomount_commit"
mkdir -p "$kernel_root/fs/nomount"
cp -a "$nomount_source/kernel/src/." "$kernel_root/fs/nomount/"
printf '\nsource "fs/nomount/Kconfig"\n' >> "$kernel_root/fs/Kconfig"
printf '\nobj-$(CONFIG_NOMOUNT) += nomount/\n' >> "$kernel_root/fs/Makefile"
"$kernel_root/scripts/config" --file "$kernel_root/arch/arm64/configs/gki_defconfig" -e NOMOUNT
printf 'NoMount source: maxsteeel/nomount@%s\n' "$nomount_commit"

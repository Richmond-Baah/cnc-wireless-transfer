#!/usr/bin/env bash
# Release build of both boards in <board>/build-release: no stack audit, /debug
# compiled out, log level WARN (tools/sdkconfig.release over <board>/sdkconfig).
# Needs FILEXFER_SIGNING_KEY and an ESP-IDF shell (export.sh).
#   tools/build_release.sh [transmitter|receiver ...]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "${FILEXFER_SIGNING_KEY:-}" ] || { echo "FILEXFER_SIGNING_KEY is not set or missing" >&2; exit 1; }
boards=("$@"); [ ${#boards[@]} -gt 0 ] || boards=(transmitter receiver)
for b in "${boards[@]}"; do
  out="$root/$b/build-release"
  mkdir -p "$out"
  python3 - "$root/$b/sdkconfig" "$root/tools/sdkconfig.release" "$out/sdkconfig" <<'PY'
import re, sys
base, overlay, dest = sys.argv[1:]
key = lambda l: (re.match(r"#?\s*(CONFIG_\w+)", l.strip()) or [None, None])[1]
over = {key(l): l.rstrip("\n") for l in open(overlay) if key(l)}
lines = [over.pop(key(l), l.rstrip("\n")) if key(l) else l.rstrip("\n") for l in open(base)]
open(dest, "w").write("\n".join(lines + list(over.values())) + "\n")
PY
  (cd "$root/$b" && idf.py -B "$out" -DSDKCONFIG="$out/sdkconfig" -DFILEXFER_RELEASE=ON -DFILEXFER_STACK_AUDIT=OFF build)
  echo "Release: $out/cnc_$b.bin"
done

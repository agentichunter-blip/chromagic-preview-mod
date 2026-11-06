#!/usr/bin/env bash
# Build and verify only. This script never connects to or flashes a device.
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${IDF_PATH:?Activate ESP-IDF v5.3 with its export.sh before running this script}"
case "$(idf.py --version)" in
  "ESP-IDF v5.3"|"ESP-IDF v5.3-dirty") ;;
  *) echo 'This build is pinned to ESP-IDF v5.3 (5.3.0).' >&2; exit 1 ;;
esac
host_python="${CHROMAGIC_HOST_PYTHON:-python3}"
"$host_python" -c 'from PIL import Image' || {
  echo 'Install tools/requirements-box-art.txt into CHROMAGIC_HOST_PYTHON first.' >&2
  exit 1
}
if [[ "$(uname -s)" == Darwin ]]; then
  export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
fi
idf.py -C "$project_dir" build size
cmake -S "$project_dir/tests/host" -B "$project_dir/build-host" -G Ninja \
  -DPython3_EXECUTABLE="$(command -v "$host_python")"
cmake --build "$project_dir/build-host"
ctest --test-dir "$project_dir/build-host" --output-on-failure

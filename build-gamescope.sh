#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="gamescope-bazzite-builder"
CONTAINER_NAME="gamescope-bazzite-build-$$"
OUTPUT_DIR="${SCRIPT_DIR}/build-output"

echo "[gamescope-bazzite] Building image..."
podman build -t "${IMAGE_NAME}" "${SCRIPT_DIR}"

echo "[gamescope-bazzite] Extracting build artifacts..."
mkdir -p "${OUTPUT_DIR}"

# Extract the installed files
podman create --name "${CONTAINER_NAME}" "${IMAGE_NAME}" /bin/true
podman cp "${CONTAINER_NAME}:/usr/bin/gamescope" "${OUTPUT_DIR}/" 2>/dev/null || true
podman cp "${CONTAINER_NAME}:/usr/lib64/" "${OUTPUT_DIR}/lib64/" 2>/dev/null || true
podman cp "${CONTAINER_NAME}:/usr/share/gamescope/" "${OUTPUT_DIR}/share/" 2>/dev/null || true
podman rm "${CONTAINER_NAME}"

echo ""
echo "[gamescope-bazzite] Build complete!"
echo "[gamescope-bazzite] Output in: ${OUTPUT_DIR}"
echo ""
echo "To use with gamescope-session-plus, set in ~/.config/gamescope-session-plus/sessions.d/steam:"
echo "  export GAMESCOPE_BIN=\"${OUTPUT_DIR}/gamescope\""

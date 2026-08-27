#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Jack Gu
#
# SPDX-License-Identifier: Apache-2.0
#
# Wraps the built app binary in a Matter OTA header, so an OTA provider can
# serve it. Every field is read from the build rather than passed in, so the
# image can never disagree with the firmware it contains.
#
#   ./tools/make_ota.sh [output.ota]

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="$here/build/stick_s3_light.bin"
config="$here/build/config/sdkconfig.h"

if [ ! -f "$bin" ] || [ ! -f "$config" ]; then
    echo "error: build first ('idf.py build') -- $bin is missing" >&2
    exit 1
fi

if [ -z "${ESP_MATTER_PATH:-}" ]; then
    echo "error: ESP_MATTER_PATH is unset; source esp-matter's export.sh" >&2
    exit 1
fi
tool="$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/src/app/ota_image_tool.py"

# The generated header is the authority: it is what the running firmware
# reports, so an image built from it always matches the device it targets.
cfg() { sed -n "s/^#define $1 \\(.*\\)$/\\1/p" "$config"; }

vendor=$(cfg CONFIG_DEVICE_VENDOR_ID)
product=$(cfg CONFIG_DEVICE_PRODUCT_ID)
version=$(cfg CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER)
# The version string Matter reports comes from esp_app_desc, i.e. version.txt.
version_str=$(head -n1 "$here/version.txt")

out="${1:-$here/build/stick_s3_light-$version_str.ota}"

echo "vendor  : $vendor"
echo "product : $product"
echo "version : $version ($version_str)"
echo "payload : $(stat -c%s "$bin") bytes"

python3 "$tool" create \
    -v "$vendor" -p "$product" \
    -vn "$version" -vs "$version_str" \
    -da sha256 \
    "$bin" "$out"

echo
echo "wrote $out"
python3 "$tool" show "$out"

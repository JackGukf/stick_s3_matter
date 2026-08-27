# SPDX-FileCopyrightText: 2026 Jack Gu
#
# SPDX-License-Identifier: Apache-2.0

"""Fail the build when the generated sdkconfig has drifted from sdkconfig.defaults.

ESP-IDF applies sdkconfig.defaults only when generating a fresh sdkconfig. Edit
the defaults with an sdkconfig already present and the change is silently
ignored -- the build succeeds and quietly carries the old value. That cost a
debugging session once: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE and the software
version both looked set and neither reached the firmware.

Symbols absent from sdkconfig entirely are skipped: those are unknown to this
target's Kconfig, which the build already warns about separately.
"""

import re
import sys

ASSIGN = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
UNSET = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")


def read_config(path):
    values = {}
    with open(path) as handle:
        for line in handle:
            line = line.rstrip("\n")
            assign = ASSIGN.match(line)
            if assign:
                values[assign.group(1)] = assign.group(2)
                continue
            unset = UNSET.match(line)
            if unset:
                values[unset.group(1)] = "n"
    return values


def main(defaults_paths, sdkconfig_path):
    actual = read_config(sdkconfig_path)
    drifted = []

    for defaults_path in defaults_paths:
        for symbol, wanted in read_config(defaults_path).items():
            # Not in sdkconfig at all -> unknown to this target's Kconfig.
            if symbol not in actual:
                continue
            got = actual[symbol]
            if got != wanted:
                drifted.append((symbol, wanted, got, defaults_path))

    if not drifted:
        return 0

    print("sdkconfig has drifted from the defaults:", file=sys.stderr)
    for symbol, wanted, got, source in drifted:
        print(f"  {symbol}: {source} wants {wanted!r}, sdkconfig has {got!r}", file=sys.stderr)
    print(
        "\nsdkconfig.defaults is only applied when sdkconfig is generated fresh, so\n"
        "editing it changes nothing on an existing build. Run:\n"
        "\n    rm sdkconfig && idf.py build\n"
        "\nIf a value was set deliberately via menuconfig, put it in sdkconfig.defaults\n"
        "instead -- it will not survive otherwise.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:-1], sys.argv[-1]))

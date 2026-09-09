#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
python3 tools/generate_version.py
python3 - <<'PYCHECK'
from pathlib import Path
import sys
sys.path.insert(0, 'tools')
from generate_config import render
assert Path('apps/opendisplay/src/config_data.h').read_text() == render(), 'Regenerate config first'
PYCHECK
build_harness() {
    cc -std=c99 -g -Wall -Wextra -Werror -Wno-unused-parameter \
        -Ilibs/od-uzlib/include -Iapps/opendisplay/src -Irepos/mbedtls/include -Irepos/mbedtls/library -Itests \
        '-DMBEDTLS_CONFIG_FILE="crypto_config.h"' \
        tests/harness.c apps/opendisplay/src/protocol.c apps/opendisplay/src/inflate.c libs/od-uzlib/src/od_zlib_stream.c \
        apps/opendisplay/src/storage.c apps/opendisplay/src/security.c \
        repos/mbedtls/library/aes.c repos/mbedtls/library/ccm.c repos/mbedtls/library/cmac.c \
        repos/mbedtls/library/cipher.c repos/mbedtls/library/cipher_wrap.c \
        repos/mbedtls/library/platform_util.c repos/mbedtls/library/constant_time.c \
        repos/mbedtls/library/block_cipher.c "$@"
}
build_harness -shared -fPIC -o "$out/harness.so"
OD_TEST_LIBRARY="$out/harness.so" python3 -m unittest discover -s tests -p 'test_*.py'
build_harness -fsanitize=address,undefined tests/test_protocol.c -o "$out/protocol"
"$out/protocol"
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
    -Ilibs/od-uzlib/include -Iapps/opendisplay/src tests/inflate_runner.c apps/opendisplay/src/inflate.c libs/od-uzlib/src/od_zlib_stream.c -o "$out/inflate"
python3 tests/check_inflate.py "$out/inflate"
mkdir -p "$out/syscfg"
python3 - "$out/syscfg/syscfg.h" <<'PYGPIO'
from pathlib import Path
import re, sys
settings = Path('apps/opendisplay/syscfg.yml').read_text()
pins = re.findall(r'    EPD_(\w+):\n        value: (\d+)', settings)
Path(sys.argv[1]).write_text('#define MYNEWT_VAL(x) MYNEWT_VAL_ ## x\n' + ''.join(f'#define MYNEWT_VAL_EPD_{key} {value}\n' for key,value in pins))
PYGPIO
cc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
    -I"$out" -Itests/fakes -Ilibs/od-uzlib/include -Iapps/opendisplay/src tests/test_epd.c apps/opendisplay/src/epd.c -o "$out/epd"
"$out/epd"
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itests/fakes -Irepos/nordic-nrfx/mdk -Iapps/opendisplay/src \
    tests/test_telemetry.c apps/opendisplay/src/telemetry.c -o "$out/telemetry"
"$out/telemetry"

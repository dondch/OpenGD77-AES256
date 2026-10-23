#!/bin/sh
# Build and run every host test in tests/. Run from MDUV380_firmware/. Exit status = number of
# failing tests. Each test's build line is the one in its own header; this just centralises them.
cd "$(dirname "$0")/.." || exit 1
OUT=${TMPDIR:-/tmp}/opengd77-tests
mkdir -p "$OUT"
CRYPTO="application/source/crypto/dmr_aes.c"
HOOK="application/source/crypto/dmr_aes_hook.c"
fails=0
run() {
    name=$1; shift
    if gcc -O2 -Wall -o "$OUT/$name" "$@" && "$OUT/$name" > "$OUT/$name.log" 2>&1; then
        echo "PASS $name: $(tail -1 "$OUT/$name.log")"
    else
        echo "FAIL $name (see $OUT/$name.log)"; tail -20 "$OUT/$name.log"; fails=$((fails + 1))
    fi
}
run test_dmr_aes      -Iapplication/include -Iapplication/include/crypto tests/test_dmr_aes.c $CRYPTO
run test_emb_sb       -Iapplication/include -Iapplication/include/crypto tests/test_emb_sb.c $CRYPTO
run test_le_mi        -Iapplication/include -Iapplication/include/crypto tests/test_le_mi.c $CRYPTO
run test_rx_bootstrap -DENABLE_AES -Itests/stubs -Iapplication/include tests/test_rx_bootstrap.c $HOOK $CRYPTO
run test_rx_bootstrap_diag -DENABLE_AES -DDMR_AES_DIAG_RX -Itests/stubs -Iapplication/include tests/test_rx_bootstrap.c $HOOK $CRYPTO
run test_rx_stale_pi  -DENABLE_AES -Itests/stubs -Iapplication/include tests/test_rx_stale_pi.c $HOOK $CRYPTO
run test_tx_state     -DENABLE_AES -Itests/stubs -Iapplication/include tests/test_tx_state.c $HOOK $CRYPTO
run test_sms_rx       -DENABLE_AES -DENABLE_DMR_DATA -Itests/stubs -Iapplication/include tests/test_sms_rx.c application/source/functions/dmr_sms.c $HOOK $CRYPTO
echo "$fails failing test program(s)"
exit $fails

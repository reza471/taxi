#!/usr/bin/env bash
# Does the meter's mbedTLS configuration talk to modern servers?
# 1. takes the REAL mbedTLS settings from a firmware build (the macros of one mbedTLS source file)
# 2. builds a small client with those settings on this PC
# 3. runs it against openssl servers that behave like typical production servers
#
#   BUILD_DIR=/tmp/taxi_build MBEDTLS_DIR=<ws>/modules/crypto/mbedtls zephyr-app/tests/tls/check_tls.sh
set -euo pipefail
BUILD_DIR="${BUILD_DIR:-/tmp/taxi_build}"
MBEDTLS_DIR="${MBEDTLS_DIR:?set MBEDTLS_DIR to modules/crypto/mbedtls of the Zephyr workspace}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-/tmp/tlscheck}"
mkdir -p "$OUT"

# --- 1. effective configuration of the firmware ---
# The Zephyr config file turns the CONFIG_ options of the build into MBEDTLS_ macros.
# Preprocess it with the build's autoconf.h: what is defined afterwards is what the chip gets.
ZCFG="$(dirname "$MBEDTLS_DIR")/../../zephyr/modules/mbedtls/configs"
[ -f "$ZCFG/config-mbedtls.h" ] || ZCFG="${ZEPHYR_BASE:?set ZEPHYR_BASE}/modules/mbedtls/configs"
AUTO="$BUILD_DIR/zephyr/include/generated/zephyr/autoconf.h"
[ -f "$AUTO" ] || { echo "no $AUTO (build the firmware first)"; exit 1; }
gcc -E -dM -x c -imacros "$AUTO" -I"$ZCFG" -I"$MBEDTLS_DIR/include" "$ZCFG/config-mbedtls.h" \
	| grep -E '^#define MBEDTLS_' > "$OUT/macros.txt"
{ echo '#ifndef HOSTCFG_H'; echo '#define HOSTCFG_H'
  # chip-only settings are left out, the rest is exactly the firmware configuration
  grep -vE 'MBEDTLS_(CONFIG_H|PLATFORM_EXIT_ALT|PLATFORM_MEMORY|MEMORY_BUFFER_ALLOC_C|MEMORY_ALIGN_MULTIPLE|NO_PLATFORM_ENTROPY|NO_DEFAULT_ENTROPY_SOURCES|HAVE_ASM)' "$OUT/macros.txt"
  grep -E '^#define CONFIG_MBEDTLS_(SSL_MAX_CONTENT_LEN|PSK_MAX_LEN)' "$AUTO"
  # what only the PC needs (sockets, entropy, error text). The TLS settings are NOT touched.
  echo '#define MBEDTLS_NET_C'; echo '#define MBEDTLS_ENTROPY_C'; echo '#define MBEDTLS_ERROR_C'
  echo '#endif'; } | awk '!seen[$0]++' > "$OUT/hostcfg.h"
echo "firmware TLS settings:"
for m in KEY_EXCHANGE_ECDHE_RSA_ENABLED KEY_EXCHANGE_ECDHE_ECDSA_ENABLED KEY_EXCHANGE_RSA_ENABLED GCM_C SHA384_C \
	 ECP_DP_SECP256R1_ENABLED ECP_DP_CURVE25519_ENABLED SSL_SERVER_NAME_INDICATION SSL_EXTENDED_MASTER_SECRET; do
	if grep -q "MBEDTLS_$m" "$OUT/macros.txt"; then printf '   %-40s yes\n' "$m"; else printf '   %-40s NO\n' "$m"; fi
done

# --- 2. client on the PC with the same settings ---
SRC=$(ls "$MBEDTLS_DIR"/library/*.c | grep -vE 'psa_|timing')
gcc -O1 -w -DMBEDTLS_CONFIG_FILE='"hostcfg.h"' -I"$OUT" -I"$MBEDTLS_DIR/include" -I"$MBEDTLS_DIR/library" \
    -o "$OUT/client" "$HERE/client.c" $SRC 2>"$OUT/build.err" || { echo "client build failed, see $OUT/build.err"; head -15 "$OUT/build.err"; exit 1; }


# --- 3. servers like production ones ---
cd "$OUT"
[ -f rsa.pem ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout rsa.key -out rsa.pem -days 30 -subj "/CN=test" 2>/dev/null
[ -f ec.pem ] || openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout ec.key -out ec.pem -days 30 -subj "/CN=test" 2>/dev/null
fail=0
run_case() { # name port cert key extra...
	local name="$1" port="$2" cert="$3" key="$4"; shift 4
	openssl s_server -accept "$port" -cert "$cert" -key "$key" -www -quiet "$@" >/dev/null 2>&1 &
	local pid=$!; sleep 0.7
	if out=$(timeout 15 ./client 127.0.0.1 "$port" 2>&1); then
		printf '  %-52s OK   %s\n' "$name" "$(echo "$out" | head -1)"
	else
		printf '  %-52s FAIL %s\n' "$name" "$(echo "$out" | head -1)"; fail=1
	fi
	kill $pid 2>/dev/null; wait $pid 2>/dev/null || true
}
echo "interop:"
run_case "openssl default, TLS1.2, RSA cert"                 4431 rsa.pem rsa.key -tls1_2
run_case "ECDHE-RSA-AES128-GCM only (typical Nginx)"         4432 rsa.pem rsa.key -tls1_2 -cipher ECDHE-RSA-AES128-GCM-SHA256
run_case "ECDHE-RSA-AES256-GCM-SHA384 only"                  4433 rsa.pem rsa.key -tls1_2 -cipher ECDHE-RSA-AES256-GCM-SHA384
run_case "ECDHE-ECDSA-AES128-GCM only (EC certificate)"      4434 ec.pem ec.key -tls1_2 -cipher ECDHE-ECDSA-AES128-GCM-SHA256
run_case "ECDHE-RSA-AES128-SHA256 (CBC)"                     4435 rsa.pem rsa.key -tls1_2 -cipher ECDHE-RSA-AES128-SHA256
run_case "server offers TLS1.2 + TLS1.3 (modern default)"    4436 rsa.pem rsa.key
run_case "x25519 key exchange preferred"                     4437 rsa.pem rsa.key -tls1_2 -groups X25519 -cipher ECDHE-RSA-AES128-GCM-SHA256
# SNI: the server must receive the host name (a CDN with several domains needs it)
sni_log=$(mktemp)
openssl s_server -accept 4438 -cert rsa.pem -key rsa.key -tls1_2 -cipher ECDHE-RSA-AES128-GCM-SHA256 -www -tlsextdebug > "$sni_log" 2>&1 &
pid=$!; sleep 0.7; timeout 15 ./client localhost 4438 >/dev/null 2>&1 || true; kill $pid 2>/dev/null; wait $pid 2>/dev/null || true
if grep -qi "server name" "$sni_log"; then printf '  %-52s OK   server received the name\n' "SNI is sent"; else printf '  %-52s FAIL no server_name extension\n' "SNI is sent"; fail=1; fi
rm -f "$sni_log"
exit $fail

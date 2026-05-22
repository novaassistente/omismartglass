#!/usr/bin/env bash
# pendant_upload_smoke.sh — pre-flight server contract validator.
#
# Purpose: prove the server at https://pendant.futuretools.today/upload accepts
# the HMAC contract documented in ~/.claude/MEMORY/PENDANT/hmac-contract.md
# from a non-firmware client (bash + openssl + curl). If smoke passes here,
# the pai_upload firmware just needs to produce the same bytes over the wire.
#
# Runs on the Linux box (Mac-free). Reads token from
# ~/.claude/integrations/pendant-ingest/.secrets/upload_token.bin (32 raw bytes).
#
# Output redacts HMAC signature in all echoed lines (so transcript never carries
# the secret). Only first 4 hex chars + last 4 hex chars are printed, never the
# full sig.
#
# Exit codes:
#   0   all gates passed
#   1   vector-1 self-test failed (openssl wrong or contract drift)
#   2   token file missing or wrong length
#   3   server 200 path failed
#   4   server 401 path (tampered HMAC) failed to actually 401
#   5   idempotency path failed

set -euo pipefail

ENDPOINT="${ENDPOINT:-https://pendant.futuretools.today/upload}"
TOKEN_PATH="${TOKEN_PATH:-$HOME/.claude/integrations/pendant-ingest/.secrets/upload_token.bin}"
SMOKE_UA="${SMOKE_UA:-PAI-Pendant-Smoke/1.0}"

redact_sig() {
    # Redacts a 64-hex-char string to "AAAA...ZZZZ (64 hex)".
    local sig="$1"
    if [[ ${#sig} -eq 64 ]]; then
        printf '%s...%s (64 hex redacted)' "${sig:0:4}" "${sig:60:4}"
    else
        printf '<malformed sig len=%d>' "${#sig}"
    fi
}

echo "=== Pendant upload smoke ==="
echo "endpoint: $ENDPOINT"
echo "ua:       $SMOKE_UA"
echo

# -----------------------------------------------------------------------------
# Gate 1: openssl + hmac-contract.md vector 1 parity check.
# -----------------------------------------------------------------------------
echo "[gate-1] openssl HMAC parity vs hmac-contract.md vector 1..."
VECTOR1_EXPECTED="4352B26E33FE0D769A8922A6BA29004109F01688E26ACC9E6CB347E5A5AFC4DA"
VECTOR1_GOT=$(printf 'hello' | openssl dgst -sha256 -mac HMAC -macopt hexkey:0000000000000000000000000000000000000000000000000000000000000000 -binary | xxd -p -c 64 | tr 'a-f' 'A-F')

if [[ "$VECTOR1_GOT" != "$VECTOR1_EXPECTED" ]]; then
    echo "  FAIL: openssl produced $(redact_sig "$VECTOR1_GOT"), expected $(redact_sig "$VECTOR1_EXPECTED")"
    echo "  Either openssl is broken or hmac-contract.md vector 1 is stale."
    exit 1
fi
echo "  OK — openssl matches contract vector 1: $(redact_sig "$VECTOR1_GOT")"
echo

# -----------------------------------------------------------------------------
# Gate 2: token file presence + length.
# -----------------------------------------------------------------------------
echo "[gate-2] token file presence + length..."
if [[ ! -f "$TOKEN_PATH" ]]; then
    echo "  FAIL: $TOKEN_PATH missing"
    exit 2
fi
TOKEN_LEN=$(stat -c%s "$TOKEN_PATH")
if [[ "$TOKEN_LEN" -ne 32 ]]; then
    echo "  FAIL: $TOKEN_PATH has $TOKEN_LEN bytes, expected 32"
    exit 2
fi
TOKEN_HEX=$(xxd -p -c 64 "$TOKEN_PATH")
echo "  OK — 32 raw bytes loaded (token hex redacted: $(redact_sig "$TOKEN_HEX"))"
echo

# -----------------------------------------------------------------------------
# Gate 3: 200 success path — valid HMAC, valid chunk_id, small body.
# -----------------------------------------------------------------------------
echo "[gate-3] POST /upload with valid HMAC..."
BODY_FILE=$(mktemp)
trap 'rm -f "$BODY_FILE"' EXIT
# 64 random bytes — plausible Opus frame size for smoke.
head -c 64 /dev/urandom > "$BODY_FILE"

# chunk_id = (now_ms << 14) | (PID & 0x3FFF)
NOW_MS=$(date +%s%3N)
PID_TAG=$((BASHPID & 0x3FFF))
CHUNK_ID=$(( (NOW_MS << 14) | PID_TAG ))

SIG=$(openssl dgst -sha256 -mac HMAC -macopt hexkey:"$TOKEN_HEX" -binary < "$BODY_FILE" | xxd -p -c 64 | tr 'a-f' 'A-F')

HTTP=$(curl -s -o /tmp/smoke_resp_3.json -w "%{http_code}" \
    -X POST "$ENDPOINT" \
    -H "User-Agent: $SMOKE_UA" \
    -H "Content-Type: application/octet-stream" \
    -H "X-Chunk-Id: $CHUNK_ID" \
    -H "X-HMAC-SHA256: $SIG" \
    --data-binary @"$BODY_FILE")

if [[ "$HTTP" != "200" ]]; then
    echo "  FAIL: expected 200, got $HTTP. Body:"
    cat /tmp/smoke_resp_3.json | head -2
    exit 3
fi
echo "  OK — 200 with chunk_id=$CHUNK_ID, sig=$(redact_sig "$SIG")"
cat /tmp/smoke_resp_3.json
echo

# -----------------------------------------------------------------------------
# Gate 4: 401 path — tampered HMAC must be rejected.
# -----------------------------------------------------------------------------
echo "[gate-4] POST /upload with tampered HMAC..."
# Deterministic tamper: flip the last hex nibble. Avoids the ~1/256
# false-negative that "${SIG:0:62}AA" produced when SIG already ended in AA
# (tampered == original → server returned 200, gate misclassified).
LAST_CHAR="${SIG: -1}"
case "$LAST_CHAR" in
    0) TAMPER_CHAR=1 ;; 1) TAMPER_CHAR=0 ;;
    2) TAMPER_CHAR=3 ;; 3) TAMPER_CHAR=2 ;;
    4) TAMPER_CHAR=5 ;; 5) TAMPER_CHAR=4 ;;
    6) TAMPER_CHAR=7 ;; 7) TAMPER_CHAR=6 ;;
    8) TAMPER_CHAR=9 ;; 9) TAMPER_CHAR=8 ;;
    A) TAMPER_CHAR=B ;; B) TAMPER_CHAR=A ;;
    C) TAMPER_CHAR=D ;; D) TAMPER_CHAR=C ;;
    E) TAMPER_CHAR=F ;; F) TAMPER_CHAR=E ;;
    *) TAMPER_CHAR=0 ;;  # shouldn't happen with uppercase hex output
esac
TAMPERED_SIG="${SIG:0:63}${TAMPER_CHAR}"
if [[ "$TAMPERED_SIG" == "$SIG" ]]; then
    echo "  FAIL: tamper produced identical sig (case logic bug)"
    exit 4
fi
HTTP=$(curl -s -o /tmp/smoke_resp_4.json -w "%{http_code}" \
    -X POST "$ENDPOINT" \
    -H "User-Agent: $SMOKE_UA" \
    -H "Content-Type: application/octet-stream" \
    -H "X-Chunk-Id: $((CHUNK_ID + 1))" \
    -H "X-HMAC-SHA256: $TAMPERED_SIG" \
    --data-binary @"$BODY_FILE")

if [[ "$HTTP" != "401" ]]; then
    echo "  FAIL: tampered HMAC should produce 401, got $HTTP"
    cat /tmp/smoke_resp_4.json | head -2
    exit 4
fi
echo "  OK — 401 hmac_mismatch on tampered sig $(redact_sig "$TAMPERED_SIG")"
cat /tmp/smoke_resp_4.json
echo

# -----------------------------------------------------------------------------
# Gate 5: idempotency — re-POSTing same chunk_id must return 200 + idempotent.
# -----------------------------------------------------------------------------
echo "[gate-5] re-POST same chunk_id (idempotency)..."
HTTP=$(curl -s -o /tmp/smoke_resp_5.json -w "%{http_code}" \
    -X POST "$ENDPOINT" \
    -H "User-Agent: $SMOKE_UA" \
    -H "Content-Type: application/octet-stream" \
    -H "X-Chunk-Id: $CHUNK_ID" \
    -H "X-HMAC-SHA256: $SIG" \
    --data-binary @"$BODY_FILE")

if [[ "$HTTP" != "200" ]]; then
    echo "  FAIL: expected 200 (idempotent), got $HTTP"
    cat /tmp/smoke_resp_5.json | head -2
    exit 5
fi
if ! grep -q '"idempotent":true\|"idempotent": true' /tmp/smoke_resp_5.json; then
    echo "  WARN: 200 but body did not assert idempotent=true. Body:"
    cat /tmp/smoke_resp_5.json
fi
echo "  OK — 200 (idempotent path)"
cat /tmp/smoke_resp_5.json
echo

# -----------------------------------------------------------------------------
echo "=== ALL GATES PASSED — server contract is what firmware will mirror ==="

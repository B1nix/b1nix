#!/bin/sh
# Generate a self-contained TLS test PKI for the b1nix loopback HTTPS smoke.
#
# Produces an EC P-256 CA and a server certificate (SAN IP:127.0.0.1,
# DNS:localhost) signed by it. EC keys keep the certificates small so the
# TLS handshake fits comfortably through the in-kernel loopback TCP path.
#
# The validity window is deliberately enormous (1970..2099) so the curl
# client accepts the chain regardless of the b1nix wall clock (which may be
# at the Unix epoch when NTP has not synced inside the test VM).
#
# Output (default build/tls-test/):
#   ca.pem          CA certificate (curl --cacert)
#   server-cert.pem leaf certificate presented by the loopback TLS server
#   server-key.pem  leaf private key

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="${1:-$ROOT_DIR/build/tls-test}"
OPENSSL="${OPENSSL:-openssl}"

mkdir -p "$OUT_DIR"

NOT_BEFORE="19700101000000Z"
NOT_AFTER="20991231235959Z"

# ── CA ──
"$OPENSSL" ecparam -name prime256v1 -genkey -noout -out "$OUT_DIR/ca-key.pem"
"$OPENSSL" req -x509 -new -nodes -key "$OUT_DIR/ca-key.pem" \
  -subj "/CN=b1nix-tls-test-ca" \
  -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
  -out "$OUT_DIR/ca.pem"

# ── Server leaf, signed by the CA ──
"$OPENSSL" ecparam -name prime256v1 -genkey -noout -out "$OUT_DIR/server-key.pem"

SAN_CNF="$OUT_DIR/server-san.cnf"
cat > "$SAN_CNF" <<'EOF'
[req]
distinguished_name = dn
[dn]
[v3_req]
subjectAltName = @alt
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
[alt]
IP.1 = 127.0.0.1
DNS.1 = localhost
EOF

"$OPENSSL" req -new -key "$OUT_DIR/server-key.pem" \
  -subj "/CN=127.0.0.1" -out "$OUT_DIR/server.csr"

"$OPENSSL" x509 -req -in "$OUT_DIR/server.csr" \
  -CA "$OUT_DIR/ca.pem" -CAkey "$OUT_DIR/ca-key.pem" -CAcreateserial \
  -not_before "$NOT_BEFORE" -not_after "$NOT_AFTER" \
  -extfile "$SAN_CNF" -extensions v3_req \
  -out "$OUT_DIR/server-cert.pem"

rm -f "$OUT_DIR/server.csr" "$SAN_CNF" "$OUT_DIR/ca.srl"

echo "$OUT_DIR"

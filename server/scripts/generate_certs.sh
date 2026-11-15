#!/usr/bin/env bash
set -e

# ==============================================================================
# Generate Self-Signed TLS Certificates for SecureLoRa HTTPS Gateway Connection
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CERTS_DIR="$(cd "${SCRIPT_DIR}/../certs" && pwd)"

mkdir -p "${CERTS_DIR}"
cd "${CERTS_DIR}"

# Determine IP address for SAN (Subject Alternative Name)
DETECTED_IP=$(ipconfig getifaddr en0 2>/dev/null || ip -4 addr show scope global | grep -oP '(?<=inet\s)\d+(\.\d+){3}' | head -n 1 || echo "127.0.0.1")
TARGET_IP="${1:-$DETECTED_IP}"

echo "======================================================================"
echo "Generating SecureLoRa SSL/TLS Certificates"
echo "Target IP for SAN: ${TARGET_IP}"
echo "Certificates directory: ${CERTS_DIR}"
echo "======================================================================"

# 1. Generate Root CA Key and Certificate
if [ ! -f ca.key ]; then
    echo "Creating Root CA..."
    openssl genrsa -out ca.key 4096
    openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
        -out ca.pem \
        -subj "/C=US/ST=California/L=Irvine/O=UCI SecureLoRa/CN=SecureLoRa Root CA"
fi

# 2. Generate Server Private Key
echo "Creating Server Private Key..."
openssl genrsa -out server.key 2048

# 3. Create OpenSSL configuration with Subject Alternative Names (SAN)
cat > server.ext <<EOF
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage = digitalSignature, nonRepudiation, keyEncipherment, dataEncipherment
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
IP.1 = 127.0.0.1
IP.2 = ${TARGET_IP}
EOF

# 4. Generate Certificate Signing Request (CSR)
openssl req -new -key server.key -out server.csr \
    -subj "/C=US/ST=California/L=Irvine/O=UCI SecureLoRa/CN=${TARGET_IP}"

# 5. Sign Server Certificate with Root CA
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
    -out server.crt -days 365 -sha256 -extfile server.ext

# Clean up temp files
rm -f server.csr server.ext

echo ""
echo "======================================================================"
echo "Certificates generated successfully!"
echo "Files created:"
echo "  - ca.pem        (Root CA certificate to send Tanishq for gateway/include/secrets.h)"
echo "  - ca.key        (Root CA private key - KEEP SECRET)"
echo "  - server.crt    (Server TLS certificate used by Nginx)"
echo "  - server.key    (Server TLS private key used by Nginx - KEEP SECRET)"
echo ""
echo "Instructions for Tanishq (gateway/include/secrets.h):"
echo "1. Set SERVER_URL: \"https://${TARGET_IP}:8443/api/readings\""
echo "2. Copy contents of certs/ca.pem into SERVER_ROOT_CA:"
echo "   static const char SERVER_ROOT_CA[] = R\"PEM("
cat ca.pem
echo "   )PEM\";"
echo "======================================================================"

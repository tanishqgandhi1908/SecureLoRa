#pragma once

// HOW TO USE: copy this file to include/secrets.h (same folder) and fill in real values.
// secrets.h is in .gitignore, so it never gets committed. NEVER commit real passwords or tokens.

#define WIFI_SSID      "YOUR_WIFI_NAME"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// Where the gateway POSTs readings (ask Sujith). Example: "https://example.com/api/readings"
// Plain "http://" works for a first bench test on your own laptop, but the final system must use https.
#define SERVER_URL     "http://192.168.1.50:8080/api/readings"

// Write-only token the server checks (sent as "Authorization: Bearer <token>"). Leave "" if the server has none yet.
#define API_TOKEN      ""

// Only needed for https:// URLs: the server's root CA certificate (PEM text), so the
// gateway can check it is talking to the real server. Ask Sujith for it.
static const char SERVER_ROOT_CA[] = R"PEM(
)PEM";

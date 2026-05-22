// =============================================================================
// pai_certs.h — Pendant Nova TLS trust anchors
// =============================================================================
// Pinned root CA(s) for *.futuretools.today via Cloudflare edge.
//
// As of 2026-05-22, pendant.futuretools.today is issued by:
//   leaf:          CN=futuretools.today
//   intermediate:  CN=WE1 (Google Trust Services)
//   root:          CN=GTS Root R4 (cross-signed by GlobalSign Root CA;
//                  cert presented in the TLS handshake, valid until 2028-01-28)
//
// Pinning the root cross-signed cert means: TLS handshake succeeds iff the
// server presents a chain anchored at this exact root. Replaces the prior
// WiFiClientSecure.setInsecure() call which was documented as a known
// follow-up (defense-in-depth — HMAC remains the actual auth at the body
// layer).
//
// Rotation policy:
//   * The cross-signed root cert below expires 2028-01-28. After that date
//     OR if Cloudflare rotates pendant.futuretools.today to a different CA
//     (Let's Encrypt, DigiCert, etc.), TLS handshakes will fail with
//     ESP_ERR_TLS_HANDSHAKE_FAILED and pai_upload will return RETRYABLE
//     forever — at which point a firmware OTA is required to ship a new
//     bundle. Maintenance window flagged in pai_storage::chunks_pending_count
//     stuck-high observability.
//
//   * To verify current cert: openssl s_client -showcerts -connect
//     pendant.futuretools.today:443 -servername pendant.futuretools.today
//
//   * To update: replace PAI_ROOT_CA_GTS_R4_PEM below with the new
//     cross-signed root PEM and re-flash.
//
// Security note: pinning a single CA is a single point of failure but
// also a tight trust boundary — no rogue CA (private fork, MITM via
// compromised public CA) can MITM this connection. Combined with the
// HMAC body authentication this gives belt-and-braces.
// =============================================================================

#ifndef PAI_CERTS_H
#define PAI_CERTS_H

namespace pai_certs
{

// GTS Root R4 cross-signed by GlobalSign Root CA. Valid 2023-11-15 →
// 2028-01-28. SHA-256 fingerprint:
//   83:9C:9D:F3:9E:CA:E5:E9:BB:33:D2:80:33:8E:73:1A:7D:AD:9C:DC:74:B2:08:80:BA:42:DD:55:48:F3:0E:5B
// (Verify with: openssl x509 -in cert.pem -noout -fingerprint -sha256)
extern const char ROOT_CA_PEM[];

} // namespace pai_certs

#endif // PAI_CERTS_H

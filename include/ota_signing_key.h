/**
 * @file ota_signing_key.h
 * @brief Embedded ECDSA-P256 Public Key for Cryptographic OTA Firmware Verification
 * @details To enforce cryptographic release signing in production builds:
 *          1. Generate an ECDSA P-256 key pair offline:
 *               openssl ecparam -name prime256v1 -genkey -noout -out ota_signing_priv.pem
 *               openssl ec -in ota_signing_priv.pem -pubout -out ota_signing_pub.pem
 *          2. Add the contents of ota_signing_priv.pem as the GitHub Actions repository secret
 *             `OTA_SIGNING_PRIVATE_KEY_PEM` (NEVER commit the private key).
 *          3. Paste the public PEM below into BREWLINK_OTA_SIGNING_PUBKEY_PEM.
 *             When non-empty, BrewLink strictly rejects any OTA binary lacking a valid
 *             ECDSA-P256-SHA256 signature (.sig release asset or X-Firmware-Signature header).
 */

#ifndef BREWLINK_OTA_SIGNING_KEY_H
#define BREWLINK_OTA_SIGNING_KEY_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BREWLINK_OTA_SIGNING_PUBKEY_PEM
#define BREWLINK_OTA_SIGNING_PUBKEY_PEM ""
#endif

#ifdef __cplusplus
}
#endif

#endif // BREWLINK_OTA_SIGNING_KEY_H

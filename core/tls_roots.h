#ifndef VJO_TLS_ROOTS_H
#define VJO_TLS_ROOTS_H

#include <stddef.h>

#include "bearssl.h"

/* GTS Root R1-R4 and GlobalSign Root CA: Google Lens (lensfrontend-pa.googleapis.com
 * chains to GTS Root R4, cross-signed by GlobalSign). For the HTTPS audio sources of
 * anki_audio_url, any host: ISRG Root X1/X2 (Let's Encrypt, also what jpdb.io and
 * api.jiten.moe used before those dictionaries were removed), Amazon Root CA 1,
 * DigiCert Global Root G2, USERTrust RSA/ECC (Sectigo), the SSL.com roots. */
extern const br_x509_trust_anchor *vjo_tls_roots;
extern const size_t vjo_tls_roots_num;

#endif

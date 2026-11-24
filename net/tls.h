/*
 * TLS 1.2 client over the stack's TCP connection, using BearSSL
 * (third_party/bearssl). Server certificates are validated against the
 * compiled-in trust anchors, the current time, and the requested host name.
 */
#ifndef CATO_NET_TLS_H
#define CATO_NET_TLS_H

#include <stdint.h>

#include "bearssl.h"
#include "net.h"

#define TLS_SEED_BYTES 48

struct tls_session {
    br_ssl_client_context client;
    br_x509_minimal_context x509;
    unsigned char buffer[BR_SSL_BUFSIZE_BIDI];
};

/*
 * Starts a handshake. days counts from January 1st of year 0 and seconds from
 * midnight, both in UTC. seed must come from a strong random source.
 * Returns 1 on success.
 */
int tls_begin(struct tls_session *tls, const char *host, uint32_t days, uint32_t seconds,
              const uint8_t *seed, int seed_length);
/* Moves records between BearSSL and TCP. Call regularly. */
void tls_pump(struct tls_session *tls, struct net *net);
/* True once the handshake has finished and application data can be sent. */
int tls_ready(const struct tls_session *tls);
/* True if the session has ended. tls_error() tells whether it failed. */
int tls_closed(const struct tls_session *tls);
int tls_error(const struct tls_session *tls);
const char *tls_error_text(int error);
/* Returns the TLS version and cipher suite numbers of the established session. */
void tls_describe(const struct tls_session *tls, int *version, int *cipher_suite);
int tls_write(struct tls_session *tls, struct net *net, const uint8_t *data, int length);
int tls_read(struct tls_session *tls, struct net *net, uint8_t *out, int capacity);
/* True when no more data will arrive. */
int tls_finished(const struct tls_session *tls, struct net *net);
void tls_close(struct tls_session *tls, struct net *net);

#endif

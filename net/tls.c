#include "tls.h"

#include "tls_anchors.h"

#define PUMP_ROUNDS 64

int tls_begin(struct tls_session *tls, const char *host, uint32_t days, uint32_t seconds,
              const uint8_t *seed, int seed_length)
{
    br_ssl_engine_context *engine = &tls->client.eng;

    if (seed_length < 32 || days == 0)
        return 0;

    br_ssl_client_init_full(&tls->client, &tls->x509, tls_trust_anchors, tls_trust_anchor_count);
    /* TLS 1.0 and 1.1 are obsolete, so only TLS 1.2 is offered. */
    br_ssl_engine_set_versions(engine, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_buffer(engine, tls->buffer, sizeof tls->buffer, 1);
    br_x509_minimal_set_time(&tls->x509, days, seconds);
    br_ssl_engine_inject_entropy(engine, seed, (size_t)seed_length);

    /* The host name is sent for SNI and checked against the server's certificate. */
    return br_ssl_client_reset(&tls->client, host, 0);
}

void tls_pump(struct tls_session *tls, struct net *net)
{
    br_ssl_engine_context *engine = &tls->client.eng;

    for (int round = 0; round < PUMP_ROUNDS; round++) {
        unsigned state = br_ssl_engine_current_state(engine);
        int progress = 0;
        if (state & BR_SSL_CLOSED)
            return;

        if (state & BR_SSL_SENDREC) {
            size_t length;
            unsigned char *data = br_ssl_engine_sendrec_buf(engine, &length);
            int sent = tcp_write(net, data, (int)length);
            if (sent > 0) {
                br_ssl_engine_sendrec_ack(engine, (size_t)sent);
                progress = 1;
            }
        }
        if (state & BR_SSL_RECVREC) {
            size_t length;
            unsigned char *data = br_ssl_engine_recvrec_buf(engine, &length);
            int received = tcp_read(net, data, (int)length);
            if (received > 0) {
                br_ssl_engine_recvrec_ack(engine, (size_t)received);
                progress = 1;
            }
        }
        if (!progress)
            return;
    }
}

int tls_ready(const struct tls_session *tls)
{
    return (br_ssl_engine_current_state(&tls->client.eng) & BR_SSL_SENDAPP) != 0;
}

int tls_closed(const struct tls_session *tls)
{
    return (br_ssl_engine_current_state(&tls->client.eng) & BR_SSL_CLOSED) != 0;
}

int tls_error(const struct tls_session *tls)
{
    return br_ssl_engine_last_error(&tls->client.eng);
}

const char *tls_error_text(int error)
{
    switch (error) {
    case BR_ERR_X509_EXPIRED:
        return "The site's certificate has expired or is not yet valid. Check the clock.";
    case BR_ERR_X509_BAD_SERVER_NAME:
        return "The site's certificate was issued for a different name.";
    case BR_ERR_X509_NOT_TRUSTED:
        return "The site's certificate is not from a trusted authority.";
    case BR_ERR_BAD_VERSION:
    case BR_ERR_UNSUPPORTED_VERSION:
        return "The site does not support TLS 1.2.";
    default:
        return "The secure connection failed.";
    }
}

void tls_describe(const struct tls_session *tls, int *version, int *cipher_suite)
{
    *version = tls->client.eng.session.version;
    *cipher_suite = tls->client.eng.session.cipher_suite;
}

int tls_write(struct tls_session *tls, struct net *net, const uint8_t *data, int length)
{
    br_ssl_engine_context *engine = &tls->client.eng;
    if (!(br_ssl_engine_current_state(engine) & BR_SSL_SENDAPP))
        return 0;

    size_t space;
    unsigned char *buffer = br_ssl_engine_sendapp_buf(engine, &space);
    int count = (size_t)length < space ? length : (int)space;
    for (int i = 0; i < count; i++)
        buffer[i] = data[i];
    br_ssl_engine_sendapp_ack(engine, (size_t)count);
    br_ssl_engine_flush(engine, 0);
    tls_pump(tls, net);
    return count;
}

int tls_read(struct tls_session *tls, struct net *net, uint8_t *out, int capacity)
{
    br_ssl_engine_context *engine = &tls->client.eng;
    tls_pump(tls, net);
    if (!(br_ssl_engine_current_state(engine) & BR_SSL_RECVAPP))
        return 0;

    size_t available;
    unsigned char *buffer = br_ssl_engine_recvapp_buf(engine, &available);
    int count = (size_t)capacity < available ? capacity : (int)available;
    for (int i = 0; i < count; i++)
        out[i] = buffer[i];
    br_ssl_engine_recvapp_ack(engine, (size_t)count);
    tls_pump(tls, net);
    return count;
}

int tls_finished(const struct tls_session *tls, struct net *net)
{
    unsigned state = br_ssl_engine_current_state(&tls->client.eng);
    if (state & BR_SSL_RECVAPP)
        return 0;
    /* Many servers close TCP without a TLS close_notify. HTTP framing catches truncation. */
    return (state & BR_SSL_CLOSED) || tcp_is_finished(net);
}

void tls_close(struct tls_session *tls, struct net *net)
{
    br_ssl_engine_close(&tls->client.eng);
    tls_pump(tls, net);
}

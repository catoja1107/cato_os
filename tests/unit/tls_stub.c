/*
 * Stand-in for net/tls.c in host unit tests, which do not link BearSSL. Secure
 * connections always fail here. HTTPS is tested end to end by tests/smoke.py.
 */
#include "tls.h"

int tls_begin(struct tls_session *tls, const char *host, uint32_t days, uint32_t seconds,
              const uint8_t *seed, int seed_length)
{
    (void)tls;
    (void)host;
    (void)days;
    (void)seconds;
    (void)seed;
    (void)seed_length;
    return 0;
}

void tls_pump(struct tls_session *tls, struct net *net)
{
    (void)tls;
    (void)net;
}

int tls_ready(const struct tls_session *tls)
{
    (void)tls;
    return 0;
}

int tls_closed(const struct tls_session *tls)
{
    (void)tls;
    return 1;
}

int tls_error(const struct tls_session *tls)
{
    (void)tls;
    return -1;
}

const char *tls_error_text(int error)
{
    (void)error;
    return "Secure connections are not available in unit tests.";
}

void tls_describe(const struct tls_session *tls, int *version, int *cipher_suite)
{
    (void)tls;
    *version = 0;
    *cipher_suite = 0;
}

int tls_write(struct tls_session *tls, struct net *net, const uint8_t *data, int length)
{
    (void)tls;
    (void)net;
    (void)data;
    (void)length;
    return 0;
}

int tls_read(struct tls_session *tls, struct net *net, uint8_t *out, int capacity)
{
    (void)tls;
    (void)net;
    (void)out;
    (void)capacity;
    return 0;
}

int tls_finished(const struct tls_session *tls, struct net *net)
{
    (void)tls;
    (void)net;
    return 1;
}

void tls_close(struct tls_session *tls, struct net *net)
{
    (void)tls;
    (void)net;
}

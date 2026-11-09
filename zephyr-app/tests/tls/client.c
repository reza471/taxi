/* Minimal TLS client built with the SAME mbedTLS settings as the meter firmware.
 * It does what net.c does: TLS 1.2 only, no certificate check, SNI = host name,
 * then a tiny HTTP GET. Prints the negotiated version and cipher suite.
 *   ./client <host> <port>
 * Exit 0 = handshake and HTTP answer OK, 1 = failed. */
#include <stdio.h>
#include <string.h>
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#if defined(MBEDTLS_MEMORY_BUFFER_ALLOC_C)
#include "mbedtls/memory_buffer_alloc.h"
/* the same fixed heap as the firmware (CONFIG_MBEDTLS_HEAP_SIZE): shows if the handshake fits */
static unsigned char heap[TAXI_HEAP_SIZE];
#endif

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: client host port\n"); return 2; }
	mbedtls_net_context net; mbedtls_ssl_context ssl; mbedtls_ssl_config conf;
	mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg;
	char err[200]; int r;
#if defined(MBEDTLS_MEMORY_BUFFER_ALLOC_C)
	mbedtls_memory_buffer_alloc_init(heap, sizeof(heap));
#endif

	mbedtls_net_init(&net); mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf);
	mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&drbg);
	if ((r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)"taxi", 4))) goto fail;
	if ((r = mbedtls_net_connect(&net, argv[1], argv[2], MBEDTLS_NET_PROTO_TCP))) goto fail;
	if ((r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
					     MBEDTLS_SSL_PRESET_DEFAULT))) goto fail;
	mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);          /* IPPROTO_TLS_1_2 */
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);                    /* TLS_PEER_VERIFY_NONE */
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	if ((r = mbedtls_ssl_setup(&ssl, &conf))) goto fail;
	if ((r = mbedtls_ssl_set_hostname(&ssl, argv[1]))) goto fail;
	mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, NULL);
	while ((r = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) goto fail;
	}
	printf("handshake OK: %s, %s\n", mbedtls_ssl_get_version(&ssl), mbedtls_ssl_get_ciphersuite(&ssl));
	char req[256];
	snprintf(req, sizeof(req), "GET /api/taxi/TaxiFare?driver_type=1&serial=TEST HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", argv[1]);
	if ((r = mbedtls_ssl_write(&ssl, (const unsigned char *)req, strlen(req))) < 0) goto fail;
	unsigned char buf[300]; r = mbedtls_ssl_read(&ssl, buf, sizeof(buf) - 1);
	if (r <= 0) goto fail;
	buf[r] = 0; char *eol = strchr((char *)buf, '\r'); if (eol) *eol = 0;
	printf("server says: %s\n", buf);
#if defined(MBEDTLS_MEMORY_DEBUG)
	size_t mu, mb;
	mbedtls_memory_buffer_alloc_max_get(&mu, &mb);
	printf("TLS heap peak: %zu of %d bytes\n", mu, TAXI_HEAP_SIZE);
#endif
	return 0;
fail:
	mbedtls_strerror(r, err, sizeof(err));
	printf("FAILED: -0x%04x %s\n", (unsigned)-r, err);
#if defined(MBEDTLS_MEMORY_DEBUG)
	{ size_t mu, mb; mbedtls_memory_buffer_alloc_max_get(&mu, &mb);
	  printf("TLS heap peak: %zu of %d bytes\n", mu, TAXI_HEAP_SIZE); }
#endif
	return 1;
}

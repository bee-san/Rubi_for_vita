/* The HTTP client's response framing (core/http.c, Anki's transport), which
 * decides whether a kept connection can carry the next request. Fake streams
 * split every boundary; no live service needed. */
#include "acutest.h"
#include "http.h"
#include "net.h"
#include <stdio.h>
#include <string.h>

static unsigned char memory[65536];
static VjoArena arena;
typedef struct {
    const char *data;
    size_t bytes, position, split;
} Stream;

static int receive(void *ctx, void *out, size_t bytes)
{
    Stream *s = ctx;
    if (bytes > s->split) bytes = s->split;
    if (bytes > s->bytes-s->position) bytes = s->bytes-s->position;
    memcpy(out, s->data+s->position, bytes);
    s->position += bytes;
    return (int)bytes;
}

static int read_response(const char *data, size_t bytes, size_t split,
                         size_t maximum, VjoHttpResponse *response)
{
    Stream stream = {data,bytes,0,split};
    VjoConn connection = {.ctx=&stream,.recv=receive};
    memset(memory,0xa5,sizeof(memory));
    vjo_arena_init(&arena,memory,sizeof(memory));
    return vjo_http_recv(&arena,&connection,maximum,response);
}

static void test_complete_framing_can_be_reused(void)
{
    const char *responses[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Length: 3 \t\r\nContent-Length: 3\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n2\r\nbc\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked \t\r\n\r\n3;one;two=token;three=\"a\\\"b\"\r\nabc\r\n0\r\nChecksum: ignored\r\n\r\n"
    };
    for (unsigned i=0; i<sizeof(responses)/sizeof(responses[0]); i++)
        for (size_t split=1; split<=2048; split*=8) {
            VjoHttpResponse response;
            TEST_CHECK(!read_response(responses[i],strlen(responses[i]),split,1024,&response));
            TEST_CHECK(response.status == 200 && response.keep_alive);
            TEST_CHECK(response.body_len == 3 && !memcmp(response.body,"abc",3));
        }
}

static void test_informational_response_then_final(void)
{
    const char *data="HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 103 Early Hints\r\nLink: </hint>\r\n\r\n"
                     "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    for (size_t split=1; split<=2048; split*=8) {
        VjoHttpResponse response;
        TEST_CHECK(!read_response(data,strlen(data),split,1024,&response));
        TEST_CHECK(response.status == 200 && response.body_len == 3 && response.keep_alive);
    }
}

static void test_close_and_unframed_responses_are_not_reused(void)
{
    const char *responses[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: keep-alive, CLOSE\r\n\r\nabc",
        "HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcunexpected"
    };
    for (unsigned i=0; i<sizeof(responses)/sizeof(responses[0]); i++) {
        VjoHttpResponse response;
        TEST_CHECK(!read_response(responses[i],strlen(responses[i]),2048,1024,&response));
        TEST_CHECK(response.status == 200 && !response.keep_alive);
    }
}

static void test_malformed_framing_never_reuses_stream(void)
{
    const char *responses[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 3garbage\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd",
        "HTTP/1.1 200 OK\r\nContent-Length: -3\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Length: 9999999999999999999999999999\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked-garbage\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nz\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3junk\r\nabc\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcXX0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3;\r\nabc\r\n0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3;x=\"unterminated\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000000000000000000000\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n", /* no trailer terminator */
        "HTTP/1.1 200 OK\nContent-Length: 3\n\nabc",
        "HTTP/1.1 200 OK\r\nContent-Length: 3", /* truncated header */
        "HTTP/1.1 200 OK\r\n Content-Length: 3\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nBad-Header\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nab", /* truncated body */
        "HTTP/1.1 101 Upgrade\r\n\r\n",
        "HTTP/1.1 100 Continue\r\nContent-Length: 0\r\n\r\n"
    };
    for (unsigned i=0; i<sizeof(responses)/sizeof(responses[0]); i++)
        for (size_t split=1; split<=2048; split*=8) {
            VjoHttpResponse response;
            TEST_CHECK_(read_response(responses[i],strlen(responses[i]),split,1024,&response) != VJO_OK,
                        "malformed response %u split=%zu",i,split);
            TEST_CHECK(!response.keep_alive);
        }
}

static void test_overlong_or_nul_header_is_rejected(void)
{
    char data[1024];
    size_t n=(size_t)sprintf(data,"HTTP/1.1 200 OK\r\nX-Long: ");
    memset(data+n,'x',600); n+=600;
    memcpy(data+n,"\r\nContent-Length: 0\r\n\r\n",23); n+=23;
    VjoHttpResponse response;
    TEST_CHECK(read_response(data,n,1,1024,&response) == VJO_E_HTTP && !response.keep_alive);
    const char nul[]="HTTP/1.1 200 OK\r\nContent-Length: 0\0hidden\r\n\r\n";
    TEST_CHECK(read_response(nul,sizeof(nul)-1,1,1024,&response) == VJO_E_HTTP && !response.keep_alive);
}

TEST_LIST = {
    {"complete_framing_can_be_reused",test_complete_framing_can_be_reused},
    {"informational_response_then_final",test_informational_response_then_final},
    {"close_and_unframed_responses_are_not_reused",test_close_and_unframed_responses_are_not_reused},
    {"malformed_framing_never_reuses_stream",test_malformed_framing_never_reuses_stream},
    {"overlong_or_nul_header_is_rejected",test_overlong_or_nul_header_is_rejected},
    {NULL,NULL}
};

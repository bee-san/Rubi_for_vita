/* Host tests only (include/vjo_vocr_sizes.h VJO_VOCR_SPOT_TEST): the spotter's pins are the committed random VTSP
 * version-4 blob's (host/fixtures/spotter/README.md), so a job's spotter runs end to end where no published spotter
 * exists. The rest of include/vjo_vocr_sizes.h is the tree's own. */
#define VJO_VOCR_SPOT_TAG "fixture"
#define VJO_VOCR_SPOT_NAME "textspot6_tiny_s1.vtsp"
#define VJO_VOCR_SPOT_BYTES 3256u
#define VJO_VOCR_SPOT_SHA256 "77b20d636a34e04744e8d3d81d81a64c485dc323af6c7c92ce649a88fe38972b"
#define VJO_VOCR_SPOT_ATTRIBUTION_BYTES 0u
#define VJO_VOCR_SPOT_ATTRIBUTION_SHA256 "0000000000000000000000000000000000000000000000000000000000000000"
#define VJO_VOCR_SPOT_FORM "trunk"
#define VJO_VOCR_SPOT_PUBLISHED 0
/* its arena for a 960 x 544 frame read at its scale (vocr_textspot6_arena_bytes; test_vocr_spot.c checks it) */
#define VJO_VOCR_SPOT_ARENA 32759u
#define VJO_VOCR_SPOT_POST 0x0Cu

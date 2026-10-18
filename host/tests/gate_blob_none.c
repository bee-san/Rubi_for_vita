/* No text-presence gate weights (core/vocr_ocr.h vjo_vocr_gate_blob): for the test builds that list their
 * sources by hand (tools/arm_host_tests.sh's capture parity) and run no job with the gate. A blob of 0 bytes
 * never loads, so a job asking for the gate would read as without it (stats.gate_rc). The CMake builds use
 * the pinned weights (cmake/VocrGate.cmake). */
#include <stddef.h>

unsigned char vjo_vocr_gate_blob[1];
const size_t vjo_vocr_gate_blob_bytes = 0;

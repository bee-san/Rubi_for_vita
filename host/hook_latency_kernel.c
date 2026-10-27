/* The real kernel transport for host/hook_latency_bench.c, its syscalls renamed (as
 * host/tests/kernel_model_broker_boundary.c does, with API 41's vjoOcrHookLine too) so the bench's
 * wrappers can time them. Test-only: the native exports and NIDs are unchanged. */
#define vjoOcrRegister lat_kernel_register
#define vjoOcrSubmit lat_kernel_submit
#define vjoOcrTake lat_kernel_take
#define vjoOcrCaptureRows lat_kernel_capture_rows
#define vjoOcrCancelled lat_kernel_cancelled
#define vjoOcrComplete lat_kernel_complete
#define vjoOcrRead lat_kernel_read
#define vjoOcrCancel lat_kernel_cancel
#define vjoOcrModelOpen lat_kernel_model_open
#define vjoOcrModelRead lat_kernel_model_read
#define vjoOcrModelClose lat_kernel_model_close
#define vjoOcrShotWrite lat_kernel_shot_write
#define vjoOcrShotSave lat_kernel_shot_save
#define vjoOcrHookCache lat_kernel_hook_cache
#define vjoOcrHookLine lat_kernel_hook_line
#include "../kernel/game_ocr.c"

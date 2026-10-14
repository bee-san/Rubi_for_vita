/* Keep the real kernel implementations behind the host's retail-return model.
 * Renaming is test-only: native exports and library NIDs remain unchanged. */
#define vjoOcrRegister test_kernel_register
#define vjoOcrSubmit test_kernel_submit
#define vjoOcrTake test_kernel_take
#define vjoOcrCaptureRows test_kernel_capture_rows
#define vjoOcrCancelled test_kernel_cancelled
#define vjoOcrComplete test_kernel_complete
#define vjoOcrRead test_kernel_read
#define vjoOcrCancel test_kernel_cancel
#define vjoOcrModelOpen test_kernel_model_open
#define vjoOcrModelRead test_kernel_model_read
#define vjoOcrModelClose test_kernel_model_close
#include "../../kernel/game_ocr.c"

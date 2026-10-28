/* The Rubi app's lines in log.txt, "Rubi app: " and the text (settings/applog.c, below the Shell's 1 MiB rotation):
 * written while config.ini's log_file is on, which main.c sets at start and the Anki page again when it is shown.
 * Between vjo_app_log_batch_begin and _end, in one function of one thread, that thread's lines go out in one append,
 * at the end. */
#ifndef VJO_SETTINGS_APPLOG_H
#define VJO_SETTINGS_APPLOG_H

void vjo_log_enable(int on);
void vjo_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vjo_app_log_batch_begin(void);
void vjo_app_log_batch_end(void);

#endif

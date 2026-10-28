/* A modest readability increase for app labels and definitions, using the same size to measure and draw. */
#ifndef VJO_SETTINGS_READABILITY_H
#define VJO_SETTINGS_READABILITY_H
static inline float vjo_app_text_pixels(float pixels) { return pixels <= 24 ? pixels + 2 : pixels; }
#endif

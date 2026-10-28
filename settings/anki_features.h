#ifndef VJO_SETTINGS_ANKI_FEATURES_H
#define VJO_SETTINGS_ANKI_FEATURES_H
/* Retained for the complete duplicate-detection workflow in #521. Shipping builds expose no import action and
 * do not consult an old known-word index at export. Parser/index tests can opt in to exercise the retained code. */
#ifndef VJO_KNOWN_WORDS_ENABLED
#define VJO_KNOWN_WORDS_ENABLED 0
#endif
#endif

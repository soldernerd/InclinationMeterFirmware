#ifndef APP_VERSION_H
#define APP_VERSION_H

/* Bump FW_VERSION_PATCH on every firmware build handed over for flashing —
 * the display shows FW_VERSION_STRING, so a changed patch number is the
 * quick visual confirmation that the new image is actually running.
 * FW_VERSION_STRING is derived from the three numbers below, so there is
 * only one place to edit. */
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  2
#define FW_VERSION_PATCH  7

#define FW_VERSION_STR2(x)  #x
#define FW_VERSION_STR(x)   FW_VERSION_STR2(x)
#define FW_VERSION_STRING \
    FW_VERSION_STR(FW_VERSION_MAJOR) "." \
    FW_VERSION_STR(FW_VERSION_MINOR) "." \
    FW_VERSION_STR(FW_VERSION_PATCH)

#endif /* APP_VERSION_H */

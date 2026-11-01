/* Minimal host config for building libmodbus 3.1.10 sources directly (no autotools).
 * HAVE_STRLCPY intentionally undefined so libmodbus uses its bundled strlcpy. */
#define HAVE_DECL_TIOCM_RTS 0
#define HAVE_DECL_TIOCSRS485 0

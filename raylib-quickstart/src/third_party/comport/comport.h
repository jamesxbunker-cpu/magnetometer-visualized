/* inc/comport.h - public API for direct COM port access */
#ifndef COMPORT_H
#define COMPORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pass this as a timeout to block forever. */
#define COMPORT_WAIT_INFINITE  ((unsigned)-1)

/* Opaque handle. Callers never poke inside. */
typedef struct comport comport_t;

/* Open a COM port (e.g. "COM3", "COM12") at the given baud, 8N1,
 * no flow control.
 *
 *   port: e.g. "COM3". Do NOT include the "\\\\.\\" prefix; the library
 *         adds it. Names are case-insensitive.
 *   baud: standard rates (9600, 115200, etc.). The Windows driver rounds
 *         to the nearest supported rate.
 *
 * Returns a handle on success, NULL on failure (with a message on stderr).
 */
comport_t *comport_open(const char *port, unsigned baud);

/* Read up to len bytes into buf.
 *
 *   timeout_ms: how long to wait for data.
 *               0                      -> non-blocking, returns immediately
 *               COMPORT_WAIT_INFINITE  -> block until at least one byte
 *               other                  -> wait up to this many ms
 *
 * Returns:
 *   > 0   number of bytes copied into buf
 *     0   timeout (no data)
 *    -1   port lost or unrecoverable error (handle should be closed)
 *
 * Non-fatal line errors (overrun, framing, parity) are handled internally
 * and printed to stderr; the read continues.
 */
int comport_read(comport_t *c, void *buf, size_t len, unsigned timeout_ms);

/* Write len bytes from buf to the port.
 *
 * Returns:
 *   > 0   number of bytes written
 *    -1   write failed
 */
int comport_write(comport_t *c, const void *buf, size_t len);

/* Close and free the handle. Safe to call with NULL. */
void comport_close(comport_t *c);

/* Return the port name this handle was opened with. Read-only, valid
 * until comport_close. May return NULL if c is NULL. */
const char *comport_name(const comport_t *c);

#ifdef __cplusplus
}
#endif

#endif /* COMPORT_H */
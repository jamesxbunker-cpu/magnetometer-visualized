/* inc/serial.h */
#ifndef SERIAL_H
#define SERIAL_H

#include <windows.h>
#include <stddef.h>

typedef struct serial_port serial_port_t;

/* Open COM port 8N1, no flow control. NULL on failure. */
serial_port_t *serial_open(const char *port, DWORD baud);

/* Close and free. Safe with NULL. */
void serial_close(serial_port_t *sp);

/* Read up to len bytes into buf. Blocks up to timeout_ms.
 * Returns bytes read (may be 0 on timeout), or -1 on error. */
int serial_read(serial_port_t *sp, void *buf, size_t len, DWORD timeout_ms);
/* Write len bytes from buf. Returns bytes written or -1 on error. */
int serial_write(serial_port_t *sp, const void *buf, size_t len);
#endif
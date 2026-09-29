/* src/comport.c - public library wrapping serial.c */
#include "comport.h"
#include "serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct comport {
    serial_port_t *sp;
    char           port_name[64];
};

comport_t *comport_open(const char *port, unsigned baud) {
    if (!port || !*port) {
        fprintf(stderr, "comport_open: empty port name\n");
        return NULL;
    }

    serial_port_t *sp = serial_open(port, (DWORD)baud);
    if (!sp) return NULL;

    comport_t *c = calloc(1, sizeof(*c));
    if (!c) {
        serial_close(sp);
        return NULL;
    }

    c->sp = sp;
    strncpy(c->port_name, port, sizeof(c->port_name) - 1);
    c->port_name[sizeof(c->port_name) - 1] = '\0';
    return c;
}

int comport_read(comport_t *c, void *buf, size_t len, unsigned timeout_ms) {
    if (!c || !c->sp) return -1;

    DWORD win_timeout = (timeout_ms == COMPORT_WAIT_INFINITE)
                            ? INFINITE
                            : (DWORD)timeout_ms;

    /* serial_read expects a DWORD timeout; 0 means "return immediately". */
    return serial_read(c->sp, buf, len, win_timeout);
}

int comport_write(comport_t *c, const void *buf, size_t len) {
    if (!c || !c->sp) return -1;
    return serial_write(c->sp, buf, len);
}

void comport_close(comport_t *c) {
    if (!c) return;
    if (c->sp) serial_close(c->sp);
    free(c);
}

const char *comport_name(const comport_t *c) {
    return c ? c->port_name : NULL;
}
/* src/serial.c */
#include "serial.h"
#include <stdio.h>
#include <stdlib.h>

struct serial_port {
    HANDLE handle;
    OVERLAPPED read_ov; // Async reads
    HANDLE read_event; // signal for when read is finished
    OVERLAPPED comm_ov; // for WaitCommEvent
    HANDLE comm_event; // signaled when comm event fires
    DWORD comm_mask; // events we are waiting on
    char   name[64];
};

/* Arm WaitCommEvent on EV_RXCHAR (data arrived) plus error/break events.
 * This is called once at open, then re-armed after every event fires. */
static void arm_comm_event(serial_port_t *sp) {
    sp->comm_mask = EV_RXCHAR | EV_ERR | EV_BREAK;
    ResetEvent(sp->comm_event);
    BOOL ok = WaitCommEvent(sp->handle, &sp->comm_mask, &sp->comm_ov);
    if (!ok) {
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            fprintf(stderr, "WaitCommEvent failed: %lu\n", e);
        }
    }
    /* ERROR_IO_PENDING is the normal case - the wait is now in flight. */
}

serial_port_t *serial_open(const char *port, DWORD baud) {
    if (!port || !*port) return NULL;

    serial_port_t *sp = calloc(1, sizeof(*sp));
    if (!sp) return NULL;

    snprintf(sp->name, sizeof(sp->name), "\\\\.\\%s", port);

    sp->handle = CreateFileA(
        sp->name,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,                      /* asynchronous */
        NULL);

    if (sp->handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateFile(%s) failed: %lu\n",
                sp->name, GetLastError());
        free(sp);
        return NULL;
    }

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(sp->handle, &dcb)) {
        fprintf(stderr, "GetCommState failed: %lu\n", GetLastError());
        CloseHandle(sp->handle);
        free(sp);
        return NULL;
    }

    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl  = DTR_CONTROL_ENABLE;
    dcb.fRtsControl  = RTS_CONTROL_ENABLE;
    dcb.fOutX = dcb.fInX = FALSE;

    if (!SetCommState(sp->handle, &dcb)) {
        fprintf(stderr, "SetCommState failed: %lu\n", GetLastError());
        CloseHandle(sp->handle);
        free(sp);
        return NULL;
    }

    COMMTIMEOUTS to = {0};
    to.ReadIntervalTimeout        = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant   = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 0;
    SetCommTimeouts(sp->handle, &to);

    SetupComm(sp->handle, 4096, 4096);
    PurgeComm(sp->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    /* Tell the driver which events we care about. */
    if (!SetCommMask(sp->handle, EV_RXCHAR | EV_ERR | EV_BREAK)) {
        fprintf(stderr, "SetCommMask failed: %lu\n", GetLastError());
        CloseHandle(sp->handle);
        free(sp);
        return NULL;
    }

    // OS signal when read event finishes
    // manual reset so only cleared after seen by program
    sp->read_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!sp->read_event) {
        fprintf(stderr, "CreateEvent(read) failed: %lu\n", GetLastError());
        CloseHandle(sp->handle);
        free(sp);
        return NULL;
    }

    sp->read_ov.hEvent = sp->read_event;

    // comm event - fires when data arrives / break / line error
    sp->comm_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!sp->comm_event) {
        fprintf(stderr, "CreateEvent(comm) failed: %lu\n", GetLastError());
        CloseHandle(sp->read_event);
        CloseHandle(sp->handle);
        free(sp);
        return NULL;
    }

    sp->comm_ov.hEvent = sp->comm_event;

    PurgeComm(sp->handle, PURGE_RXCLEAR | PURGE_TXCLEAR);

    // prime the first comm-event wait
    arm_comm_event(sp);

    return sp;
}

/* Clear any pending driver-level errors and report them.
 * Returns the COMSTAT snapshot so callers can act on it if needed. */
static void recover_errors(serial_port_t *sp) {
    DWORD errors = 0;
    COMSTAT stat = {0};
    if (!ClearCommError(sp->handle, &errors, &stat)) {
        fprintf(stderr, "ClearCommError failed: %lu\n", GetLastError());
        return;
    }
    if (errors & CE_RXOVER){
        fprintf(stderr, "[serial] RX overrun\n");
    }
    if (errors & CE_OVERRUN){
        fprintf(stderr, "[serial] overrun\n");}
    if (errors & CE_FRAME){
        fprintf(stderr, "[serial] framing error\n");
    }
    if (errors & CE_RXPARITY){
        fprintf(stderr, "[serial] parity error\n");
    }
    /* CE_BREAK intentionally not logged - too noisy on virtual ports. */
}

void serial_close(serial_port_t *sp) {
    if (!sp) return;
    fprintf(stderr, "[serial] serial_close called (handle=%p)\n",
        (void*)sp->handle); 
    if (sp->handle != INVALID_HANDLE_VALUE){
        CancelIo(sp->handle); // cancel pending reads and comm wait
        CloseHandle(sp->handle);
    }
    if (sp->read_event){
        CloseHandle(sp->read_event);
    }
    if (sp->comm_event){
        CloseHandle(sp->comm_event);
    }
    free(sp);
}

int serial_read(serial_port_t *sp, void *buf, size_t len, DWORD timeout_ms) {
    if (!sp || !buf || len == 0) {
        return -1;
    }

    /* 1. Wait for the driver to signal "data arrived" (or error/break).
     *    No spin - if nothing happens within timeout_ms, return 0. */
    DWORD w = WaitForSingleObject(sp->comm_event, timeout_ms);
    if (w == WAIT_TIMEOUT) {
        return 0;
    }
    if (w != WAIT_OBJECT_0) {
        fprintf(stderr, "WaitForSingleObject(comm) failed: %lu\n", GetLastError());
        return -1;
    }

    /* 2. Re-arm immediately so the next byte isn't missed while we read. */
    arm_comm_event(sp);

    /* 3. Handle error / break events. */
    if (sp->comm_mask & EV_ERR) {
        recover_errors(sp);
    }
    if (sp->comm_mask & EV_BREAK) {
        fprintf(stderr, "[serial] break\n");
    }

    /* 4. Data should be available now. Read it. */
    ResetEvent(sp->read_event);
    DWORD got = 0;
    BOOL ok = ReadFile(sp->handle, buf, (DWORD)len, &got, &sp->read_ov);

    if (ok) {
        return (int)got;
    }

    DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) {
        fprintf(stderr, "ReadFile failed: %lu\n", err);
        recover_errors(sp);
        return -1;
    }

    w = WaitForSingleObject(sp->read_event, timeout_ms);
    if (w == WAIT_TIMEOUT) {
        CancelIo(sp->handle);
        return 0;
    }
    if (w != WAIT_OBJECT_0) {
        fprintf(stderr, "WaitForSingleObject(read) failed: %lu\n", GetLastError());
        return -1;
    }
    if (!GetOverlappedResult(sp->handle, &sp->read_ov, &got, FALSE)) {
        DWORD e = GetLastError();
        if (e == ERROR_OPERATION_ABORTED) return 0;
        fprintf(stderr, "GetOverlappedResult failed: %lu\n", e);
        recover_errors(sp);
        return -1;
    }
    return (int)got;
}

int serial_write(serial_port_t *sp, const void *buf, size_t len) {
    if (!sp || !buf || len == 0) return -1;

    DWORD written = 0;
    if (!WriteFile(sp->handle, buf, (DWORD)len, &written, NULL)) {
        fprintf(stderr, "WriteFile failed: %lu\n", GetLastError());
        return -1;
    }
    return (int)written;
}
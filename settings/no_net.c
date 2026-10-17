/* The Rubi app opens no socket: the overlay is offline. VitaSDK's newlib still
 * calls three SceNet functions for a socket descriptor (_read_r and _write_r in
 * syscalls.o, __vita_fd_drop in io.o, which crt0's _init_vita_io pulls in), so
 * without these the app would import SceNet for code that never runs: no socket
 * descriptor can exist, as newlib's socket() is not linked. Defined here, they
 * fail as for a bad descriptor, and tools/check_offline_imports.py checks that
 * the app imports no network library. */
#include <psp2/net/net.h>

int sceNetRecv(int s, void *buf, unsigned int len, int flags)
{
    (void)s; (void)buf; (void)len; (void)flags;
    return SCE_NET_ERROR_EBADF;
}

int sceNetSend(int s, const void *msg, unsigned int len, int flags)
{
    (void)s; (void)msg; (void)len; (void)flags;
    return SCE_NET_ERROR_EBADF;
}

int sceNetSocketClose(int s)
{
    (void)s;
    return SCE_NET_ERROR_EBADF;
}

/*
 * watchdog.c - Hardware-Watchdog ueber die Linux-Watchdog-Schnittstelle
 * (Treiber bcm2835_wdt, siehe watchdog.h).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Fable 5.2),
 * geprueft von: <Name>
 */
#include "watchdog.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/watchdog.h>

#include "pi2_config.h"
#include "types.h"

#define WATCHDOG_FD_CLOSED (-1)

/* Wird in main geoeffnet/geschlossen und vom Safety-Logik-Thread bedient. */
static _Atomic int32_t g_fd = WATCHDOG_FD_CLOSED;

int32_t watchdog_open(void)
{
    int32_t result = -1;
    int32_t fd = open(WATCHDOG_DEVICE, O_WRONLY | O_CLOEXEC);

    if (fd >= 0) {
        int32_t timeout_s = (int32_t)WATCHDOG_TIMEOUT_S;

        /* Der Treiber darf den Wert anpassen; akzeptiert wird nur der geforderte. */
        if ((ioctl(fd, WDIOC_SETTIMEOUT, &timeout_s) == 0) &&
            (timeout_s == (int32_t)WATCHDOG_TIMEOUT_S) &&
            (ioctl(fd, WDIOC_KEEPALIVE, 0) == 0)) {
            atomic_store(&g_fd, fd);
            result = 0;
        } else {
            /* Oeffnen hat den Watchdog bereits gestartet: wieder anhalten.
             * Gelingt das nicht, setzt er den Pi zurueck (sichere Seite); der
             * Start wird in jedem Fall abgebrochen. */
            static const char_t magic = 'V';
            bool stopped = (write(fd, &magic, sizeof(magic)) == (ssize_t)sizeof(magic));

            if ((close(fd) != 0) || !stopped) {
                result = -1;
            }
        }
    }
    return result;
}

bool watchdog_is_open(void)
{
    return atomic_load(&g_fd) != WATCHDOG_FD_CLOSED;
}

int32_t watchdog_kick(void)
{
    int32_t result = -1;
    int32_t fd = atomic_load(&g_fd);

    if ((fd != WATCHDOG_FD_CLOSED) && (ioctl(fd, WDIOC_KEEPALIVE, 0) == 0)) {
        result = 0;
    }
    return result;
}

int32_t watchdog_close(void)
{
    int32_t result = 0;
    int32_t fd = atomic_exchange(&g_fd, WATCHDOG_FD_CLOSED);

    if (fd != WATCHDOG_FD_CLOSED) {
        static const char_t magic = 'V';

        /* Ohne das Zeichen 'V' vor dem Schliessen liefe der Watchdog weiter. */
        if (write(fd, &magic, sizeof(magic)) != (ssize_t)sizeof(magic)) {
            result = -1;
        }
        if (close(fd) != 0) {
            result = -1;
        }
    }
    return result;
}

/*
 * shared.c - Gemeinsame Datensaetze von Pi 1, je ein Mutex pro Datensatz.
 * Beschreibung und Regeln siehe shared.h.
 *
 * Schlaegt pthread_mutex_lock fehl (nur bei Programmierfehlern moeglich),
 * wird nicht kopiert: Ein Leser behaelt dann seinen Startwert, der so
 * gewaehlt ist, dass nichts passiert (keine Stufe, keine Verbindung).
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "shared.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_operator_mutex;
static pthread_mutex_t g_ctrl_mutex;
static pthread_mutex_t g_link_mutex;

static operator_cmd_t g_operator;
static app_ctrl_t g_ctrl;
static link_status_t g_link;

int32_t shared_init(void)
{
    int32_t result = -1;

    (void)memset(&g_operator, 0, sizeof(g_operator));
    (void)memset(&g_ctrl, 0, sizeof(g_ctrl));
    (void)memset(&g_link, 0, sizeof(g_link));

    if (pthread_mutex_init(&g_operator_mutex, NULL) == 0) {
        if (pthread_mutex_init(&g_ctrl_mutex, NULL) == 0) {
            if (pthread_mutex_init(&g_link_mutex, NULL) == 0) {
                result = 0;
            } else {
                (void)pthread_mutex_destroy(&g_ctrl_mutex);
                (void)pthread_mutex_destroy(&g_operator_mutex);
            }
        } else {
            (void)pthread_mutex_destroy(&g_operator_mutex);
        }
    }
    return result;
}

void shared_destroy(void)
{
    /* Nur nach dem Ende aller Threads aufrufen. */
    (void)pthread_mutex_destroy(&g_link_mutex);
    (void)pthread_mutex_destroy(&g_ctrl_mutex);
    (void)pthread_mutex_destroy(&g_operator_mutex);
}

/* --- operator_cmd: schreibt nur der Eingabe-Thread ------------------------- */

void shared_operator_write(const operator_cmd_t *cmd)
{
    if ((cmd != NULL) && (pthread_mutex_lock(&g_operator_mutex) == 0)) {
        g_operator = *cmd;
        (void)pthread_mutex_unlock(&g_operator_mutex);
    }
}

void shared_operator_read(operator_cmd_t *cmd)
{
    if ((cmd != NULL) && (pthread_mutex_lock(&g_operator_mutex) == 0)) {
        *cmd = g_operator;
        (void)pthread_mutex_unlock(&g_operator_mutex);
    }
}

/* --- app_ctrl: schreibt nur main --------------------------------------------- */

void shared_ctrl_write(const app_ctrl_t *ctrl)
{
    if ((ctrl != NULL) && (pthread_mutex_lock(&g_ctrl_mutex) == 0)) {
        g_ctrl = *ctrl;
        (void)pthread_mutex_unlock(&g_ctrl_mutex);
    }
}

void shared_ctrl_read(app_ctrl_t *ctrl)
{
    if ((ctrl != NULL) && (pthread_mutex_lock(&g_ctrl_mutex) == 0)) {
        *ctrl = g_ctrl;
        (void)pthread_mutex_unlock(&g_ctrl_mutex);
    }
}

/* --- link_status: schreibt nur der Kommunikations-Thread ---------------------- */

void shared_link_write(const link_status_t *link)
{
    if ((link != NULL) && (pthread_mutex_lock(&g_link_mutex) == 0)) {
        g_link = *link;
        (void)pthread_mutex_unlock(&g_link_mutex);
    }
}

void shared_link_read(link_status_t *link)
{
    if ((link != NULL) && (pthread_mutex_lock(&g_link_mutex) == 0)) {
        *link = g_link;
        (void)pthread_mutex_unlock(&g_link_mutex);
    }
}

/*
 * names.c - Klartextnamen fuer Zustand, Fehlercode und Stufe.
 *
 * Erstellt mit Unterstuetzung eines Sprachmodells (Claude Code, Claude Opus 5.5),
 * geprueft von: <Name>
 */
#include "names.h"

#include "protocol.h"

const char_t *name_state(uint8_t state)
{
    const char_t *text;

    switch (state) {
    case STATE_INIT:
        text = "INIT";
        break;
    case STATE_BETRIEB:
        text = "BETRIEB";
        break;
    case STATE_OBJEKT_STOPP:
        text = "OBJEKT_STOPP";
        break;
    case STATE_FEHLER:
        text = "FEHLER";
        break;
    case STATE_SHUTDOWN:
        text = "SHUTDOWN";
        break;
    default:
        text = "?";
        break;
    }
    return text;
}

const char_t *name_error(uint8_t error)
{
    const char_t *text;

    switch (error) {
    case ERR_NONE:
        text = "-";
        break;
    case ERR_SENSOR:
        text = "SENSOR";
        break;
    case ERR_KOMMUNIKATION:
        text = "KOMMUNIKATION";
        break;
    case ERR_DATENALTER:
        text = "DATENALTER";
        break;
    default:
        text = "?";
        break;
    }
    return text;
}

const char_t *name_level(uint8_t level)
{
    const char_t *text;

    switch (level) {
    case LEVEL_OFF:
        text = "AUS";
        break;
    case LEVEL_LOW:
        text = "LOW";
        break;
    case LEVEL_MEDIUM:
        text = "MEDIUM";
        break;
    case LEVEL_HIGH:
        text = "HIGH";
        break;
    default:
        text = "?";
        break;
    }
    return text;
}

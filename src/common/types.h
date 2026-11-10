/*
 * types.h - Projektweite Typdefinitionen (MISRA C:2012 Dir 4.6).
 *
 * Im Projekt werden die Typen fester Breite aus <stdint.h> sowie bool,
 * size_t und ssize_t verwendet. Fuer Zeichen und Zeichenketten gilt char_t.
 */
#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef char char_t; /* Dir 4.6 Ausnahme 1: Basistyp nur in dieser typedef */

#endif /* TYPES_H */

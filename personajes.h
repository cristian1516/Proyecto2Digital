/*
 * personajes.h - Personajes guardados en la microSD (archivos .SPR).
 *
 * El formato está descrito en herramientas/convertir.py, que es el que los
 * genera a partir de sprites.c. Un .SPR trae una o más paletas: la segunda
 * se usa para el jugador 2 cuando los dos eligen al mismo personaje.
 */
#ifndef PERSONAJES_H_
#define PERSONAJES_H_

#include <stdint.h>
#include "sprites.h"
#include "recursos.h"

/* Copia el personaje del archivo al almacén de RAM y llena *pj. Usa la
 * paleta número "paleta" (si el archivo no la tiene, la 0). Devuelve REC_OK
 * o un error de recursos.h. */
uint8_t Personaje_Cargar(const char *archivo, Personaje *pj, uint8_t paleta);

/* Cambia la paleta de un personaje ya cargado (si no existe, usa la 0) */
void Personaje_Paleta(Personaje *pj, uint8_t paleta);

/* Lee el nombre de cada versión de color del personaje (el que se muestra
 * en el menú, 16 bytes con el 0 final). Guarda hasta "max" y en *nPaletas
 * cuántas guardó. */
uint8_t Personaje_Nombres(const char *archivo, char nombres[][16], uint8_t max,
		uint8_t *nPaletas);

/* Dibuja la pose en guardia directo de la SD, sin usar el almacén: los pies
 * en la fila "pies" y centrada en la columna "centro". Lo transparente se
 * pinta del color "fondo". */
uint8_t Personaje_Vista(const char *archivo, uint8_t paleta, int16_t centro,
		int16_t pies, uint8_t voltear, uint16_t fondo);

#endif /* PERSONAJES_H_ */

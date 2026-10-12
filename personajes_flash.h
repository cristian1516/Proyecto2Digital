/*
 * personajes_flash.h - Personajes guardados en la flash del STM32.
 *
 * Los personajes (.SPR) ya no se leen de la microSD: sus bytes, comprimidos,
 * están en arreglos const (pj_ryu.c, pj_deejay.c...) que el compilador deja
 * en la flash. La lista está en personajes_flash.c y la escribe
 * herramientas/spr_a_c.py. Los escenarios (.ESC) siguen en la SD.
 *
 * El juego los abre con el mismo nombre de archivo de antes ("RYU.SPR"):
 * SD_Abrir() de recursos.c busca primero en esta lista y, si está, lee de la
 * flash en vez de la tarjeta.
 */
#ifndef PERSONAJES_FLASH_H_
#define PERSONAJES_FLASH_H_

#include <stdint.h>
#include <stddef.h>

typedef struct {
	const char    *archivo;     /* "RYU.SPR": 8 letras + extensión            */
	const uint8_t *datos;       /* los bytes del .SPR                          */
	uint32_t       tam;
	const char    *nombres[4];  /* lo que se ve en el menú, uno por color;
	                               NULL = el nombre que trae el archivo        */
} PersonajeFlash;

extern const PersonajeFlash PERSONAJES_FLASH[];
extern const uint8_t N_PERSONAJES_FLASH;

#endif /* PERSONAJES_FLASH_H_ */

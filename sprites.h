/*
 * sprites.h - Personajes del juego.
 *
 * Formato de los cuadros: 1 byte por píxel. Cada byte es un índice a la
 * paleta del personaje; el índice 0 es transparente. Los cuadros van uno
 * después de otro: el cuadro k empieza en px + k * w * h, y cada cuadro va
 * por filas, de arriba hacia abajo y de izquierda a derecha. Todos los
 * personajes miran a la derecha y tienen los pies en la última fila.
 *
 * Cada personaje puede tener su propio tamaño de cuadro.
 */
#ifndef INC_SPRITES_H_
#define INC_SPRITES_H_

#include <stdint.h>

typedef struct {
	const uint8_t  *px;       /* cuadros de la animación de caminar         */
	const uint16_t *paleta;   /* colores RGB565 (la entrada 0 no se usa)    */
	uint8_t  w, h;            /* tamaño de un cuadro, en píxeles            */
	uint8_t  cuadros;         /* cuántos cuadros tiene la animación         */
} Personaje;

extern const Personaje ryu;       /* Ryu, colores originales        72 x 95  */
extern const Personaje ryuAzul;   /* misma hoja de Ryu, traje azul          */
extern const Personaje deeJay;    /* Dee Jay                      107 x 110 */

#endif /* INC_SPRITES_H_ */

/*
 * sprites.h - Personajes del juego.
 *
 * Cada personaje tiene una lista de cuadros por animación. Un cuadro es una
 * imagen de w x h con 1 byte por píxel: cada byte es un índice a la paleta
 * del personaje y el índice 0 es transparente. Los píxeles van por filas, de
 * arriba hacia abajo y de izquierda a derecha.
 *
 * Los cuadros no miden todos lo mismo (el de la patada es mucho más ancho
 * que el de la guardia), así que cada uno dice cómo se coloca respecto al
 * cuerpo del personaje:
 *
 *   - Los pies están en la última fila del cuadro: esa fila va en el piso.
 *   - "eje" es cuántas columnas del cuadro quedan DETRÁS del centro del
 *     cuerpo. Todos los personajes están dibujados mirando a la derecha;
 *     al voltearlos, esas columnas quedan del otro lado del centro.
 *
 *        eje                     centro del cuerpo
 *     |<------>|                        |
 *     +--------+----------------+       v
 *     |  espalda    brazo ----> |   [ cuerpo ]----> zona que golpea
 *     +--------+----------------+
 *
 * El cuerpo (cuerpoW x cuerpoH) es la caja que se usa para las colisiones.
 * Es más angosta que el dibujo: las manos y los pies sobresalen de ella.
 */
#ifndef INC_SPRITES_H_
#define INC_SPRITES_H_

#include <stdint.h>

/* Un cuadro de animación */
typedef struct {
	const uint8_t *px;    /* w * h índices de paleta (0 = transparente)      */
	uint8_t  w, h;        /* tamaño del cuadro, en píxeles                   */
	uint8_t  eje;         /* columnas que quedan detrás del centro del cuerpo */
} Cuadro;

/* Una animación: lista de cuadros. Con n = 0 el personaje no la tiene. */
typedef struct {
	const Cuadro *cuadros;
	uint8_t  n;
	uint8_t  activo;      /* solo en los ataques: el cuadro del golpe, con el
	                         brazo o la pierna extendidos. Los cuadros de
	                         antes son la preparación; los de después, si
	                         hay, son para recoger el brazo o la pierna     */
} Animacion;

/* Zona que hace daño mientras el ataque está activo: un rectángulo delante
 * del personaje. Sale del cuadro activo del ataque: es lo que sobresale de
 * la pose en guardia. Para que un ataque llegue más o menos lejos basta con
 * cambiar aquí su ancho (w). */
typedef struct {
	uint8_t  dx;          /* del centro del cuerpo a donde empieza, hacia adelante */
	uint8_t  dy;          /* de la parte de arriba del cuerpo hacia abajo          */
	uint8_t  w, h;
} ZonaGolpe;

typedef struct {
	const uint16_t *paleta;   /* colores RGB565 (la entrada 0 no se usa)     */
	uint8_t   cuerpoW;        /* caja de colisión del cuerpo                 */
	uint8_t   cuerpoH;
	uint8_t   margen;         /* lo más que el dibujo sobresale por detrás
	                             del cuerpo: el cuerpo no se acerca más que
	                             esto al borde de la pantalla                */
	Animacion caminar;        /* el cuadro 0 es también la pose en guardia   */
	Animacion ataque[2];      /* [0] golpe, [1] patada. Si n = 0, el ataque
	                             se dibuja como un rectángulo                */
	ZonaGolpe zona[2];        /* [0] golpe, [1] patada                       */
} Personaje;

extern const Personaje ryu;       /* Ryu, colores originales                 */
extern const Personaje ryuAzul;   /* los mismos cuadros de Ryu, traje azul   */
extern const Personaje deeJay;    /* Dee Jay                                 */

#endif /* INC_SPRITES_H_ */

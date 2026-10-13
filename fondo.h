/*
 * El escenario elegido en el menú se copia comprimido al almacén de RAM
 * (Fondo_Cargar). Al repintar, Fondo_Fila() entrega una fila ya
 * descomprimida: se descomprime un bloque de 8 filas a la vez y se guarda
 * el último, porque Repintar() pide las filas en orden.
 *
 * Si la imagen mide menos de 240 de alto se centra en la pantalla y lo que
 * sobra arriba y abajo queda negro.
 */
#ifndef FONDO_H_
#define FONDO_H_

#include <stdint.h>
#include "recursos.h"

#define FONDO_W            320
#define FONDO_H_MAX        240
#define FONDO_FILAS_BLOQUE 8         /* máximo de filas por bloque */
#define FONDO_SIN_SD       0x5D9F    /* color liso si no hay escenario cargado */

/* Copia el escenario al almacén de RAM. Devuelve REC_OK o un error. */
uint8_t Fondo_Cargar(const char *archivo);

/* Olvida el escenario cargado (se vuelve fondo liso) */
void Fondo_Quitar(void);

/* Usa lo que sobra del almacén para guardar bloques ya descomprimidos y no
 * descomprimirlos en cada tick. Llamar después de cargar lo demás. */
void Fondo_UsarRamLibre(void);

/* Llamar al empezar cada pelea: los menús usaron la memoria del bloque */
void Fondo_Reiniciar(void);

/* Dibuja el escenario en toda la pantalla, directo de la SD y sin usar el
 * almacén (vista previa del menú). */
uint8_t Fondo_Vista(const char *archivo);

/* Solo lee el nombre que se muestra en el menú (16 bytes con el 0 final) */
uint8_t Fondo_Nombre(const char *archivo, char nombre[16]);

/* Índices de paleta de la fila y de la pantalla (FONDO_W de ellos), o NULL
 * si esa fila no tiene escenario: ahí va el color Fondo_Vacio(). */
const uint8_t *Fondo_Fila(int16_t y);

extern uint16_t fondoPal[256];
extern uint16_t fondoAlto;     /* 0 = no hay escenario cargado */
extern uint16_t fondoPiso;     /* línea del piso del escenario; 0 = no se indica */

static inline uint8_t Fondo_Listo(void) {
	return fondoAlto != 0;
}

/* Color de las filas sin escenario: negro arriba y abajo de la imagen, o
 * el color liso si no hay escenario. */
static inline uint16_t Fondo_Vacio(void) {
	return fondoAlto ? 0x0000 : FONDO_SIN_SD;
}

#endif /* FONDO_H_ */

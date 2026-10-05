/*
 * fondo.h - Escenario guardado en la microSD.
 *
 * El escenario vive en un archivo de la SD. Antes de la pelea se copia
 * completo a la RAM (Fondo_Cargar) y durante la pelea se lee de ahí con
 * Fondo_Color(): la SD es demasiado lenta para leerla en cada tick.
 *
 * Formato del archivo (todos los números en little-endian):
 *
 *   bytes 0-3    'F' 'N' 'D' '1'
 *   bytes 4-5    ancho en píxeles (siempre 320)
 *   bytes 6-7    alto en píxeles (hasta 240)
 *   bytes 8-9    N = cantidad de colores de la paleta (1 a 256)
 *   bytes 10-11  0 (reservado)
 *   después      N colores RGB565, 2 bytes cada uno
 *   después      ancho x alto bytes: un índice de paleta por píxel, por
 *                filas, de arriba hacia abajo y de izquierda a derecha
 *
 * Con 1 byte por píxel una imagen de 320x220 ocupa 70 kB y cabe en la RAM
 * (128 kB); en RGB565 serían 141 kB y no cabría.
 *
 * Si la imagen mide menos de 240 de alto se centra en la pantalla y lo que
 * sobra arriba y abajo queda negro.
 */
#ifndef FONDO_H_
#define FONDO_H_

#include <stdint.h>

#define FONDO_W        320
#define FONDO_H_MAX    240
#define FONDO_SIN_SD   0x5D9F    /* color liso si no se pudo cargar el fondo */

/* Resultado de Fondo_Cargar y Fondo_Guardar */
enum {
	FONDO_OK = 0,
	FONDO_ERR_SD,          /* 1: no se pudo montar la tarjeta               */
	FONDO_ERR_ABRIR,       /* 2: no se pudo abrir o crear el archivo        */
	FONDO_ERR_DATOS,       /* 3: lectura o escritura incompleta             */
	FONDO_ERR_FORMATO,     /* 4: el archivo no tiene el formato de arriba   */
	FONDO_ERR_COLORES,     /* 5: la imagen tiene más de 256 colores         */
	FONDO_ERR_VERIFICAR    /* 6: lo guardado no coincide con la imagen      */
};

/* Copia el escenario de la SD a la RAM. Devuelve FONDO_OK o un error. */
uint8_t Fondo_Cargar(const char *nombre);

/* Convierte una imagen RGB565 de FONDO_W x alto (por ejemplo un arreglo de
 * Bitmaps.h) al formato de arriba, la guarda en la SD y la vuelve a leer
 * para comprobar que quedó igual. Al terminar el escenario queda cargado. */
uint8_t Fondo_Guardar(const char *nombre, const uint16_t *imagen, uint16_t alto);

/* Último código de FatFs (FRESULT), para saber por qué falló:
 *   3 = no hay tarjeta o no responde    13 = no está formateada en FAT32
 *   4 = el archivo no existe             1 = error de lectura/escritura  */
uint8_t Fondo_CodigoFatFs(void);

/* Datos en RAM. No usarlos directamente: usar Fondo_Color(). */
extern uint8_t  fondoPx[FONDO_W * FONDO_H_MAX];
extern uint16_t fondoPal[256];
extern uint16_t fondoAlto;     /* 0 = no hay escenario cargado */
extern uint16_t fondoY;        /* fila de la pantalla donde empieza */

static inline uint8_t Fondo_Listo(void) {
	return fondoAlto != 0;
}

/* Color RGB565 del escenario en el píxel (x, y) de la pantalla */
static inline uint16_t Fondo_Color(int16_t x, int16_t y) {
	if (fondoAlto == 0)
		return FONDO_SIN_SD;
	if (y < (int16_t) fondoY || y >= (int16_t) (fondoY + fondoAlto))
		return 0x0000;
	return fondoPal[fondoPx[(uint32_t) (y - fondoY) * FONDO_W + (uint32_t) x]];
}

#endif /* FONDO_H_ */

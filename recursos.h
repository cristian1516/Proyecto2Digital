/*
 * recursos.h - Lo que se carga de la microSD: memoria, tarjeta y compresión.
 *
 * Los personajes (.SPR) y los escenarios (.ESC) viven en la SD, comprimidos.
 * Antes de cada pelea se copian a la RAM solo los elegidos en el menú
 * (personajes.c y fondo.c), en un bloque de memoria fijo: el "almacén".
 * Así la flash ya no guarda imágenes y la RAM solo tiene lo que se usa.
 *
 *   - Almacén: un arreglo de RAM_ALMACEN bytes. Ram_Pedir() va entregando
 *     pedazos de él, uno detrás de otro; Ram_Liberar() lo vacía completo
 *     antes de cargar una pelea nueva. No hay malloc ni fragmentación.
 *
 *   - Compresión LZ (la misma de herramientas/convertir.py): grupos de 8
 *     elementos con un byte de banderas al inicio; bit en 1 = un byte
 *     literal, bit en 0 = copia de 2 bytes que repite (v >> 10) + 3 bytes
 *     que están (v & 0x3FF) + 1 bytes atrás.
 */
#ifndef RECURSOS_H_
#define RECURSOS_H_

#include <stdint.h>
#include "fatfs.h"

/* Tamaño del almacén. La combinación más grande (Ryu vs Dee Jay con todos
 * sus cuadros y CASA ANGEL) usa unos 111 kB; lo que sobra de los 128 kB
 * del STM32F446 queda para las demás variables (~12 kB) y la pila (~4 kB,
 * el juego usa menos de 2 kB). Si el compilador dice que la RAM no alcanza
 * ("region RAM overflowed"), bajar este número. */
#ifndef RAM_ALMACEN
#define RAM_ALMACEN    (112u * 1024u)
#endif

/* Resultado de las funciones que leen la SD */
enum {
	REC_OK = 0,
	REC_ERR_SD,        /* 1: no se pudo montar la tarjeta                    */
	REC_ERR_ABRIR,     /* 2: no se pudo abrir el archivo                     */
	REC_ERR_DATOS,     /* 3: lectura incompleta o datos dañados              */
	REC_ERR_FORMATO,   /* 4: el archivo no tiene el formato esperado         */
	REC_ERR_RAM        /* 5: no cabe en el almacén de RAM                    */
};

/* ---- Almacén de RAM ---------------------------------------------------- */
void     Ram_Liberar(void);
void    *Ram_Pedir(uint32_t n);         /* NULL si no cabe; alineado a 8 */
uint32_t Ram_Usada(void);
uint32_t Ram_Libre(void);

/* ---- Tarjeta -------------------------------------------------------------- */
uint8_t  SD_Montar(void);               /* REC_OK o REC_ERR_SD; si ya está montada no hace nada */
uint8_t  SD_CodigoFatFs(void);          /* último FRESULT, para mostrarlo en pantalla */
void     SD_Anotar(FRESULT r);          /* guarda r como último código */

/* Abre un archivo de la raíz para leerlo (en USERFile, uno a la vez) */
uint8_t  SD_Abrir(const char *nombre);
/* Lee exactamente n bytes; REC_OK o REC_ERR_DATOS */
uint8_t  SD_Leer(void *dst, uint32_t n);
uint8_t  SD_Saltar(uint32_t posicion);  /* ir a esa posición del archivo */
void     SD_Cerrar(void);

/* Busca en la raíz los archivos con esa extensión (".SPR", ".ESC").
 * Guarda hasta "max" nombres de archivo (8.3) y devuelve cuántos encontró. */
uint8_t  SD_Listar(const char *ext, char nombres[][13], uint8_t max);

/* ---- Descompresión -------------------------------------------------------- */

/* De memoria a memoria: "n" bytes de salida a partir de "tam" bytes
 * comprimidos. Devuelve 1 si salió bien y 0 si los datos están dañados. */
uint8_t  LZ_Descomprimir(const uint8_t *in, uint32_t tam, uint8_t *out, uint32_t n);

/* Directo del archivo abierto, sin cargarlo a la RAM: descomprime "n" bytes
 * y cada vez que completa una fila de "w" bytes llama a fila(px, k, ctx)
 * (k = 0, 1, 2... + fila0). Sirve para dibujar las vistas previas del menú
 * sin tocar el almacén. Antes de la primera llamada: LZ_FlujoIniciar(). */
typedef void (*LZ_FnFila)(const uint8_t *px, uint16_t k, void *ctx);
void     LZ_FlujoIniciar(void);
uint8_t  LZ_Flujo(uint32_t n, uint16_t w, uint16_t fila0, LZ_FnFila fila, void *ctx);

/* Área de trabajo compartida. Los menús la usan para dibujar las vistas
 * previas directo de la SD y la pelea para descomprimir el escenario; como
 * nunca pasan a la vez, comparten la misma memoria (ahorra ~2.5 kB). */
typedef union {
	struct {
		uint16_t pal[256];        /* paleta de la vista previa             */
		uint8_t  anillo[1024];    /* ventana del descompresor              */
		uint8_t  renglon[320];    /* fila descomprimida                    */
		uint8_t  entrada[256];    /* lo leído del archivo                  */
	} menu;
	uint8_t bloque[320 * 8];      /* bloque del escenario (fondo.c)        */
} AreaTrabajo;
extern AreaTrabajo trabajo;
#define vistaPal (trabajo.menu.pal)

/* Lee números little-endian de un arreglo de bytes */
static inline uint16_t LE16(const uint8_t *p) {
	return (uint16_t) (p[0] | (p[1] << 8));
}
static inline uint32_t LE32(const uint8_t *p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16)
	     | ((uint32_t) p[3] << 24);
}

#endif /* RECURSOS_H_ */

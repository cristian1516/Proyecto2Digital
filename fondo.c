/*
 * fondo.c - Escenarios de la microSD (.ESC).
 *
 * Encabezado del archivo (36 bytes, little-endian):
 *   0-3 "ESC1" | 4-19 nombre | 20-21 ancho (320) | 22-23 alto
 *   24-25 nColores | 26-27 piso | 28-29 filasBloque | 30-31 nBloques
 *   32-35 tamDatos
 */
#include <string.h>
#include "main.h"
#include "ili9341.h"
#include "fondo.h"

#define ENCABEZADO 36

uint16_t fondoPal[256];
uint16_t fondoAlto;
uint16_t fondoPiso;

static uint16_t fondoY;         /* fila de la pantalla donde empieza la imagen */
static uint8_t  filasBloque;
static uint16_t nBloques;
static const uint8_t *datos;    /* bloques comprimidos, en el almacén       */
static uint32_t *inicio;        /* dónde empieza cada bloque (nBloques + 1) */

/* Último bloque descomprimido. Comparte memoria con los menús*/
_Static_assert(sizeof trabajo.bloque >= FONDO_W * FONDO_FILAS_BLOQUE, "AreaTrabajo muy chica");
#define bloque (trabajo.bloque)
static int16_t  bloqueAct = -1;

/* Bloques ya descomprimidos guardados en lo que sobró del almacén. El
 * bloque b va en el lugar b % nGuardados. Los peleadores se mueven casi
 * siempre en las mismas filas, así que después de unos ticks ya casi no
 * hay que descomprimir nada. */
static uint8_t  *guardados;
static int16_t  *guardadoDe;    /* qué bloque tiene cada lugar (-1 = nada) */
static uint16_t  nGuardados;

typedef struct {
	char     nombre[16];
	uint16_t alto, colores, piso, filas, bloques;
	uint32_t tamDatos;
} Encabezado;

static uint8_t LeerEncabezado(Encabezado *e) {
	uint8_t b[ENCABEZADO];
	if (SD_Leer(b, sizeof b) != REC_OK)
		return REC_ERR_DATOS;
	if (memcmp(b, "ESC1", 4) != 0)
		return REC_ERR_FORMATO;
	memcpy(e->nombre, b + 4, 16);
	e->nombre[15] = 0;
	e->alto     = LE16(b + 22);
	e->colores  = LE16(b + 24);
	e->piso     = LE16(b + 26);
	e->filas    = LE16(b + 28);
	e->bloques  = LE16(b + 30);
	e->tamDatos = LE32(b + 32);
	if (LE16(b + 20) != FONDO_W || e->alto == 0 || e->alto > FONDO_H_MAX
			|| e->colores == 0 || e->colores > 256 || e->filas == 0
			|| e->filas > FONDO_FILAS_BLOQUE || e->tamDatos == 0
			|| e->bloques != (e->alto + e->filas - 1) / e->filas)
		return REC_ERR_FORMATO;
	return REC_OK;
}

void Fondo_Quitar(void) {
	fondoAlto = 0;
	bloqueAct = -1;
	nGuardados = 0;
}

void Fondo_Reiniciar(void) {
	bloqueAct = -1;
}

void Fondo_UsarRamLibre(void) {
	const uint32_t tamBloque = (uint32_t) FONDO_W * filasBloque;
	uint32_t n;
	nGuardados = 0;
	if (fondoAlto == 0)
		return;
	n = Ram_Libre() / (tamBloque + 2u + 8u);   /* + etiqueta + alineación */
	if (n > nBloques)
		n = nBloques;
	if (n < 2)
		return;                         /* no vale la pena: se usa "bloque" */
	guardadoDe = Ram_Pedir(n * 2u);
	guardados  = Ram_Pedir(n * tamBloque);
	if (guardadoDe == NULL || guardados == NULL)
		return;
	for (uint32_t i = 0; i < n; i++)
		guardadoDe[i] = -1;
	nGuardados = (uint16_t) n;
}

/* Descomprime el bloque b en "destino" */
static void Descomprimir(int16_t b, uint8_t *destino) {
	uint16_t filas = filasBloque;
	if ((uint16_t) (b * filasBloque + filas) > fondoAlto)
		filas = (uint16_t) (fondoAlto - b * filasBloque);      /* el último */
	if (!LZ_Descomprimir(datos + inicio[b], inicio[b + 1] - inicio[b],
			destino, (uint32_t) filas * FONDO_W))
		memset(destino, 0, (uint32_t) filas * FONDO_W);        /* dañado: color 0 */
}

static uint8_t Cargar(void) {
	Encabezado e;
	uint8_t res = LeerEncabezado(&e);
	if (res != REC_OK)
		return res;

	inicio = Ram_Pedir((e.bloques + 1u) * 4u);
	datos  = Ram_Pedir(e.tamDatos);
	if (inicio == NULL || datos == NULL)
		return REC_ERR_RAM;

	if (SD_Leer(fondoPal, e.colores * 2u) != REC_OK)
		return REC_ERR_DATOS;

	/* Tamaños de los bloques -> dónde empieza cada uno */
	inicio[0] = 0;
	for (uint16_t i = 0; i < e.bloques; i++) {
		uint8_t t[2];
		if (SD_Leer(t, 2) != REC_OK)
			return REC_ERR_DATOS;
		inicio[i + 1] = inicio[i] + LE16(t);
	}
	if (inicio[e.bloques] != e.tamDatos)
		return REC_ERR_FORMATO;

	/* Los bloques: una sola lectura, directo a la RAM */
	if (SD_Leer((void *) datos, e.tamDatos) != REC_OK)
		return REC_ERR_DATOS;

	filasBloque = (uint8_t) e.filas;
	nBloques = e.bloques;
	fondoY = (uint16_t) ((FONDO_H_MAX - e.alto) / 2);
	fondoPiso = e.piso;
	fondoAlto = e.alto;
	return REC_OK;
}

uint8_t Fondo_Cargar(const char *archivo) {
	uint8_t res;
	Fondo_Quitar();
	res = SD_Abrir(archivo);
	if (res != REC_OK)
		return res;
	res = Cargar();
	SD_Cerrar();
	if (res != REC_OK)
		Fondo_Quitar();
	return res;
}

const uint8_t *Fondo_Fila(int16_t y) {
	int16_t fy, b;
	if (fondoAlto == 0)
		return NULL;
	fy = (int16_t) (y - fondoY);                    /* fila de la imagen */
	if (fy < 0 || fy >= (int16_t) fondoAlto)
		return NULL;
	b = (int16_t) (fy / filasBloque);
	fy = (int16_t) (fy - b * filasBloque);          /* fila dentro del bloque */
	if (nGuardados > 0) {
		const uint16_t lugar = (uint16_t) b % nGuardados;
		uint8_t *p = guardados + (uint32_t) lugar * FONDO_W * filasBloque;
		if (guardadoDe[lugar] != b) {
			Descomprimir(b, p);
			guardadoDe[lugar] = b;
		}
		return p + (uint32_t) fy * FONDO_W;
	}
	if (b != bloqueAct) {
		Descomprimir(b, bloque);
		bloqueAct = b;
	}
	return bloque + (uint32_t) fy * FONDO_W;
}

uint8_t Fondo_Nombre(const char *archivo, char nombre[16]) {
	Encabezado e;
	uint8_t res = SD_Abrir(archivo);
	if (res != REC_OK)
		return res;
	res = LeerEncabezado(&e);
	SD_Cerrar();
	if (res == REC_OK)
		memcpy(nombre, e.nombre, 16);
	return res;
}

/* Vista previa del menú  */

static void FilaVista(const uint8_t *px, uint16_t k, void *ctx) {
	int16_t y = (int16_t) (*(const uint16_t *) ctx + k);
	if (y >= 240)
		return;
	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
	SetWindows(0, y, FONDO_W - 1, y);
	for (uint16_t x = 0; x < FONDO_W; x++) {
		uint16_t c = vistaPal[px[x]];
		LCD_DATA((uint8_t) (c >> 8));
		LCD_DATA((uint8_t) c);
	}
	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

uint8_t Fondo_Vista(const char *archivo) {
	Encabezado e;
	uint16_t y0;
	uint8_t res = SD_Abrir(archivo);
	if (res != REC_OK)
		return res;

	res = LeerEncabezado(&e);
	if (res == REC_OK && SD_Leer(vistaPal, e.colores * 2u) != REC_OK)
		res = REC_ERR_DATOS;
	if (res == REC_OK && SD_Saltar(ENCABEZADO + e.colores * 2u + e.bloques * 2u) != REC_OK)
		res = REC_ERR_DATOS;
	if (res == REC_OK) {
		y0 = (uint16_t) ((FONDO_H_MAX - e.alto) / 2);
		if (y0 > 0)                                     /* franjas negras */
			FillRect(0, 0, FONDO_W, y0, 0x0000);
		if (y0 + e.alto < FONDO_H_MAX)
			FillRect(0, y0 + e.alto, FONDO_W, FONDO_H_MAX - y0 - e.alto, 0x0000);
		/* Los bloques van uno tras otro en el archivo: cada uno es una
		 * compresión aparte, pero la lectura sigue donde quedó. */
		LZ_FlujoIniciar();
		for (uint16_t b = 0; b < e.bloques && res == REC_OK; b++) {
			uint16_t primera = (uint16_t) (b * e.filas);
			uint16_t filas = e.filas;
			if (primera + filas > e.alto)
				filas = (uint16_t) (e.alto - primera);
			if (!LZ_Flujo((uint32_t) filas * FONDO_W, FONDO_W, primera, FilaVista, &y0))
				res = REC_ERR_DATOS;
		}
	}
	SD_Cerrar();
	return res;
}

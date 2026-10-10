/*
 * personajes.c - Carga de personajes (.SPR) de la microSD. Ver personajes.h.
 *
 * Encabezado del archivo (56 bytes, little-endian):
 *   0-3 "PJ01" | 4-19 nombre | 20 cuerpoW | 21 cuerpoH | 22 margen
 *   23 nPaletas | 24-25 nColores | 26-27 nCuadros | 28-29 maxCuadro
 *   32-43 seis animaciones {n, activo}: caminar, ko, cubre, dano, golpe,
 *         patada | 44-51 dos zonas {dx, dy, w, h} | 52-55 tamDatos
 * Después: las paletas, en "PJ02" el nombre de cada paleta (16 bytes cada
 * uno, p. ej. "RYU" y "RYU AZUL"), la tabla de cuadros (8 bytes cada uno: w, h, eje,
 * 0, tam de 4 bytes) y los cuadros comprimidos.
 */
#include <string.h>
#include "main.h"
#include "ili9341.h"
#include "personajes.h"

#define ENCABEZADO   56
#define CUADRO_TABLA 8
#define N_ANIM       6

typedef struct {
	char     nombre[16];
	uint8_t  cuerpoW, cuerpoH, margen, nPaletas;
	uint16_t nColores, nCuadros, maxCuadro;
	uint8_t  anim[N_ANIM][2];      /* {n, activo} */
	uint8_t  zona[2][4];
	uint32_t tamDatos;
	uint32_t tamNombres;           /* PJ02: nPaletas * 16; PJ01: 0 */
} Encabezado;

/* Lee y revisa el encabezado del archivo ya abierto */
static uint8_t LeerEncabezado(Encabezado *e) {
	uint8_t b[ENCABEZADO];
	uint16_t suma = 0;

	if (SD_Leer(b, sizeof b) != REC_OK)
		return REC_ERR_DATOS;
	if (memcmp(b, "PJ01", 4) != 0 && memcmp(b, "PJ02", 4) != 0)
		return REC_ERR_FORMATO;
	memcpy(e->nombre, b + 4, 16);
	e->nombre[15] = 0;
	e->cuerpoW   = b[20];
	e->cuerpoH   = b[21];
	e->margen    = b[22];
	e->nPaletas  = b[23];
	e->nColores  = LE16(b + 24);
	e->nCuadros  = LE16(b + 26);
	e->maxCuadro = LE16(b + 28);
	memcpy(e->anim, b + 32, sizeof e->anim);
	memcpy(e->zona, b + 44, sizeof e->zona);
	e->tamDatos  = LE32(b + 52);
	e->tamNombres = (b[3] == '2') ? (uint32_t) e->nPaletas * 16u : 0;

	if (e->nPaletas == 0 || e->nPaletas > 8 || e->nColores == 0
			|| e->nColores > 256 || e->nCuadros == 0 || e->maxCuadro == 0
			|| e->tamDatos == 0 || e->anim[0][0] == 0)
		return REC_ERR_FORMATO;
	for (uint8_t a = 0; a < N_ANIM; a++) {
		if (e->anim[a][0] > 0 && e->anim[a][1] >= e->anim[a][0])
			return REC_ERR_FORMATO;     /* el cuadro activo no existe */
		suma += e->anim[a][0];
	}
	if (suma != e->nCuadros)
		return REC_ERR_FORMATO;
	return REC_OK;
}

static uint8_t Cargar(Personaje *pj, uint8_t paleta) {
	Encabezado e;
	uint16_t *paletas;
	Cuadro   *cuadros;
	uint8_t  *datos;
	uint32_t  offset = 0;
	uint8_t   res;
	Animacion *destino[N_ANIM];
	uint16_t  k = 0;

	res = LeerEncabezado(&e);
	if (res != REC_OK)
		return res;

	paletas = Ram_Pedir((uint32_t) e.nPaletas * e.nColores * 2u);
	cuadros = Ram_Pedir((uint32_t) e.nCuadros * sizeof(Cuadro));
	datos   = Ram_Pedir(e.tamDatos);
	if (paletas == NULL || cuadros == NULL || datos == NULL)
		return REC_ERR_RAM;

	if (SD_Leer(paletas, (uint32_t) e.nPaletas * e.nColores * 2u) != REC_OK)
		return REC_ERR_DATOS;
	/* Los nombres de las paletas solo se usan en el menú: se saltan */
	if (e.tamNombres > 0 && SD_Saltar(ENCABEZADO + (uint32_t) e.nPaletas * e.nColores * 2u
			+ e.tamNombres) != REC_OK)
		return REC_ERR_DATOS;

	/* Tabla de cuadros: tamaño y dónde queda cada uno dentro de "datos" */
	for (uint16_t i = 0; i < e.nCuadros; i++) {
		uint8_t t[CUADRO_TABLA];
		uint32_t tam;
		if (SD_Leer(t, sizeof t) != REC_OK)
			return REC_ERR_DATOS;
		tam = LE32(t + 4);
		if (t[0] == 0 || t[1] == 0 || (uint32_t) t[0] * t[1] > e.maxCuadro
				|| tam == 0 || tam > 0xFFFFu || offset + tam > e.tamDatos)
			return REC_ERR_FORMATO;
		cuadros[i].datos = datos + offset;
		cuadros[i].tam   = (uint16_t) tam;
		cuadros[i].w     = t[0];
		cuadros[i].h     = t[1];
		cuadros[i].eje   = t[2];
		offset += tam;
	}
	if (offset != e.tamDatos)
		return REC_ERR_FORMATO;

	/* Los cuadros comprimidos: una sola lectura, directo a la RAM */
	if (SD_Leer(datos, e.tamDatos) != REC_OK)
		return REC_ERR_DATOS;

	/* Todo bien: llenar el personaje */
	memset(pj, 0, sizeof *pj);
	memcpy(pj->nombre, e.nombre, sizeof pj->nombre);
	pj->paletas   = paletas;
	pj->nPaletas  = e.nPaletas;
	pj->nColores  = e.nColores;
	pj->maxCuadro = e.maxCuadro;
	pj->cuerpoW   = e.cuerpoW;
	pj->cuerpoH   = e.cuerpoH;
	pj->margen    = e.margen;
	destino[0] = &pj->caminar;
	destino[1] = &pj->ko;
	destino[2] = &pj->cubre;
	destino[3] = &pj->dano;
	destino[4] = &pj->ataque[0];
	destino[5] = &pj->ataque[1];
	for (uint8_t a = 0; a < N_ANIM; a++) {
		destino[a]->n       = e.anim[a][0];
		destino[a]->activo  = e.anim[a][1];
		destino[a]->cuadros = e.anim[a][0] ? &cuadros[k] : NULL;
		k = (uint16_t) (k + e.anim[a][0]);
	}
	for (uint8_t z = 0; z < 2; z++) {
		pj->zona[z].dx = e.zona[z][0];
		pj->zona[z].dy = e.zona[z][1];
		pj->zona[z].w  = e.zona[z][2];
		pj->zona[z].h  = e.zona[z][3];
	}
	Personaje_Paleta(pj, paleta);
	return REC_OK;
}

uint8_t Personaje_Cargar(const char *archivo, Personaje *pj, uint8_t paleta) {
	uint8_t res = SD_Abrir(archivo);
	if (res != REC_OK)
		return res;
	res = Cargar(pj, paleta);
	SD_Cerrar();
	return res;
}

void Personaje_Paleta(Personaje *pj, uint8_t paleta) {
	if (paleta >= pj->nPaletas)
		paleta = 0;
	pj->paleta = pj->paletas + (uint32_t) paleta * pj->nColores;
}

uint8_t Personaje_Nombres(const char *archivo, char nombres[][16], uint8_t max,
		uint8_t *nPaletas) {
	Encabezado e;
	uint8_t n, res = SD_Abrir(archivo);
	if (res != REC_OK)
		return res;
	res = LeerEncabezado(&e);
	if (res == REC_OK) {
		n = (e.nPaletas < max) ? e.nPaletas : max;
		*nPaletas = n;
		if (e.tamNombres > 0) {
			/* PJ02: un nombre por paleta, después de las paletas */
			if (SD_Saltar(ENCABEZADO + (uint32_t) e.nPaletas * e.nColores * 2u) != REC_OK
					|| SD_Leer(nombres, (uint32_t) n * 16u) != REC_OK)
				res = REC_ERR_DATOS;
			for (uint8_t i = 0; i < n; i++)
				nombres[i][15] = 0;
		} else {
			/* PJ01: no trae nombres; "RYU", "RYU 2", "RYU 3"... */
			for (uint8_t i = 0; i < n; i++) {
				memcpy(nombres[i], e.nombre, 16);
				if (i > 0) {
					uint8_t l = (uint8_t) strlen(nombres[i]);
					if (l > 13) l = 13;
					nombres[i][l] = ' ';
					nombres[i][l + 1] = (char) ('1' + i);
					nombres[i][l + 2] = 0;
				}
			}
		}
	}
	SD_Cerrar();
	return res;
}

/* ---- Vista previa del menú ----------------------------------------------- */

typedef struct {
	int16_t  x, y;          /* esquina superior izquierda en la pantalla */
	uint8_t  w, voltear;
	uint16_t fondo;
} Vista;

/* Dibuja una fila del cuadro, tal como sale del descompresor */
static void FilaVista(const uint8_t *px, uint16_t k, void *ctx) {
	const Vista *v = ctx;
	int16_t y = (int16_t) (v->y + k);
	int16_t a = v->x, b = (int16_t) (v->x + v->w);   /* columnas [a, b) */
	if (y < 0 || y >= 240)
		return;
	if (a < 0) a = 0;
	if (b > 320) b = 320;
	if (a >= b)
		return;
	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
	SetWindows(a, y, b - 1, y);
	for (int16_t x = a; x < b; x++) {
		int16_t col = (int16_t) (x - v->x);
		uint8_t i = px[v->voltear ? v->w - 1 - col : col];
		uint16_t c = i ? vistaPal[i] : v->fondo;
		LCD_DATA((uint8_t) (c >> 8));
		LCD_DATA((uint8_t) c);
	}
	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

uint8_t Personaje_Vista(const char *archivo, uint8_t paleta, int16_t centro,
		int16_t pies, uint8_t voltear, uint16_t fondo) {
	Encabezado e;
	uint8_t t[CUADRO_TABLA];
	uint32_t pos;
	Vista v;
	uint8_t res = SD_Abrir(archivo);
	if (res != REC_OK)
		return res;

	res = LeerEncabezado(&e);
	if (paleta >= e.nPaletas)
		paleta = 0;
	/* Solo la paleta que se va a usar y la entrada del cuadro 0 (guardia) */
	pos = ENCABEZADO + (uint32_t) paleta * e.nColores * 2u;
	if (res == REC_OK && (SD_Saltar(pos) != REC_OK
			|| SD_Leer(vistaPal, e.nColores * 2u) != REC_OK))
		res = REC_ERR_DATOS;
	pos = ENCABEZADO + (uint32_t) e.nPaletas * e.nColores * 2u + e.tamNombres;
	if (res == REC_OK && (SD_Saltar(pos) != REC_OK
			|| SD_Leer(t, sizeof t) != REC_OK))
		res = REC_ERR_DATOS;
	pos += (uint32_t) e.nCuadros * CUADRO_TABLA;
	if (res == REC_OK && SD_Saltar(pos) != REC_OK)
		res = REC_ERR_DATOS;

	if (res == REC_OK) {
		v.w = t[0];
		v.voltear = voltear;
		v.fondo = fondo;
		v.x = (int16_t) (voltear ? centro - (t[0] - t[2]) : centro - t[2]);
		v.y = (int16_t) (pies - t[1]);
		LZ_FlujoIniciar();
		if (!LZ_Flujo((uint32_t) t[0] * t[1], t[0], 0, FilaVista, &v))
			res = REC_ERR_DATOS;
	}
	SD_Cerrar();
	return res;
}

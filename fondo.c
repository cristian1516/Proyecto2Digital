/*
 * fondo.c - Escenario guardado en la microSD. Ver fondo.h.
 *
 * Usa FatFs tal como lo genera CubeMX (modo "User-defined") con el driver
 * fatfs_sd.c sobre SPI1.
 */
#include "main.h"
#include "fatfs.h"      /* ff.h, USERFatFS, USERFile, USERPath, MX_FATFS_Init */
#include "fondo.h"

extern SPI_HandleTypeDef hspi1;

uint8_t  fondoPx[FONDO_W * FONDO_H_MAX];
uint16_t fondoPal[256];
uint16_t fondoAlto;
uint16_t fondoY;
uint16_t fondoPiso;

static uint8_t montada;        /* 1 = la tarjeta ya está montada */
static uint8_t codigoFatFs;

/* Encabezado del archivo: 12 bytes, sin relleno */
typedef struct {
	char     firma[4];
	uint16_t ancho, alto, colores, piso;
} Encabezado;

uint8_t Fondo_CodigoFatFs(void) {
	return codigoFatFs;
}

/* Cambia la velocidad del SPI1 (bits BR del registro CR1) */
static void VelocidadSPI(uint32_t prescaler) {
	__HAL_SPI_DISABLE(&hspi1);
	MODIFY_REG(hspi1.Instance->CR1, SPI_CR1_BR, prescaler);
	__HAL_SPI_ENABLE(&hspi1);
}

/* Monta la tarjeta la primera vez que se necesita */
static uint8_t Montar(void) {
	uint32_t normal;
	FRESULT r;

	if (montada)
		return FONDO_OK;

	/* Por si main.c no llamó a MX_FATFS_Init(): sin esto FatFs no tiene
	 * driver y el programa se cae al tocar la tarjeta. */
	if (FATFS_GetAttachedDriversNbr() == 0)
		MX_FATFS_Init();

	/* Una SD se debe inicializar a 400 kHz o menos. Se baja el SPI a
	 * 80 MHz / 256 = 312 kHz solo para montar y luego se regresa a la
	 * velocidad configurada en CubeMX. */
	normal = hspi1.Instance->CR1 & SPI_CR1_BR;
	VelocidadSPI(SPI_BAUDRATEPRESCALER_256);
	r = f_mount(&USERFatFS, USERPath, 1);       /* 1 = montar ahora */
	VelocidadSPI(normal);

	codigoFatFs = (uint8_t) r;
	if (r != FR_OK)
		return FONDO_ERR_SD;
	montada = 1;
	return FONDO_OK;
}

uint8_t Fondo_Cargar(const char *nombre) {
	Encabezado e;
	UINT n;
	FRESULT r;
	uint8_t res = FONDO_OK;

	if (Montar() != FONDO_OK)
		return FONDO_ERR_SD;

	r = f_open(&USERFile, nombre, FA_READ);
	codigoFatFs = (uint8_t) r;
	if (r != FR_OK)
		return FONDO_ERR_ABRIR;

	/* Encabezado */
	r = f_read(&USERFile, &e, sizeof e, &n);
	if (r != FR_OK || n != sizeof e)
		res = FONDO_ERR_DATOS;
	else if (e.firma[0] != 'F' || e.firma[1] != 'N' || e.firma[2] != 'D'
			|| e.firma[3] != '1' || e.ancho != FONDO_W || e.alto == 0
			|| e.alto > FONDO_H_MAX || e.colores == 0 || e.colores > 256)
		res = FONDO_ERR_FORMATO;

	/* Paleta. Desde aquí se pisa lo que había en la RAM; si el archivo no
	 * existe o no sirve, el escenario anterior sigue intacto. */
	if (res == FONDO_OK) {
		fondoAlto = 0;
		r = f_read(&USERFile, fondoPal, e.colores * 2u, &n);
		if (r != FR_OK || n != e.colores * 2u)
			res = FONDO_ERR_DATOS;
	}

	/* Píxeles: una sola lectura, directo al arreglo en RAM */
	if (res == FONDO_OK) {
		UINT total = (UINT) e.ancho * e.alto;
		r = f_read(&USERFile, fondoPx, total, &n);
		if (r != FR_OK || n != total)
			res = FONDO_ERR_DATOS;
	}

	if (r != FR_OK)
		codigoFatFs = (uint8_t) r;
	f_close(&USERFile);

	if (res == FONDO_OK) {
		fondoY = (uint16_t) ((FONDO_H_MAX - e.alto) / 2);
		fondoPiso = e.piso;
		fondoAlto = e.alto;
	}
	return res;
}

uint8_t Fondo_Guardar(const char *nombre, const uint16_t *imagen, uint16_t alto) {
	Encabezado e = { { 'F', 'N', 'D', '1' }, FONDO_W, alto, 0, 0 };
	uint8_t  fila[FONDO_W];
	uint32_t total = (uint32_t) FONDO_W * alto;
	uint16_t colores = 0;
	uint8_t  ultimo = 0;
	uint8_t  res = FONDO_OK;
	UINT n;
	FRESULT r;

	fondoAlto = 0;                  /* se va a usar fondoPal como área de trabajo */
	if (alto == 0 || alto > FONDO_H_MAX)
		return FONDO_ERR_FORMATO;
	if (Montar() != FONDO_OK)
		return FONDO_ERR_SD;

	/* 1) Paleta: lista de colores distintos, en orden de aparición */
	for (uint32_t i = 0; i < total; i++) {
		uint16_t c = imagen[i];
		uint16_t k;
		if (colores > 0 && fondoPal[ultimo] == c)
			continue;               /* igual al píxel anterior: lo más común */
		for (k = 0; k < colores && fondoPal[k] != c; k++)
			;
		if (k == colores) {
			if (colores == 256)
				return FONDO_ERR_COLORES;
			fondoPal[colores++] = c;
		}
		ultimo = (uint8_t) k;
	}
	e.colores = colores;

	/* 2) Archivo: encabezado, paleta y los índices fila por fila */
	r = f_open(&USERFile, nombre, FA_WRITE | FA_CREATE_ALWAYS);
	codigoFatFs = (uint8_t) r;
	if (r != FR_OK)
		return FONDO_ERR_ABRIR;

	r = f_write(&USERFile, &e, sizeof e, &n);
	if (r != FR_OK || n != sizeof e)
		res = FONDO_ERR_DATOS;
	if (res == FONDO_OK) {
		r = f_write(&USERFile, fondoPal, colores * 2u, &n);
		if (r != FR_OK || n != colores * 2u)
			res = FONDO_ERR_DATOS;
	}
	ultimo = 0;
	for (uint16_t y = 0; y < alto && res == FONDO_OK; y++) {
		const uint16_t *p = imagen + (uint32_t) y * FONDO_W;
		for (uint16_t x = 0; x < FONDO_W; x++) {
			if (fondoPal[ultimo] != p[x]) {
				uint16_t k = 0;
				while (fondoPal[k] != p[x])
					k++;            /* siempre está: se agregó en el paso 1 */
				ultimo = (uint8_t) k;
			}
			fila[x] = ultimo;
		}
		r = f_write(&USERFile, fila, FONDO_W, &n);
		if (r != FR_OK || n != FONDO_W)
			res = FONDO_ERR_DATOS;
	}
	if (r != FR_OK)
		codigoFatFs = (uint8_t) r;

	/* Cerrar es lo que termina de escribir los datos en la tarjeta */
	r = f_close(&USERFile);
	if (r != FR_OK && res == FONDO_OK) {
		codigoFatFs = (uint8_t) r;
		res = FONDO_ERR_DATOS;
	}
	if (res != FONDO_OK)
		return res;

	/* 3) Comprobación: leer el archivo y comparar píxel por píxel */
	res = Fondo_Cargar(nombre);
	if (res != FONDO_OK)
		return res;
	if (fondoAlto != alto) {
		fondoAlto = 0;
		return FONDO_ERR_VERIFICAR;
	}
	for (uint32_t i = 0; i < total; i++) {
		if (fondoPal[fondoPx[i]] != imagen[i]) {
			fondoAlto = 0;
			return FONDO_ERR_VERIFICAR;
		}
	}
	return FONDO_OK;
}

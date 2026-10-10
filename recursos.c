/*
 * recursos.c - Almacén de RAM, tarjeta SD y descompresión. Ver recursos.h.
 *
 * Usa FatFs tal como lo genera CubeMX (modo "User-defined") con el driver
 * fatfs_sd.c sobre SPI1. Solo hay un archivo abierto a la vez (USERFile).
 */
#include <string.h>
#include "main.h"
#include "recursos.h"

extern SPI_HandleTypeDef hspi1;

/* ------------------------------------------------------------------------ */
/* Almacén de RAM                                                           */
/* ------------------------------------------------------------------------ */

/* uint64_t para que quede alineado a 8 bytes */
static uint64_t almacen[RAM_ALMACEN / 8];
static uint32_t usado;          /* bytes entregados */

void Ram_Liberar(void) {
	usado = 0;
}

void *Ram_Pedir(uint32_t n) {
	void *p;
	n = (n + 7u) & ~7u;           /* cada pedazo alineado a 8 */
	if (n > RAM_ALMACEN - usado)
		return NULL;
	p = (uint8_t *) almacen + usado;
	usado += n;
	return p;
}

uint32_t Ram_Usada(void) {
	return usado;
}

uint32_t Ram_Libre(void) {
	return RAM_ALMACEN - usado;
}

/* ------------------------------------------------------------------------ */
/* Tarjeta                                                                   */
/* ------------------------------------------------------------------------ */

static uint8_t montada;         /* 1 = la tarjeta ya está montada */
static uint8_t codigoFatFs;

uint8_t SD_CodigoFatFs(void) {
	return codigoFatFs;
}

void SD_Anotar(FRESULT r) {
	codigoFatFs = (uint8_t) r;
}

/* Cambia la velocidad del SPI1 (bits BR del registro CR1) */
static void VelocidadSPI(uint32_t prescaler) {
	__HAL_SPI_DISABLE(&hspi1);
	MODIFY_REG(hspi1.Instance->CR1, SPI_CR1_BR, prescaler);
	__HAL_SPI_ENABLE(&hspi1);
}

uint8_t SD_Montar(void) {
	uint32_t normal;
	FRESULT r;

	if (montada)
		return REC_OK;

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
		return REC_ERR_SD;
	montada = 1;
	return REC_OK;
}

uint8_t SD_Abrir(const char *nombre) {
	FRESULT r;
	if (SD_Montar() != REC_OK)
		return REC_ERR_SD;
	r = f_open(&USERFile, nombre, FA_READ);
	codigoFatFs = (uint8_t) r;
	if (r == FR_DISK_ERR || r == FR_NOT_READY)
		montada = 0;                    /* sacaron la tarjeta: volver a montar */
	return (r == FR_OK) ? REC_OK : REC_ERR_ABRIR;
}

uint8_t SD_Leer(void *dst, uint32_t n) {
	UINT leidos;
	FRESULT r = f_read(&USERFile, dst, n, &leidos);
	if (r != FR_OK) {
		codigoFatFs = (uint8_t) r;
		if (r == FR_DISK_ERR || r == FR_NOT_READY)
			montada = 0;
	}
	return (r == FR_OK && leidos == n) ? REC_OK : REC_ERR_DATOS;
}

uint8_t SD_Saltar(uint32_t posicion) {
	FRESULT r = f_lseek(&USERFile, posicion);
	if (r != FR_OK)
		codigoFatFs = (uint8_t) r;
	return (r == FR_OK && f_tell(&USERFile) == posicion) ? REC_OK : REC_ERR_DATOS;
}

void SD_Cerrar(void) {
	f_close(&USERFile);
}

/* Compara sin importar mayúsculas: ¿"nombre" termina en "ext"? */
static uint8_t TerminaEn(const char *nombre, const char *ext) {
	size_t a = strlen(nombre), b = strlen(ext);
	if (a <= b)
		return 0;
	nombre += a - b;
	for (size_t i = 0; i < b; i++) {
		char c = nombre[i];
		if (c >= 'a' && c <= 'z')
			c = (char) (c - 'a' + 'A');
		if (c != ext[i])
			return 0;
	}
	return 1;
}

uint8_t SD_Listar(const char *ext, char nombres[][13], uint8_t max) {
	static DIR dir;
	static FILINFO info;            /* con nombres largos ocupa ~280 bytes */
	uint8_t n = 0;
	FRESULT r;

	if (SD_Montar() != REC_OK)
		return 0;
	r = f_opendir(&dir, USERPath);
	codigoFatFs = (uint8_t) r;
	if (r != FR_OK) {
		montada = 0;
		return 0;
	}
	while (n < max) {
		const char *corto;
		r = f_readdir(&dir, &info);
		if (r != FR_OK || info.fname[0] == 0)
			break;                      /* error o fin del directorio */
		if (info.fattrib & (AM_DIR | AM_HID | AM_SYS))
			continue;
		/* La Mac crea archivos "._RYU.SPR" al copiar a la SD: no son del juego */
		if (info.fname[0] == '.')
			continue;
		/* El nombre 8.3 siempre sirve para abrirlo, aunque tenga nombre largo */
		corto = info.altname[0] ? info.altname : info.fname;
		if (strlen(corto) > 12 || !TerminaEn(corto, ext))
			continue;
		strcpy(nombres[n++], corto);
	}
	f_closedir(&dir);
	return n;
}

/* ------------------------------------------------------------------------ */
/* Descompresión                                                             */
/* ------------------------------------------------------------------------ */

uint16_t vistaPal[256];

uint8_t LZ_Descomprimir(const uint8_t *in, uint32_t tam, uint8_t *out, uint32_t n) {
	const uint8_t *finIn = in + tam;
	uint8_t *const inicio = out;
	uint8_t *const fin = out + n;
	uint8_t banderas = 0, bits = 0;

	while (out < fin) {
		if (bits == 0) {
			if (in >= finIn)
				return 0;
			banderas = *in++;
			bits = 8;
		}
		if (banderas & 1) {                          /* literal */
			if (in >= finIn)
				return 0;
			*out++ = *in++;
		} else {                                     /* copia */
			uint16_t v;
			uint32_t d, l;
			const uint8_t *src;
			if (in + 1 >= finIn)
				return 0;
			v = (uint16_t) (in[0] | (in[1] << 8));
			in += 2;
			d = (v & 0x3FFu) + 1u;
			l = (v >> 10) + 3u;
			if (d > (uint32_t) (out - inicio))
				return 0;                            /* apunta antes del inicio */
			if (l > (uint32_t) (fin - out))
				l = (uint32_t) (fin - out);
			src = out - d;
			while (l--)
				*out++ = *src++;                     /* byte a byte: puede traslaparse */
		}
		banderas >>= 1;
		bits--;
	}
	return 1;
}

/* Lectura del archivo abierto en pedazos pequeños */
static uint8_t  entrada[256];
static uint16_t entPos, entLen;
static uint8_t  entError;

static uint8_t LeerByte(void) {
	if (entPos == entLen) {
		UINT n = 0;
		if (f_read(&USERFile, entrada, sizeof entrada, &n) != FR_OK || n == 0) {
			entError = 1;
			return 0;
		}
		entPos = 0;
		entLen = (uint16_t) n;
	}
	return entrada[entPos++];
}

void LZ_FlujoIniciar(void) {
	entPos = entLen = 0;
	entError = 0;
}

uint8_t LZ_Flujo(uint32_t n, uint16_t w, uint16_t fila0, LZ_FnFila fila, void *ctx) {
	static uint8_t anillo[1024];        /* lo último descomprimido (la ventana) */
	static uint8_t renglon[320];
	uint32_t hechos = 0;
	uint16_t enRenglon = 0, k = fila0;
	uint8_t banderas = 0, bits = 0;

	if (w == 0 || w > sizeof renglon)
		return 0;

	while (hechos < n && !entError) {
		uint32_t l = 1, d = 0;
		if (bits == 0) {
			banderas = LeerByte();
			bits = 8;
		}
		if (!(banderas & 1)) {
			uint16_t v = LeerByte();
			v |= (uint16_t) (LeerByte() << 8);
			d = (v & 0x3FFu) + 1u;
			l = (v >> 10) + 3u;
			if (d > hechos)
				return 0;
		}
		banderas >>= 1;
		bits--;
		while (l-- && hechos < n) {
			uint8_t b = d ? anillo[(hechos - d) & 1023u] : LeerByte();
			anillo[hechos & 1023u] = b;
			hechos++;
			renglon[enRenglon++] = b;
			if (enRenglon == w) {
				fila(renglon, k++, ctx);
				enRenglon = 0;
			}
		}
	}
	if (enRenglon > 0 && !entError)
		fila(renglon, k, ctx);          /* última fila incompleta */
	return !entError;
}

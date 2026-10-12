/*
 * personajes_flash.c - Personajes guardados en la flash (comprimidos).
 * Generado por herramientas/spr_a_c.py; se puede editar a mano.
 *
 * Cada renglón es un personaje del menú, en este orden. "nombres" es lo
 * que se ve en pantalla, uno por cada versión de color (máximo 15
 * letras, en MAYÚSCULAS y sin acentos ni Ñ). Si se deja NULL se usa el
 * nombre que trae el archivo.
 */
#include "personajes_flash.h"

extern const uint8_t pj_deejay[39339];
extern const uint8_t pj_chunglee[22409];
extern const uint8_t pj_cammy[28452];
extern const uint8_t pj_ryu[35205];

const PersonajeFlash PERSONAJES_FLASH[] = {
	{ "DEEJAY.SPR", pj_deejay, sizeof pj_deejay, { "ANGEL" } },
	{ "CHUNGLEE.SPR", pj_chunglee, sizeof pj_chunglee, { "CHUNG LEE" } },
	{ "CAMMY.SPR", pj_cammy, sizeof pj_cammy, { "CAMMY" } },
	{ "RYU.SPR", pj_ryu, sizeof pj_ryu, { "RYU", "RYU AZUL" } },
};

const uint8_t N_PERSONAJES_FLASH = sizeof PERSONAJES_FLASH / sizeof PERSONAJES_FLASH[0];

/*
 * game.h
 *
 * Motor del videojuego de pelea (Proyecto 2, IE3054).
 * Copiar en Core/Inc.
 */

#ifndef INC_GAME_H_
#define INC_GAME_H_

#include <stdint.h>

/* Bits de la máscara de botones de cada jugador.
 * Es el mismo formato que envía el ESP32 receptor: 0xFF, J1, J2
 * (el bit 7 siempre va en 0, así 0xFF solo puede ser la cabecera). */
#define BTN_ARRIBA  0x01
#define BTN_ABAJO   0x02
#define BTN_IZQ     0x04
#define BTN_DER     0x08
#define BTN_A       0x10   /* golpe  */
#define BTN_B       0x20   /* patada */

/* IDs de sonido que se envían a la Nucleo 2 (1 byte por UART) */
enum {
	SND_SILENCIO = 0,
	SND_MENU,
	SND_PELEA,
	SND_GOLPE,
	SND_KO,
	SND_VICTORIA
};

/* Llamar una vez, después de LCD_Init() */
void Game_Init(void);

/* Llamar en cada vuelta del while(1). No bloquea: solo trabaja
 * cuando ya pasó un tick de 33 ms. */
void Game_Update(void);

/* Entradas (se pueden llamar desde la interrupción de UART) */
void Game_RxByte(uint8_t b);    /* byte recibido del ESP32 receptor       */
void Game_TeclaPC(uint8_t c);   /* tecla recibida de la terminal de la PC */

/* Ganchos: están definidos como "weak" en game.c y no hacen nada.
 * Si se definen en otro archivo (por ejemplo main.c), se usa esa versión. */
void Game_Sonido(uint8_t id);         /* enviar el ID de sonido a la Nucleo 2 */
void Game_Resultado(uint8_t ganador); /* 1, 2 o 0 = empate; guardar en la SD  */

#endif /* INC_GAME_H_ */

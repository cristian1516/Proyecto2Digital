/*
 * Formato: 1 byte por píxel. Cada byte es un índice a la paleta; el índice 0
 * es transparente. Los cuadros van uno después de otro: el cuadro k empieza
 * en ryuCaminar + k * SPR_W * SPR_H, y cada cuadro va por filas, de arriba
 * hacia abajo y de izquierda a derecha. El personaje mira a la derecha.
 */
#ifndef INC_SPRITES_H_
#define INC_SPRITES_H_

#include <stdint.h>

#define SPR_W            72     /* ancho de un cuadro                    */
#define SPR_H            95     /* alto de un cuadro                     */
#define SPR_COLORES      21     /* entradas de la paleta (0 = transp.)   */
#define RYU_CAMINAR_N    6      /* cuadros de la animación de caminar    */

extern const uint8_t  ryuCaminar[RYU_CAMINAR_N * SPR_W * SPR_H];
extern const uint16_t ryuPaleta[2][SPR_COLORES];   /* [0] = J1, [1] = J2 */

#endif /* INC_SPRITES_H_ */

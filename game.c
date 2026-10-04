/*
 * game.c
 *
 * Copiar en Core/Src.
 *
 * Estructura:
 *   - Game_Update() corre un tick fijo de 33 ms sin bloquear (no usa HAL_Delay).
 *   - Máquina de estados del juego: MENU -> PELEA -> GANADOR -> MENU.
 *   - Cada peleador tiene su propia máquina de estados.
 *   - Las colisiones usan AABB (cajas alineadas a los ejes), como en la clase.
 *   - El dibujado solo repinta lo que cambió: se borra la franja que el
 *     peleador dejó descubierta y se vuelve a pintar su caja.
 *
 * Los peleadores se dibujan con sprites (sprites.c / sprites.h): 1 byte por
 * píxel con paleta, y el índice 0 es transparente. Cada jugador tiene su
 * personaje (tabla PERSONAJE) y cada personaje su propio tamaño de cuadro.
 * Ver DibujarSprite() y DibujarCuerpo().
 */

#include "game.h"
#include "ili9341.h"   /* también incluye main.h (HAL) */
#include "sprites.h"

/* Escenario: imagen RGB565 de FONDO_W x FONDO_H definida en Bitmaps.h. En
 * pantalla empieza en la fila FONDO_Y; arriba y abajo queda negro. */
extern const uint16_t Escenario2[];
#define FONDO_W        320
#define FONDO_H        220
#define FONDO_Y        10

/* ------------------------------------------------------------------------ */
/* Configuración                                                             */
/* ------------------------------------------------------------------------ */
#define ANCHO          320
#define ALTO           240
#define TICK_MS        33        /* ~30 cuadros por segundo                  */
#define TICKS_SEGUNDO  30

#define PISO_Y         200       /* y donde empieza el piso                  */
#define X_INICIAL      30        /* separación inicial del borde de pantalla */
#define TICKS_CUADRO   3         /* ticks que dura cada cuadro de animación  */
#define KO_H           20        /* alto del peleador caído                  */
#define VEL_X          4         /* px por tick al caminar                   */
#define VEL_SALTO      (-14)     /* velocidad inicial del salto (px/tick)    */
#define GRAVEDAD       2         /* px/tick^2                                */
#define VIDA_MAX       100
#define TIEMPO_RONDA   60        /* segundos                                 */

#define BARRA_W        120
#define BARRA_H        10
#define BARRA_Y        10

#define T_TECLA_MS     150       /* cuánto "dura presionada" una tecla de PC */
#define T_UART_MS      300       /* sin tramas del ESP32 -> botones en 0     */

/* Colores RGB565 */
#define C_NEGRO        0x0000
#define C_BLANCO       0xFFFF
#define C_AMARILLO     0xFFE0
#define C_FONDO        0x5D9F    /* cielo */
#define C_PISO         0x8A22
#define C_J1           0xF800
#define C_J2           0x001F
#define C_GRIS         0x7BEF
#define C_VIDA         0x07E0
#define C_VIDA_FONDO   0x8000

/* ------------------------------------------------------------------------ */
/* Tipos                                                                     */
/* ------------------------------------------------------------------------ */
typedef enum { EST_MENU, EST_PELEA, EST_GANADOR } EstadoJuego;

typedef enum {
	P_QUIETO, P_CAMINA, P_SALTO, P_GOLPE, P_PATADA, P_DANO, P_KO
} EstadoPeleador;

/* Caja alineada a los ejes: esquina superior izquierda, ancho y alto */
typedef struct { int16_t x, y, w, h; } Caja;

typedef struct {
	int16_t  x, y;           /* esquina superior izquierda del cuerpo        */
	int16_t  vy;             /* velocidad vertical                           */
	int8_t   dir;            /* +1 mira a la derecha, -1 a la izquierda      */
	uint8_t  estado;         /* EstadoPeleador                               */
	uint8_t  t;              /* ticks que lleva en el estado actual          */
	uint8_t  conecto;        /* el ataque actual ya hizo daño                */
	int16_t  vida;
	uint8_t  entrada;        /* máscara de botones en este tick              */
	uint8_t  entradaPrev;    /* máscara del tick anterior (para flancos)     */
	const Personaje *pj;     /* sprite y tamaño del cuerpo (ver sprites.h)   */
	/* Lo que hay pintado en pantalla, para saber qué borrar y qué repintar */
	Caja     cuerpoDib;
	Caja     brazoDib;
	uint8_t  aspectoDib;
	int16_t  vidaDib;
	uint8_t  sucio;          /* 1 = hay que repintarlo completo              */
} Peleador;

/* Datos de cada ataque. Los ticks "ini..fin" son los cuadros activos:
 * solo ahí existe la caja de ataque y puede hacer daño. */
typedef struct {
	uint8_t duracion, ini, fin;
	uint8_t alcance, alto;   /* tamaño de la caja de ataque                  */
	uint8_t dy;              /* altura del ataque: % del alto del cuerpo,
	                            medido desde arriba                          */
	uint8_t dano;
} Ataque;

static const Ataque ATAQUES[2] = {
	/* duracion ini fin alcance alto dy(%) dano */
	{  8,       2,  4,  22,     10,  30,    8 },   /* golpe  (A) */
	{ 12,       4,  7,  30,     12,  59,   14 },   /* patada (B) */
};

/* Personaje de cada jugador. Para cambiarlo basta con cambiar esta tabla
 * (personajes disponibles: ryu, ryuAzul, deeJay). */
static const Personaje *const PERSONAJE[2] = { &ryu, &deeJay };

/* ------------------------------------------------------------------------ */
/* Variables                                                                 */
/* ------------------------------------------------------------------------ */
static EstadoJuego estadoJuego;
static Peleador    pl[2];
static uint32_t    ultimoTick;
static uint16_t    ticksEstado;      /* ticks desde que se entró al estado   */
static uint8_t     tiempo, tiempoDib;
static uint8_t     finRonda;         /* 1 = ya hay ganador, esperando        */
static uint16_t    ticksFin;
static uint8_t     ganador;          /* 1, 2 o 0 = empate                    */
static uint8_t     prevMenu;         /* botones del tick anterior en menús   */
static uint8_t     demo;             /* 1 = pelea automática de demostración */
static uint32_t    semilla = 12345;

/* Cajas del tick actual (las usa el dibujado) */
static Caja cajaCuerpo[2], cajaBrazo[2];

/* Entradas: se escriben desde interrupciones, se leen en el tick */
static volatile uint8_t  entUart[2];
static volatile uint32_t ultimoRx;
static volatile uint32_t teclaExpira[2][6];
static volatile uint8_t  entradaReal;   /* ya llegó algo de un mando o la PC */

/* ------------------------------------------------------------------------ */
/* Ganchos (se pueden redefinir en main.c)                                   */
/* ------------------------------------------------------------------------ */
__weak void Game_Sonido(uint8_t id)         { (void) id; }
__weak void Game_Resultado(uint8_t ganador) { (void) ganador; }

/* ------------------------------------------------------------------------ */
/* Entradas                                                                  */
/* ------------------------------------------------------------------------ */

/* Protocolo del ESP32 receptor: 0xFF, J1, J2 */
void Game_RxByte(uint8_t b) {
	static uint8_t fase = 0, j1 = 0;
	if (b == 0xFF) {
		fase = 1;
	} else if (fase == 1) {
		j1 = b;
		fase = 2;
	} else if (fase == 2) {
		entUart[0] = j1 & 0x3F;
		entUart[1] = b & 0x3F;
		ultimoRx = HAL_GetTick();
		entradaReal = 1;
		fase = 0;
	}
}

/* Teclado de la PC para probar sin mandos.
 * J1: w a s d, f = A, g = B.   J2: i j k l, o = A, p = B.
 * La terminal solo avisa cuando se presiona, no cuando se suelta, así que
 * cada tecla cuenta como presionada durante T_TECLA_MS. */
void Game_TeclaPC(uint8_t c) {
	static const char teclas[2][6] = {
		{ 'w', 's', 'a', 'd', 'f', 'g' },
		{ 'i', 'k', 'j', 'l', 'o', 'p' },
	};
	for (uint8_t j = 0; j < 2; j++) {
		for (uint8_t b = 0; b < 6; b++) {
			if (c == teclas[j][b]) {
				teclaExpira[j][b] = HAL_GetTick() + T_TECLA_MS;
				entradaReal = 1;
			}
		}
	}
}

static uint8_t LeerEntrada(uint8_t j) {
	uint32_t ahora = HAL_GetTick();
	uint8_t m = 0;
	if (ahora - ultimoRx < T_UART_MS)
		m = entUart[j];
	for (uint8_t b = 0; b < 6; b++) {
		if ((int32_t) (teclaExpira[j][b] - ahora) > 0)
			m |= (uint8_t) (1 << b);
	}
	return m;
}

/* ------------------------------------------------------------------------ */
/* Cajas y colisiones                                                        */
/* ------------------------------------------------------------------------ */

/* AABB de la clase, hay colisión si las cajas se traslapan en X y en Y.
 * Con >= y <=, tocarse por el borde también cuenta. */
static uint8_t Colision(Caja a, Caja b) {
	return (a.x + a.w >= b.x) && (a.x <= b.x + b.w)
	    && (a.y + a.h >= b.y) && (a.y <= b.y + b.h);
}

/* Igual que Colision pero más estricta comparten al menos un píxel.
 * Se usa solo para el dibujado. */
static uint8_t Solapa(Caja a, Caja b) {
	return a.w > 0 && b.w > 0
	    && (a.x + a.w > b.x) && (a.x < b.x + b.w)
	    && (a.y + a.h > b.y) && (a.y < b.y + b.h);
}

static uint8_t Igual(Caja a, Caja b) {
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static Caja CajaCuerpo(const Peleador *p) {
	Caja c = { p->x, p->y, p->pj->w, p->pj->h };
	if (p->estado == P_KO) {
		c.y = PISO_Y - KO_H;
		c.h = KO_H;
	}
	return c;
}

/* Caja de ataque. Devuelve w = 0 si no hay ataque activo en este tick. */
static Caja CajaAtaque(const Peleador *p) {
	Caja c = { 0, 0, 0, 0 };
	if (p->estado == P_GOLPE || p->estado == P_PATADA) {
		const Ataque *a = &ATAQUES[p->estado - P_GOLPE];
		if (p->t >= a->ini && p->t <= a->fin) {
			c.w = a->alcance;
			c.h = a->alto;
			c.y = p->y + (p->pj->h * a->dy) / 100;
			c.x = (p->dir > 0) ? p->x + p->pj->w : p->x - a->alcance;
		}
	}
	return c;
}

/* Lógica de los peleadores                                                  */
static void Cambiar(Peleador *p, uint8_t estado) {
	p->estado = estado;
	p->t = 0;
	p->conecto = 0;
}

static void ActualizarPeleador(Peleador *p, const Peleador *o) {
	uint8_t in = p->entrada;
	uint8_t nuevo = in & (uint8_t) ~p->entradaPrev;   /* flancos de subida */
	int16_t nx = p->x;
	const int16_t suelo = PISO_Y - p->pj->h;

	p->entradaPrev = in;
	if (p->t < 255)
		p->t++;

	switch (p->estado) {
	case P_QUIETO:
	case P_CAMINA:
		p->dir = (o->x >= p->x) ? 1 : -1;      /* siempre mira al rival */
		if (nuevo & BTN_A) {
			Cambiar(p, P_GOLPE);
		} else if (nuevo & BTN_B) {
			Cambiar(p, P_PATADA);
		} else {
			if (in & BTN_IZQ) nx -= VEL_X;
			if (in & BTN_DER) nx += VEL_X;
			if (in & BTN_ARRIBA) {
				Cambiar(p, P_SALTO);
				p->vy = VEL_SALTO;
			} else {
				p->estado = (nx != p->x) ? P_CAMINA : P_QUIETO;
			}
		}
		break;

	case P_SALTO:
		if (in & BTN_IZQ) nx -= VEL_X;
		if (in & BTN_DER) nx += VEL_X;
		break;

	case P_GOLPE:
	case P_PATADA:
		if (p->t >= ATAQUES[p->estado - P_GOLPE].duracion)
			Cambiar(p, P_QUIETO);
		break;

	case P_DANO:
		if (p->t <= 4) nx -= 3 * p->dir;       /* retroceso */
		/* Si lo golpearon en el aire, sigue cayendo como salto: así no
		 * puede volver a saltar ni atacar antes de tocar el piso. */
		if (p->t >= 8) Cambiar(p, (p->y < suelo) ? P_SALTO : P_QUIETO);
		break;

	default:   /* P_KO */
		break;
	}

	/* Movimiento vertical: vale para cualquier estado que esté en el aire */
	if (p->y < suelo || p->vy < 0) {
		p->y += p->vy;
		p->vy += GRAVEDAD;
		if (p->y >= suelo) {
			p->y = suelo;
			p->vy = 0;
			if (p->estado == P_SALTO)
				Cambiar(p, P_QUIETO);
		}
	}

	/* Límites de la pantalla */
	if (nx < 0) nx = 0;
	if (nx > ANCHO - p->pj->w) nx = ANCHO - p->pj->w;

	/* Los peleadores no se atraviesan: si el movimiento en X lo metería
	 * dentro del rival, se cancela. */
	if (nx + p->pj->w > o->x && nx < o->x + o->pj->w)
		nx = p->x;
	p->x = nx;
}

/* ¿El ataque de "a" alcanza a "v" en este tick? */
/* Devuelve el daño que el ataque de "a" le hace a "v" en este tick, o 0 si
 * no conecta. */
static uint8_t Conecta(const Peleador *a, const Peleador *v) {
	Caja at = CajaAtaque(a);
	if (at.w > 0 && !a->conecto && v->estado != P_KO
			&& Colision(at, CajaCuerpo(v)))
		return ATAQUES[a->estado - P_GOLPE].dano;
	return 0;
}

/* El daño llega ya calculado si los dos se golpean en el mismo tick, al
 * aplicar el segundo golpe el atacante ya cambió de estado. */
static void AplicarGolpe(Peleador *a, Peleador *v, uint8_t dano) {
	a->conecto = 1;
	v->vida -= dano;
	if (v->vida <= 0) {
		v->vida = 0;
		Cambiar(v, P_KO);
		Game_Sonido(SND_KO);
	} else {
		Cambiar(v, P_DANO);
		Game_Sonido(SND_GOLPE);
	}
}

/* Jugador automático para el modo demostración */
static uint32_t Azar(void) {
	semilla = semilla * 1103515245u + 12345u;
	return semilla >> 16;
}

static uint8_t EntradaDemo(const Peleador *yo, const Peleador *otro) {
	uint8_t hacia = (otro->x > yo->x) ? BTN_DER : BTN_IZQ;
	uint8_t lejos = (otro->x > yo->x) ? BTN_IZQ : BTN_DER;
	/* Espacio libre entre las dos cajas (cada personaje tiene su ancho) */
	int16_t hueco = (otro->x > yo->x) ? otro->x - (yo->x + yo->pj->w)
	                                  : yo->x - (otro->x + otro->pj->w);
	uint32_t r = Azar();
	if (hueco > 20)
		return (r % 16 == 0) ? (hacia | BTN_ARRIBA) : hacia;
	switch (r % 8) {
	case 0:  return BTN_A;
	case 1:  return BTN_B;
	case 2:  return lejos;
	default: return 0;
	}
}

/* ------------------------------------------------------------------------ */
/* Dibujado                                                                  */
/* ------------------------------------------------------------------------ */

/* FillRect recortado a la pantalla (FillRect no revisa límites) */
static void Rellenar(Caja c, uint16_t color) {
	if (c.x < 0) { c.w += c.x; c.x = 0; }
	if (c.y < 0) { c.h += c.y; c.y = 0; }
	if (c.x + c.w > ANCHO) c.w = ANCHO - c.x;
	if (c.y + c.h > ALTO)  c.h = ALTO - c.y;
	if (c.w <= 0 || c.h <= 0)
		return;
	FillRect(c.x, c.y, c.w, c.h, color);
}

/* Color del escenario en el píxel (x, y) de la pantalla */
static uint16_t ColorFondo(int16_t x, int16_t y) {
	if (y < FONDO_Y || y >= FONDO_Y + FONDO_H)
		return C_NEGRO;
	/* uint32_t: el índice pasa de 65535 */
	return Escenario2[(uint32_t) (y - FONDO_Y) * FONDO_W + (uint32_t) x];
}

/* Pinta el pedazo del escenario que cae dentro de la caja c (recortada a la
 * pantalla). Con c = toda la pantalla dibuja el escenario completo. */
static void DibujarFondo(Caja c) {
	if (c.x < 0) { c.w += c.x; c.x = 0; }
	if (c.y < 0) { c.h += c.y; c.y = 0; }
	if (c.x + c.w > ANCHO) c.w = ANCHO - c.x;
	if (c.y + c.h > ALTO)  c.h = ALTO - c.y;
	if (c.w <= 0 || c.h <= 0)
		return;

	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
	SetWindows(c.x, c.y, c.x + c.w - 1, c.y + c.h - 1);
	for (int16_t j = 0; j < c.h; j++) {
		for (int16_t i = 0; i < c.w; i++) {
			uint16_t color = ColorFondo(c.x + i, c.y + j);
			LCD_DATA((uint8_t) (color >> 8));
			LCD_DATA((uint8_t) color);
		}
	}
	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

/* Borra una zona: vuelve a pintar ahí el escenario. Si la zona tocaba algo
 * que sigue en pantalla, lo marca para repintarlo. */
static void Borrar(Caja c) {
	DibujarFondo(c);
	for (uint8_t k = 0; k < 2; k++) {
		if (Solapa(c, cajaCuerpo[k]) || Solapa(c, cajaBrazo[k]))
			pl[k].sucio = 1;
	}
}

/* Borra solo la parte de "vieja" que "nueva" ya no cubre. Así un peleador
 * que avanza 4 px borra una franja de 4 px en vez de toda su caja. */
static void BorrarDiferencia(Caja vieja, Caja nueva) {
	if (!Solapa(vieja, nueva)) {
		Borrar(vieja);
		return;
	}
	int16_t vx2 = vieja.x + vieja.w, vy2 = vieja.y + vieja.h;
	int16_t nx2 = nueva.x + nueva.w, ny2 = nueva.y + nueva.h;
	if (nueva.x > vieja.x)                                   /* izquierda */
		Borrar((Caja) { vieja.x, vieja.y, nueva.x - vieja.x, vieja.h });
	if (nx2 < vx2)                                           /* derecha   */
		Borrar((Caja) { nx2, vieja.y, vx2 - nx2, vieja.h });
	if (nueva.y > vieja.y)                                   /* arriba    */
		Borrar((Caja) { vieja.x, vieja.y, vieja.w, nueva.y - vieja.y });
	if (ny2 < vy2)                                           /* abajo     */
		Borrar((Caja) { vieja.x, ny2, vieja.w, vy2 - ny2 });
}

/* Cuadro de animación que le toca al peleador en este tick. Por ahora solo
 * hay animación de caminar; en los demás estados se usa el cuadro 0. */
static uint8_t Cuadro(const Peleador *p) {
	uint8_t k, atras;
	if (p->estado != P_CAMINA)
		return 0;
	k = (uint8_t) ((ticksEstado / TICKS_CUADRO) % p->pj->cuadros);
	/* Si camina alejándose del rival, la animación corre al revés */
	atras = p->entrada & ((p->dir > 0) ? BTN_IZQ : BTN_DER);
	return atras ? (uint8_t) (p->pj->cuadros - 1 - k) : k;
}

/* Número que resume cómo se ve el peleador. Si cambia, hay que repintarlo. */
static uint8_t Aspecto(const Peleador *p) {
	uint8_t a = Cuadro(p);                       /* bits 0-3 */
	if (p->estado == P_DANO) a |= 0x10;
	if (p->estado == P_KO)   a |= 0x20;
	return a | (p->dir > 0 ? 0x80 : 0);
}

/* Dibuja un cuadro del personaje pj con su esquina superior izquierda en
 * (x, y).
 *   cuadro   número de cuadro de la animación (0 = el primero)
 *   voltear  1 = espejo horizontal (el sprite original mira a la derecha)
 *   silueta  1 = todo el personaje en blanco (cuando recibe un golpe)
 *
 * El índice 0 es transparente: ahí se pinta el píxel del escenario que
 * queda detrás. Como cada píxel de la caja se escribe una sola vez, el
 * personaje no parpadea.
 */
static void DibujarSprite(int16_t x, int16_t y, const Personaje *pj,
		uint8_t cuadro, uint8_t voltear, uint8_t silueta) {
	const int16_t w = pj->w, h = pj->h;
	const uint8_t *px = pj->px + (uint32_t) cuadro * w * h;
	const int8_t paso = voltear ? -1 : 1;

	if (x < 0 || y < 0 || x + w > ANCHO || y + h > ALTO)
		return;

	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
	SetWindows(x, y, x + w - 1, y + h - 1);
	for (int16_t j = 0; j < h; j++) {
		/* Volteado: la fila se lee de derecha a izquierda */
		const uint8_t *f = px + j * w + (voltear ? w - 1 : 0);
		for (int16_t i = 0; i < w; i++) {
			uint16_t c;
			if (*f == 0)
				c = ColorFondo(x + i, y + j);       /* transparente */
			else if (silueta)
				c = C_BLANCO;
			else
				c = pj->paleta[*f];
			f += paso;
			LCD_DATA((uint8_t) (c >> 8));
			LCD_DATA((uint8_t) c);
		}
	}
	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

/* Pinta el cuerpo del peleador dentro de la caja c. */
static void DibujarCuerpo(const Peleador *p, Caja c) {
	if (p->estado == P_KO) {          /* todavía no hay sprite de KO */
		Rellenar(c, C_GRIS);
		return;
	}
	DibujarSprite(c.x, c.y, p->pj, Cuadro(p), p->dir < 0,
			p->estado == P_DANO);
}

static void DibujarVida(uint8_t i) {
	Peleador *p = &pl[i];
	int16_t lleno = (int16_t) (p->vida * BARRA_W / VIDA_MAX);
	int16_t x0 = (i == 0) ? 10 : ANCHO - 10 - BARRA_W;
	/* Las dos barras se vacían hacia el centro de la pantalla */
	if (i == 0) {
		Rellenar((Caja) { x0, BARRA_Y, lleno, BARRA_H }, C_VIDA);
		Rellenar((Caja) { x0 + lleno, BARRA_Y, BARRA_W - lleno, BARRA_H }, C_VIDA_FONDO);
	} else {
		Rellenar((Caja) { x0, BARRA_Y, BARRA_W - lleno, BARRA_H }, C_VIDA_FONDO);
		Rellenar((Caja) { x0 + BARRA_W - lleno, BARRA_Y, lleno, BARRA_H }, C_VIDA);
	}
	p->vidaDib = p->vida;
}

static void DibujarTiempo(void) {
	char txt[3] = { (char) ('0' + tiempo / 10), (char) ('0' + tiempo % 10), 0 };
	LCD_Print(txt, ANCHO / 2 - 16, BARRA_Y - 3, 2, C_BLANCO, C_FONDO);
	tiempoDib = tiempo;
}

/* Repinta solo lo que cambió respecto al tick anterior */
static void DibujarPelea(void) {
	uint8_t i, algo = 0;

	for (i = 0; i < 2; i++) {
		cajaCuerpo[i] = CajaCuerpo(&pl[i]);
		cajaBrazo[i] = CajaAtaque(&pl[i]);
	}

	/* 1. Borrar lo que quedó descubierto */
	for (i = 0; i < 2; i++) {
		Peleador *p = &pl[i];
		if (p->brazoDib.w > 0 && !Igual(p->brazoDib, cajaBrazo[i]))
			Borrar(p->brazoDib);
		if (p->cuerpoDib.w > 0 && !Igual(p->cuerpoDib, cajaCuerpo[i]))
			BorrarDiferencia(p->cuerpoDib, cajaCuerpo[i]);
	}

	/* 2. Cuerpos que cambiaron */
	for (i = 0; i < 2; i++) {
		Peleador *p = &pl[i];
		uint8_t asp = Aspecto(p);
		if (p->sucio || asp != p->aspectoDib || !Igual(p->cuerpoDib, cajaCuerpo[i])) {
			DibujarCuerpo(p, cajaCuerpo[i]);
			algo = 1;
		}
		p->aspectoDib = asp;
	}

	/* 3. Brazos/piernas de ataque: van encima de los cuerpos, así que se
	 *    repintan si se pintó cualquier cosa en este tick. */
	for (i = 0; i < 2; i++) {
		Peleador *p = &pl[i];
		if (cajaBrazo[i].w > 0 && (algo || !Igual(p->brazoDib, cajaBrazo[i]))) {
			Rellenar(cajaBrazo[i], C_AMARILLO);
			algo = 1;
		}
		p->cuerpoDib = cajaCuerpo[i];
		p->brazoDib = cajaBrazo[i];
		p->sucio = 0;
	}

	/* 4. Marcador */
	for (i = 0; i < 2; i++) {
		if (pl[i].vida != pl[i].vidaDib)
			DibujarVida(i);
	}
	if (tiempo != tiempoDib)
		DibujarTiempo();
}

/* ------------------------------------------------------------------------ */
/* Estados del juego                                                         */
/* ------------------------------------------------------------------------ */
static void EntrarMenu(void) {
	estadoJuego = EST_MENU;
	ticksEstado = 0;
	prevMenu = 0xFF;      /* evita que un botón ya presionado cuente */
	LCD_Clear(C_NEGRO);
	LCD_Print("STREET FIGHTER", 48, 70, 2, C_AMARILLO, C_NEGRO);
	LCD_Print("J1 vs J2", 96, 100, 2, C_BLANCO, C_NEGRO);
	Game_Sonido(SND_MENU);
}

static void EntrarPelea(void) {
	estadoJuego = EST_PELEA;
	ticksEstado = 0;
	finRonda = 0;
	ticksFin = 0;
	tiempo = TIEMPO_RONDA;
	demo = !entradaReal;
	semilla += HAL_GetTick();

	for (uint8_t i = 0; i < 2; i++) {
		Peleador *p = &pl[i];
		p->pj = PERSONAJE[i];
		p->x = (i == 0) ? X_INICIAL : ANCHO - X_INICIAL - p->pj->w;
		p->y = PISO_Y - p->pj->h;
		p->vy = 0;
		p->dir = (i == 0) ? 1 : -1;
		p->vida = VIDA_MAX;
		p->entrada = 0;
		p->entradaPrev = 0xFF;
		p->cuerpoDib = (Caja) { 0, 0, 0, 0 };
		p->brazoDib = (Caja) { 0, 0, 0, 0 };
		p->sucio = 1;
		Cambiar(p, P_QUIETO);
	}

	/* Escenario completo. Se pinta con la misma función que después lo
	 * restaura por pedazos, para que coincidan píxel a píxel. */
	DibujarFondo((Caja) { 0, 0, ANCHO, ALTO });

	DibujarVida(0);
	DibujarVida(1);
	DibujarTiempo();
	DibujarPelea();
	Game_Sonido(SND_PELEA);
}

static void EntrarGanador(void) {
	estadoJuego = EST_GANADOR;
	ticksEstado = 0;
	prevMenu = 0xFF;
	LCD_Clear(C_NEGRO);
	if (ganador == 0)
		LCD_Print("EMPATE", 112, 90, 2, C_BLANCO, C_NEGRO);
	else
		LCD_Print(ganador == 1 ? "JUGADOR 1 GANA" : "JUGADOR 2 GANA", 48, 90, 2,
				ganador == 1 ? C_J1 : C_J2, C_NEGRO);
	LCD_Print("A: MENU", 104, 150, 2, C_BLANCO, C_NEGRO);
	Game_Sonido(SND_VICTORIA);
	if (!demo)
		Game_Resultado(ganador);
}

/* Devuelve 1 si algún jugador acaba de presionar A */
static uint8_t PresionaronA(void) {
	uint8_t in = LeerEntrada(0) | LeerEntrada(1);
	uint8_t nuevo = in & (uint8_t) ~prevMenu;
	prevMenu = in;
	return (nuevo & BTN_A) != 0;
}

static void TickMenu(void) {
	/* Texto parpadeante */
	if (ticksEstado % 30 == 1)
		LCD_Print("PRESIONA A", 80, 160, 2, C_BLANCO, C_NEGRO);
	else if (ticksEstado % 30 == 16)
		LCD_Print("          ", 80, 160, 2, C_BLANCO, C_NEGRO);

	if (PresionaronA())
		EntrarPelea();
	else if (!entradaReal && ticksEstado > 3 * TICKS_SEGUNDO)
		EntrarPelea();    /* nadie ha conectado un mando: demostración */
}

static void TickPelea(void) {
	uint8_t i;

	/* Si alguien conecta un mando durante la demostración, volver al menú */
	if (demo && entradaReal) {
		EntrarMenu();
		return;
	}

	/* 1. Entradas */
	for (i = 0; i < 2; i++) {
		if (finRonda)
			pl[i].entrada = 0;
		else if (demo)
			pl[i].entrada = EntradaDemo(&pl[i], &pl[1 - i]);
		else
			pl[i].entrada = LeerEntrada(i);
	}

	/* 2. Movimiento y estados */
	ActualizarPeleador(&pl[0], &pl[1]);
	ActualizarPeleador(&pl[1], &pl[0]);

	/* 3. Golpes: primero se revisan los dos y después se aplican, para que
	 *    dos golpes simultáneos se conecten ambos. */
	uint8_t g0 = Conecta(&pl[0], &pl[1]);
	uint8_t g1 = Conecta(&pl[1], &pl[0]);
	if (g0) AplicarGolpe(&pl[0], &pl[1], g0);
	if (g1) AplicarGolpe(&pl[1], &pl[0], g1);

	/* 4. Reloj y fin de ronda */
	if (!finRonda) {
		if (ticksEstado % TICKS_SEGUNDO == 0 && tiempo > 0)
			tiempo--;
		if (pl[0].estado == P_KO || pl[1].estado == P_KO || tiempo == 0) {
			finRonda = 1;
			if (pl[0].vida == pl[1].vida)
				ganador = 0;
			else
				ganador = (pl[0].vida > pl[1].vida) ? 1 : 2;
		}
	} else if (++ticksFin > 2 * TICKS_SEGUNDO) {
		EntrarGanador();
		return;
	}

	/* 5. Pantalla */
	DibujarPelea();
}

static void TickGanador(void) {
	if (PresionaronA() || (demo && ticksEstado > 3 * TICKS_SEGUNDO))
		EntrarMenu();
}

/* ------------------------------------------------------------------------ */
/* Funciones públicas                                                        */
/* ------------------------------------------------------------------------ */
void Game_Init(void) {
	ultimoTick = HAL_GetTick();
	EntrarMenu();
}

void Game_Update(void) {
	uint32_t ahora = HAL_GetTick();
	if (ahora - ultimoTick < TICK_MS)
		return;             /* todavía no toca*/
	ultimoTick = ahora;
	ticksEstado++;

	switch (estadoJuego) {
	case EST_MENU:    TickMenu();    break;
	case EST_PELEA:   TickPelea();   break;
	case EST_GANADOR: TickGanador(); break;
	}
}

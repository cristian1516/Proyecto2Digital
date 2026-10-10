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
 *   - El dibujado solo repinta las zonas de la pantalla donde algo cambió.
 *
 * Controles: flechas para caminar y saltar, A golpe, B patada, y mantener
 * ABAJO para cubrirse (un golpe cubierto no quita vida).
 *
 * Menús: TITULO -> ELEGIR PERSONAJES -> ELEGIR ESCENARIO -> PELEA. En los
 * menús, IZQ/DER cambian, A elige y B regresa. Cada jugador elige su
 * personaje con su propio control.
 *
 * Los peleadores se dibujan con sprites (sprites.c / sprites.h): 1 byte por
 * píxel con paleta, y el índice 0 es transparente. Cada jugador tiene su
 * personaje (tabla PERSONAJE). Un personaje tiene dos cajas distintas:
 *
 *   - El CUERPO (cuerpoW x cuerpoH): es la caja de las colisiones. Con ella
 *     se decide si un golpe conecta y que los peleadores no se atraviesen.
 *   - El CUADRO que se dibuja: cambia de tamaño con la animación y es más
 *     ancho que el cuerpo (en el golpe y la patada, mucho más).
 *
 * La pantalla se arma por capas (escenario, un peleador, el otro), ver
 * Repintar(). Así el puño de uno puede pasar por encima del otro.
 *
 * Personajes (.SPR) y escenarios (.ESC) están en la microSD, comprimidos.
 * Al terminar de elegir se copian a la RAM solo los elegidos (personajes.c,
 * fondo.c, recursos.c) y durante la pelea ya no se toca la tarjeta. Si en
 * la siguiente pelea se elige lo mismo, no se vuelve a leer nada.
 *
 * Cada jugador tiene un búfer donde se descomprime el cuadro que le toca
 * dibujar; solo se descomprime cuando el cuadro cambia.
 */

#include <stddef.h>
#include <string.h>
#include "game.h"
#include "ili9341.h"     /* también incluye main.h (HAL) */
#include "sprites.h"
#include "recursos.h"    /* almacén de RAM y tarjeta SD */
#include "personajes.h"  /* personajes: de la microSD a la RAM */
#include "fondo.h"       /* escenario: de la microSD a la RAM */

/* Cuántos archivos de cada tipo se muestran en el menú. Para agregar un
 * personaje o un escenario basta con copiar su archivo a la raíz de la SD. */
#define MAX_PJ         8     /* archivos .SPR                           */
#define MAX_OP         12    /* opciones del menú: una por cada versión de
                                color de cada personaje (RYU, RYU AZUL...) */
#define MAX_ESC        8


/* Configuración                                                             */

#define ANCHO          320
#define ALTO           240
#define TICK_MS        33        /* ~30 cuadros por segundo                  */
#define TICKS_SEGUNDO  30

#define PISO_NORMAL    200       /* y de los pies si el escenario no la trae */
#define X_INICIAL      20        /* al empezar, cuánto se separa cada peleador
                                    de su tope en el borde de la pantalla    */
#define TICKS_CUADRO   3         /* ticks que dura cada cuadro de animación  */
#define KO_H           20        /* alto del rectángulo de KO, para personajes
                                    que no tienen sprite de KO               */
#define TICKS_KO       4         /* ticks que dura cada cuadro de la caída   */
#define VEL_KO         4         /* px por tick que retrocede mientras cae   */
#define TICKS_DANO     8         /* ticks que dura el golpe recibido         */
#define TICKS_EMPUJE   4         /* ticks que retrocede al cubrir un golpe   */
#define VEL_EMPUJE     2         /* px por tick de ese retroceso             */
#define VEL_X          4         /* px por tick al caminar                   */
#define VEL_SALTO      (-14)     /* velocidad inicial del salto (px/tick)    */
#define GRAVEDAD       2         /* px/tick^2                                */
#define VIDA_MAX       100
#define TIEMPO_RONDA   60        /* segundos                                 */

#define BARRA_W        120
#define BARRA_H        10
#define BARRA_Y        10
#define MARCADOR_H     24        /* franja de arriba: barras de vida y reloj */

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
#define C_LISTO        0x07E0


/* Tipos                                                                     */

typedef enum {
	EST_TITULO, EST_ELEGIR_PJ, EST_ELEGIR_ESC, EST_AVISO, EST_PELEA, EST_GANADOR
} EstadoJuego;

typedef enum {
	P_QUIETO, P_CAMINA, P_SALTO, P_GOLPE, P_PATADA, P_DANO, P_KO,
	P_CUBRE     /* cubriéndose: mientras se mantenga ABAJO */
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
	uint8_t  empuje;         /* ticks de retroceso que le quedan por haber
	                            cubierto un golpe                            */
	int16_t  vida;
	uint8_t  entrada;        /* máscara de botones en este tick              */
	uint8_t  entradaPrev;    /* máscara del tick anterior (para flancos)     */
	const Personaje *pj;     /* sprites y tamaño del cuerpo (ver sprites.h)  */
	int16_t  vidaDib;        /* vida que muestra la barra en pantalla        */
} Peleador;

/* Tiempos y daño de cada ataque, iguales para todos los personajes. Los
 * ticks "ini..fin" son los activos: solo ahí el ataque puede hacer daño. La
 * zona que golpea es de cada personaje (ZonaGolpe, en sprites.h). */
typedef struct {
	uint8_t duracion, ini, fin;
	uint8_t dano;
} Ataque;

static const Ataque ATAQUES[2] = {
	/* duracion ini fin dano */
	{  8,       2,  4,   8 },   /* golpe  (A) */
	{ 12,       4,  7,  14 },   /* patada (B) */
};

/* Una capa es algo que se ve en pantalla: un cuadro de sprite o un
 * rectángulo de un solo color. Cada peleador tiene dos. */
typedef struct {
	Caja caja;                /* zona de la pantalla que ocupa; w = 0: nada  */
	const Cuadro   *cuadro;   /* cuadro que se ve (para saber si cambió)     */
	const uint8_t  *px;       /* píxeles ya descomprimidos; NULL = rectángulo liso */
	const uint16_t *paleta;
	uint16_t color;           /* color del rectángulo, o de la silueta       */
	uint8_t  voltear;         /* 1 = espejo horizontal (mira a la izquierda) */
	uint8_t  silueta;         /* 1 = el sprite completo de un solo color         */
} Capa;

enum {
	CUERPO,     /* el sprite del peleador                                      */
	BRAZO       /* rectángulo del ataque, para personajes sin sprite de ataque */
};

/* Archivos que hay en la SD y el nombre que se muestra de cada uno */
static char    archPj[MAX_PJ][13];
static uint8_t nPalArch[MAX_PJ];   /* versiones de color de cada archivo     */
static char    archEsc[MAX_ESC][13], nomEsc[MAX_ESC][16];
static uint8_t nPj, nEsc;

/* Opciones del menú de personajes: archivo y paleta de cada una */
static char    nomOp[MAX_OP][16];
static uint8_t opArch[MAX_OP], opPal[MAX_OP];
static uint8_t nOp;

/* Lo elegido en el menú */
static uint8_t eleccion[2];        /* opción de cada jugador (índice)      */
static uint8_t listo[2];           /* el jugador ya confirmó su personaje  */
static uint8_t escElegido;
static uint8_t ticksListos;        /* los dos listos: pausa antes de seguir */

/* Lo que está cargado en la RAM: archivo de cada jugador y escenario.
 * 0xFF = nada. Si se elige lo mismo otra vez no se vuelve a leer la SD. */
static uint8_t   cargado[3] = { 0xFF, 0xFF, 0xFF };
static Personaje pjCargado[2];

/* Búfer de cada jugador donde se descomprime el cuadro que se dibuja */
static uint8_t      *bufCuadro[2];
static const Cuadro *cuadroEnBuf[2];   /* cuál cuadro tiene ahora */

/* Mensaje de la pantalla de aviso (errores al cargar) */
static char avisoTxt[4][41];

/* Variables                                                                 */

static EstadoJuego estadoJuego;
static Peleador    pl[2];
static int16_t     pisoY = PISO_NORMAL;  /* y donde se paran los peleadores  */
static uint32_t    ultimoTick;
static uint16_t    ticksEstado;      /* ticks desde que se entró al estado   */
static uint8_t     tiempo, tiempoDib;
static uint8_t     finRonda;         /* 1 = ya hay ganador, esperando        */
static uint16_t    ticksFin;
static uint8_t     ganador;          /* 1, 2 o 0 = empate                    */
static uint8_t     prevMenu;         /* botones del tick anterior en menús   */
static uint8_t     prevJ[2];         /* lo mismo, por jugador                */
static uint8_t     demo;             /* 1 = pelea automática de demostración */
static uint32_t    semilla = 12345;

/* Lo que hay pintado en pantalla ahora mismo: las capas de cada peleador y
 * cuál de los dos va encima del otro. */
static Capa    capa[2][2];
static uint8_t encima;

/* Zonas de la pantalla que hay que repintar en este tick */
#define MAX_ZONAS  9          /* 4 capas x (donde estaba + donde está) + 1 */
static Caja    zonas[MAX_ZONAS];
static uint8_t nZonas;

/* Una fila de píxeles ya armada, lista para mandarla a la pantalla */
static uint16_t fila[ANCHO];

/* Entradas: se escriben desde interrupciones, se leen en el tick */
static volatile uint8_t  entUart[2];
static volatile uint32_t ultimoRx;
static volatile uint32_t teclaExpira[2][6];
static volatile uint8_t  entradaReal;   /* ya llegó algo de un mando o la PC */


/* Ganchos (se pueden redefinir en main.c)                                   */

__weak void Game_Sonido(uint8_t id)         { (void) id; }
__weak void Game_Resultado(uint8_t ganador) { (void) ganador; }


/* Entradas                                                                  */


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

/* Cajas y colisiones                                                        */


/* AABB de la clase, hay colisión si las cajas se traslapan en X y en Y.
 * Con >= y <=, tocarse por el borde también cuenta. */
static uint8_t Colision(Caja a, Caja b) {
	return (a.x + a.w >= b.x) && (a.x <= b.x + b.w)
	    && (a.y + a.h >= b.y) && (a.y <= b.y + b.h);
}

static uint8_t Igual(Caja a, Caja b) {
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* ¿El peleador está dando un golpe o una patada? */
static uint8_t Atacando(const Peleador *p) {
	return p->estado == P_GOLPE || p->estado == P_PATADA;
}

/* Caja del cuerpo: la que cuenta para las colisiones. (x, y) del peleador
 * es su esquina superior izquierda. */
static Caja CajaCuerpo(const Peleador *p) {
	Caja c = { p->x, p->y, p->pj->cuerpoW, p->pj->cuerpoH };
	if (p->estado == P_KO) {
		c.y = pisoY - KO_H;
		c.h = KO_H;
	}
	return c;
}

/* Caja de ataque: la zona que golpea, delante del peleador. Devuelve w = 0
 * si no hay ataque activo en este tick. */
static Caja CajaAtaque(const Peleador *p) {
	Caja c = { 0, 0, 0, 0 };
	if (Atacando(p)) {
		const uint8_t k = p->estado - P_GOLPE;     /* 0 golpe, 1 patada */
		const Ataque *a = &ATAQUES[k];
		if (p->t >= a->ini && p->t <= a->fin) {
			const ZonaGolpe *z = &p->pj->zona[k];
			const int16_t centro = p->x + p->pj->cuerpoW / 2;
			c.w = z->w;
			c.h = z->h;
			c.y = p->y + z->dy;
			/* Mirando a la izquierda la zona queda del otro lado del centro */
			c.x = (p->dir > 0) ? centro + z->dx : centro - z->dx - z->w;
		}
	}
	return c;
}

/* Lógica de los peleadores                                                  */
static void Cambiar(Peleador *p, uint8_t estado) {
	p->estado = estado;
	p->t = 0;
	p->conecto = 0;
	p->empuje = 0;
}

static void ActualizarPeleador(Peleador *p, const Peleador *o) {
	uint8_t in = p->entrada;
	uint8_t nuevo = in & (uint8_t) ~p->entradaPrev;   /* flancos de subida */
	int16_t nx = p->x;
	const int16_t suelo = pisoY - p->pj->cuerpoH;
	/* El cuerpo no llega al borde de la pantalla: deja lugar para lo que el
	 * dibujo sobresale por detrás (ver "margen" en sprites.h). */
	const int16_t xMin = p->pj->margen;
	const int16_t xMax = ANCHO - p->pj->margen - p->pj->cuerpoW;

	p->entradaPrev = in;
	if (p->t < 255)
		p->t++;

	switch (p->estado) {
	case P_QUIETO:
	case P_CAMINA:
		p->dir = (o->x >= p->x) ? 1 : -1;      /* siempre mira al rival */
		if (in & BTN_ABAJO) {
			Cambiar(p, P_CUBRE);       /* se cubre mientras mantenga ABAJO */
		} else if (nuevo & BTN_A) {
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
		if (p->t >= TICKS_DANO) Cambiar(p, (p->y < suelo) ? P_SALTO : P_QUIETO);
		break;

	case P_CUBRE:
		/* No camina ni ataca. Si cubrió un golpe, retrocede un poco. */
		p->dir = (o->x >= p->x) ? 1 : -1;
		if (p->empuje > 0) {
			nx -= VEL_EMPUJE * p->dir;
			p->empuje--;
		}
		if (!(in & BTN_ABAJO))
			Cambiar(p, P_QUIETO);              /* soltó ABAJO */
		break;

	default:   /* P_KO */
		/* Sale despedido hacia atrás mientras duran los cuadros de la
		 * caída; con el último (tendido en el piso) ya no se mueve. */
		if (p->pj->ko.n > 1 && p->t < (p->pj->ko.n - 1) * TICKS_KO)
			nx -= VEL_KO * p->dir;
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
	if (nx < xMin) nx = xMin;
	if (nx > xMax) nx = xMax;

	/* Los peleadores no se atraviesan: si el movimiento en X metería su
	 * cuerpo dentro del cuerpo del rival, se cancela. */
	if (nx + p->pj->cuerpoW > o->x && nx < o->x + o->pj->cuerpoW)
		nx = p->x;
	p->x = nx;
}

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
	if (v->estado == P_CUBRE) {
		/* Golpe cubierto: no quita vida, solo lo empuja un poco hacia atrás */
		v->empuje = TICKS_EMPUJE;
		return;
	}
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
	/* Espacio libre entre los dos cuerpos (cada personaje tiene su ancho) */
	int16_t hueco = (otro->x > yo->x) ? otro->x - (yo->x + yo->pj->cuerpoW)
	                                  : yo->x - (otro->x + otro->pj->cuerpoW);
	uint32_t r;

	/* Cuando el rival empieza un ataque, decide si se cubre (la mitad de
	 * las veces) y mantiene esa decisión mientras dure el ataque. */
	static uint8_t cubrir[2];
	const uint8_t yoNum = (uint8_t) (yo - pl);
	if (Atacando(otro)) {
		if (otro->t == 0)
			cubrir[yoNum] = (Azar() % 2 == 0);
		if (cubrir[yoNum] && hueco <= 30)
			return BTN_ABAJO;
	}

	r = Azar();
	if (hueco > 20)
		return (r % 16 == 0) ? (hacia | BTN_ARRIBA) : hacia;
	switch (r % 8) {
	case 0:  return BTN_A;
	case 1:  return BTN_B;
	case 2:  return lejos;
	default: return 0;
	}
}


/* Dibujado                                                                  */

/*
 * La pantalla se arma por capas, de atrás hacia adelante:
 *
 *   escenario -> cuerpo de un peleador -> cuerpo del otro -> rectángulos
 *
 * Para repintar una zona, Repintar() arma cada fila en memoria: primero el
 * escenario y después, encima, los píxeles no transparentes de cada capa.
 * Luego manda la fila a la pantalla. Cada píxel se escribe una sola vez con
 * su color final, así que nada parpadea, y el puño de un peleador puede
 * pasar por encima del otro sin borrarlo.
 *
 * En cada tick, DibujarPelea() compara cómo debe verse cada peleador con lo
 * que ya hay pintado y solo repinta las zonas donde algo cambió.
 */

/* Recorta una caja a la pantalla. Si queda fuera, devuelve w o h <= 0. */
static Caja Recortar(Caja c) {
	if (c.x < 0) { c.w += c.x; c.x = 0; }
	if (c.y < 0) { c.h += c.y; c.y = 0; }
	if (c.x + c.w > ANCHO) c.w = ANCHO - c.x;
	if (c.y + c.h > ALTO)  c.h = ALTO - c.y;
	return c;
}

/* FillRect recortado a la pantalla (FillRect no revisa límites) */
static void Rellenar(Caja c, uint16_t color) {
	c = Recortar(c);
	if (c.w <= 0 || c.h <= 0)
		return;
	FillRect(c.x, c.y, c.w, c.h, color);
}

/* Pone en "fila" los píxeles de una capa. La fila es el renglón y de la
 * pantalla, desde la columna x0, con w píxeles. Los píxeles transparentes
 * del sprite no se tocan: ahí queda lo que ya había en la fila. */
static void PintarCapa(const Capa *k, int16_t x0, int16_t w, int16_t y) {
	const uint8_t *f;
	int16_t a, b, x;

	if (k->caja.w <= 0 || y < k->caja.y || y >= k->caja.y + k->caja.h)
		return;                          /* la capa no pasa por este renglón */

	/* Columnas de la pantalla donde la capa y la fila coinciden: [a, b) */
	a = (k->caja.x > x0) ? k->caja.x : x0;
	b = (k->caja.x + k->caja.w < x0 + w) ? k->caja.x + k->caja.w : x0 + w;

	if (k->px == NULL) {                 /* rectángulo de un solo color */
		for (x = a; x < b; x++)
			fila[x - x0] = k->color;
		return;
	}

	f = k->px + (y - k->caja.y) * k->caja.w;       /* renglón del cuadro */
	for (x = a; x < b; x++) {
		int16_t col = x - k->caja.x;               /* columna del cuadro */
		uint8_t indice;
		if (k->voltear)
			col = k->caja.w - 1 - col;             /* espejo: de atrás para adelante */
		indice = f[col];
		if (indice != 0)                           /* 0 = transparente */
			fila[x - x0] = k->silueta ? k->color : k->paleta[indice];
	}
}

/* Repinta varias zonas de la pantalla con todo lo que debe verse en ellas.
 * Se avanza fila por fila de la pantalla y en cada una se pintan los
 * pedazos de todas las zonas que pasan por ahí: así cada fila del
 * escenario se descomprime una sola vez aunque las zonas se encimen. */
static void RepintarZonas(const Caja *zs, uint8_t n) {
	const uint8_t abajo = (uint8_t) (1 - encima);
	Caja z[MAX_ZONAS];
	uint8_t m = 0, k;
	int16_t i, y, y0 = ALTO, y1 = 0;

	for (k = 0; k < n && m < MAX_ZONAS; k++) {
		Caja c = Recortar(zs[k]);
		if (c.w <= 0 || c.h <= 0)
			continue;
		z[m++] = c;
		if (c.y < y0) y0 = c.y;
		if (c.y + c.h > y1) y1 = c.y + c.h;
	}
	if (m == 0)
		return;

	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
	for (y = y0; y < y1; y++) {
		const uint8_t *f = Fondo_Fila(y);      /* NULL: fila sin escenario */
		const uint16_t vacio = Fondo_Vacio();
		for (k = 0; k < m; k++) {
			const Caja c = z[k];
			if (y < c.y || y >= c.y + c.h)
				continue;
			/* Capas, de atrás hacia adelante */
			if (f != NULL) {
				for (i = 0; i < c.w; i++)
					fila[i] = fondoPal[f[c.x + i]];
			} else {
				for (i = 0; i < c.w; i++)
					fila[i] = vacio;
			}
			PintarCapa(&capa[abajo][CUERPO],  c.x, c.w, y);
			PintarCapa(&capa[encima][CUERPO], c.x, c.w, y);
			PintarCapa(&capa[0][BRAZO], c.x, c.w, y);
			PintarCapa(&capa[1][BRAZO], c.x, c.w, y);

			/* La fila ya tiene los colores finales: a la pantalla */
			SetWindows(c.x, y, c.x + c.w - 1, y);
			for (i = 0; i < c.w; i++) {
				LCD_DATA((uint8_t) (fila[i] >> 8));
				LCD_DATA((uint8_t) fila[i]);
			}
		}
	}
	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

static void Repintar(Caja c) {
	RepintarZonas(&c, 1);
}

/* Caja más pequeña que contiene a las dos */
static Caja Union(Caja a, Caja b) {
	Caja u;
	int16_t x2 = (a.x + a.w > b.x + b.w) ? a.x + a.w : b.x + b.w;
	int16_t y2 = (a.y + a.h > b.y + b.h) ? a.y + a.h : b.y + b.h;
	u.x = (a.x < b.x) ? a.x : b.x;
	u.y = (a.y < b.y) ? a.y : b.y;
	u.w = x2 - u.x;
	u.h = y2 - u.y;
	return u;
}

/* Parte que las dos cajas tienen en común (w o h <= 0 si no se tocan) */
static Caja Cruce(Caja a, Caja b) {
	Caja c;
	int16_t x2 = (a.x + a.w < b.x + b.w) ? a.x + a.w : b.x + b.w;
	int16_t y2 = (a.y + a.h < b.y + b.h) ? a.y + a.h : b.y + b.h;
	c.x = (a.x > b.x) ? a.x : b.x;
	c.y = (a.y > b.y) ? a.y : b.y;
	c.w = x2 - c.x;
	c.h = y2 - c.y;
	return c;
}

static int32_t Area(Caja c) {
	return (int32_t) c.w * c.h;
}

/* Anota una zona para repintarla en este tick. Si repintarla junto con otra
 * ya anotada no cuesta más píxeles que hacerlo por separado, las junta: así
 * un peleador que avanza 4 px se repinta una vez y no dos (donde estaba y
 * donde está). */
static void Anotar(Caja c) {
	uint8_t i = 0;

	if (c.w <= 0 || c.h <= 0)
		return;
	c = Recortar(c);
	/* Los peleadores no llegan a la franja del marcador. Si un sprite más
	 * alto llegara, se corta ahí en vez de borrar las barras de vida. */
	if (c.y < MARCADOR_H) {
		c.h -= MARCADOR_H - c.y;
		c.y = MARCADOR_H;
	}
	if (c.w <= 0 || c.h <= 0)
		return;

	while (i < nZonas) {
		Caja u = Union(c, zonas[i]);
		if (Area(u) <= Area(c) + Area(zonas[i])) {
			c = u;                          /* se juntan...                */
			zonas[i] = zonas[--nZonas];     /* ...y la otra sale de la lista */
			i = 0;                          /* la caja creció: comparar de nuevo */
		} else {
			i++;
		}
	}
	zonas[nZonas++] = c;
}

/* Cuadro del ataque que toca en el tick t. El cuadro "activo" de la
 * animación (brazo o pierna extendidos) se ve durante los ticks en que el
 * ataque hace daño. Los cuadros anteriores a él se reparten antes, como
 * preparación. Después van los cuadros que le siguen; si la animación no
 * tiene, se repiten los de preparación al revés. */
static uint8_t CuadroAtaque(const Animacion *an, const Ataque *a, uint8_t t) {
	const uint8_t activo = an->activo;
	const uint8_t despues = (uint8_t) (an->n - 1 - activo);   /* cuadros tras el activo */
	uint8_t recoger, k;

	if (t >= a->ini && t <= a->fin)
		return activo;
	if (t < a->ini)                                    /* preparación */
		return (uint8_t) (t * activo / a->ini);

	recoger = (uint8_t) (a->duracion - 1 - a->fin);    /* ticks que quedan */
	k = (uint8_t) (t - a->fin - 1);                    /* 0, 1, 2...       */
	if (k >= recoger)
		return 0;
	if (despues > 0)
		return (uint8_t) (activo + 1 + k * despues / recoger);
	if (activo == 0)
		return 0;
	return (uint8_t) (activo - 1 - k * activo / recoger);
}

/* Cuadro de sprite que le toca al peleador en este tick */
static const Cuadro *CuadroActual(const Peleador *p) {
	const Personaje *pj = p->pj;

	if (Atacando(p)) {
		const uint8_t k = p->estado - P_GOLPE;         /* 0 golpe, 1 patada */
		const Animacion *an = &pj->ataque[k];
		if (an->n > 0)
			return &an->cuadros[CuadroAtaque(an, &ATAQUES[k], p->t)];
	} else if (p->estado == P_KO && pj->ko.n > 0) {
		/* Caída: un cuadro cada TICKS_KO ticks, y el último se queda */
		const uint8_t ultimo = (uint8_t) (pj->ko.n - 1);
		uint8_t k = (uint8_t) (p->t / TICKS_KO);
		if (k > ultimo)
			k = ultimo;
		/* El último cuadro es tendido en el piso. Si lo noquearon en el
		 * aire, se queda en el anterior hasta que termina de caer. */
		if (k == ultimo && ultimo > 0 && p->y < pisoY - pj->cuerpoH)
			k = (uint8_t) (ultimo - 1);
		return &pj->ko.cuadros[k];
	} else if (p->estado == P_CUBRE && pj->cubre.n > 0) {
		/* Cubriéndose: los cuadros se repiten mientras dure */
		return &pj->cubre.cuadros[(ticksEstado / TICKS_CUADRO) % pj->cubre.n];
	} else if (p->estado == P_DANO && pj->dano.n > 0) {
		/* Recibiendo un golpe: los cuadros se reparten en los ticks que dura */
		uint8_t k = (uint8_t) (p->t * pj->dano.n / TICKS_DANO);
		if (k >= pj->dano.n)
			k = (uint8_t) (pj->dano.n - 1);
		return &pj->dano.cuadros[k];
	} else if (p->estado == P_CAMINA) {
		uint8_t k = (uint8_t) ((ticksEstado / TICKS_CUADRO) % pj->caminar.n);
		/* Si camina alejándose del rival, la animación corre al revés */
		if (p->entrada & ((p->dir > 0) ? BTN_IZQ : BTN_DER))
			k = (uint8_t) (pj->caminar.n - 1 - k);
		return &pj->caminar.cuadros[k];
	}
	/* Quieto, saltando, o sin sprite para lo que está haciendo: la pose
	 * en guardia */
	return &pj->caminar.cuadros[0];
}

static const Capa CAPA_VACIA = { { 0, 0, 0, 0 }, NULL, NULL, NULL, 0, 0, 0 };

/* Capa del cuerpo: el cuadro de sprite que toca, colocado en la pantalla.
 * El cuadro se alinea con el cuerpo por los pies (su última fila va en el
 * piso) y por el centro (ver "eje" en sprites.h). */
static Capa CapaCuerpo(const Peleador *p) {
	Capa c = CAPA_VACIA;
	const Cuadro *q;
	int16_t centro, pies;

	if (p->estado == P_KO && p->pj->ko.n == 0) {
		c.caja = CajaCuerpo(p);         /* personaje sin sprite de KO: */
		c.color = C_GRIS;               /* un rectángulo gris en el piso */
		return c;
	}

	q = CuadroActual(p);

	/* El cuadro está comprimido en la RAM: se descomprime en el búfer del
	 * jugador, solo si ese búfer todavía no lo tiene. */
	{
		const uint8_t j = (uint8_t) (p - pl);
		if (cuadroEnBuf[j] != q) {
			if (!LZ_Descomprimir(q->datos, q->tam, bufCuadro[j], (uint32_t) q->w * q->h))
				memset(bufCuadro[j], 0, (uint32_t) q->w * q->h);   /* dañado: no se ve */
			cuadroEnBuf[j] = q;
		}
		c.px = bufCuadro[j];
	}
	c.cuadro = q;

	centro = p->x + p->pj->cuerpoW / 2;
	pies = p->y + p->pj->cuerpoH;

	c.caja.w = q->w;
	c.caja.h = q->h;
	c.caja.y = pies - q->h;
	/* Mirando a la derecha, "eje" columnas quedan a la izquierda del centro.
	 * Volteado quedan a la derecha, y a la izquierda las w - eje restantes. */
	c.caja.x = (p->dir > 0) ? centro - q->eje : centro - (q->w - q->eje);
	c.paleta = p->pj->paleta;
	c.voltear = (p->dir < 0);
	/* Al recibir un golpe, sin sprite de daño: la guardia toda en blanco */
	c.silueta = (p->estado == P_DANO && p->pj->dano.n == 0);
	c.color = C_BLANCO;

	if (p->estado == P_KO) {
		/* Los cuadros de KO son anchos y quedan detrás del cuerpo. Si el
		 * peleador cae junto al borde, se corren hacia adentro para que se
		 * vean completos. */
		if (c.caja.x < 0)
			c.caja.x = 0;
		if (c.caja.x + c.caja.w > ANCHO)
			c.caja.x = ANCHO - c.caja.w;
	}
	return c;
}

/* Capa del brazo: solo para personajes que todavía no tienen sprite de ese
 * ataque. Se dibuja la zona que golpea como un rectángulo amarillo. */
static Capa CapaBrazo(const Peleador *p) {
	Capa c = CAPA_VACIA;
	if (Atacando(p) && p->pj->ataque[p->estado - P_GOLPE].n == 0) {
		c.caja = CajaAtaque(p);         /* w = 0 fuera de los ticks activos */
		c.color = C_AMARILLO;
	}
	return c;
}

static uint8_t MismaCapa(const Capa *a, const Capa *b) {
	return Igual(a->caja, b->caja) && a->cuadro == b->cuadro && a->px == b->px
	    && a->paleta == b->paleta
	    && a->color == b->color && a->voltear == b->voltear
	    && a->silueta == b->silueta;
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
	Capa nueva[2][2];
	uint8_t i, k;
	uint8_t encimaNuevo = encima;

	/* 1. Cómo debe verse cada peleador en este tick */
	for (i = 0; i < 2; i++) {
		nueva[i][CUERPO] = CapaCuerpo(&pl[i]);
		nueva[i][BRAZO]  = CapaBrazo(&pl[i]);
	}
	/* El que ataca va encima, para que su puño o su pierna tapen al rival.
	 * Si atacan los dos o ninguno, el orden se queda como estaba. */
	if (Atacando(&pl[0]) != Atacando(&pl[1]))
		encimaNuevo = Atacando(&pl[1]) ? 1 : 0;

	/* 2. Zonas que cambiaron: donde estaba cada capa y donde está ahora */
	nZonas = 0;
	for (i = 0; i < 2; i++) {
		for (k = 0; k < 2; k++) {
			if (!MismaCapa(&capa[i][k], &nueva[i][k])) {
				Anotar(capa[i][k].caja);
				Anotar(nueva[i][k].caja);
				capa[i][k] = nueva[i][k];
			}
		}
	}
	if (encimaNuevo != encima) {
		/* Cambió quién va encima: se nota solo donde los dos se cruzan */
		Anotar(Cruce(capa[0][CUERPO].caja, capa[1][CUERPO].caja));
		encima = encimaNuevo;
	}

	/* 3. Repintar esas zonas con las capas nuevas */
	RepintarZonas(zonas, nZonas);

	/* 4. Marcador */
	for (i = 0; i < 2; i++) {
		if (pl[i].vida != pl[i].vidaDib)
			DibujarVida(i);
	}
	if (tiempo != tiempoDib)
		DibujarTiempo();
}


/* ------------------------------------------------------------------------ */
/* Textos                                                                    */
/* ------------------------------------------------------------------------ */
/* Copia el texto s al final de d (como máximo n letras) y devuelve dónde
 * quedó el final, para seguir pegando. */
static char *Pegar(char *d, const char *s, uint8_t n) {
	while (*s && n--)
		*d++ = *s++;
	*d = 0;
	return d;
}

/* Pega un número sin signo en decimal */
static char *PegarNum(char *d, uint32_t v) {
	char t[10];
	uint8_t n = 0;
	do {
		t[n++] = (char) ('0' + v % 10);
		v /= 10;
	} while (v);
	while (n)
		*d++ = t[--n];
	*d = 0;
	return d;
}

/* Texto centrado en la franja [x0, x0 + w) de la pantalla. tam: 1 = 8 px
 * por letra, 2 = 16 px por letra. */
static void TextoEn(const char *t, int16_t x0, int16_t w, int16_t y, uint8_t tam,
		uint16_t color, uint16_t fondo) {
	int16_t ancho = (int16_t) (strlen(t) * (tam == 1 ? 8 : 16));
	int16_t x = (int16_t) (x0 + (w - ancho) / 2);
	if (x < 0)
		x = 0;
	LCD_Print((char *) t, x, y, tam, color, fondo);
}

static void Centrado(const char *t, int16_t y, uint8_t tam, uint16_t color, uint16_t fondo) {
	TextoEn(t, 0, ANCHO, y, tam, color, fondo);
}

/* ------------------------------------------------------------------------ */
/* Archivos de la SD                                                         */
/* ------------------------------------------------------------------------ */

/* "RYU.SPR" -> "RYU": nombre para mostrar si el archivo no trae uno */
static void SinExtension(char *d, const char *archivo) {
	uint8_t i = 0;
	while (archivo[i] && archivo[i] != '.' && i < 15) {
		d[i] = archivo[i];
		i++;
	}
	d[i] = 0;
}

/* Busca en la SD los personajes y escenarios y lee sus nombres */
static void BuscarArchivos(void) {
	nPj = SD_Listar(".SPR", archPj, MAX_PJ);
	nOp = 0;
	for (uint8_t i = 0; i < nPj; i++) {
		/* Una opción por cada versión de color: RYU, RYU AZUL... */
		char nombres[4][16];
		uint8_t np = 0;
		if (Personaje_Nombres(archPj[i], nombres, 4, &np) != REC_OK || np == 0) {
			np = 1;
			SinExtension(nombres[0], archPj[i]);
		}
		nPalArch[i] = np;
		for (uint8_t k = 0; k < np && nOp < MAX_OP; k++) {
			if (nombres[k][0] == 0)
				SinExtension(nombres[k], archPj[i]);
			memcpy(nomOp[nOp], nombres[k], 16);
			opArch[nOp] = i;
			opPal[nOp] = k;
			nOp++;
		}
	}
	nEsc = SD_Listar(".ESC", archEsc, MAX_ESC);
	for (uint8_t i = 0; i < nEsc; i++) {
		if (Fondo_Nombre(archEsc[i], nomEsc[i]) != REC_OK || nomEsc[i][0] == 0)
			SinExtension(nomEsc[i], archEsc[i]);
	}
	/* La lista pudo cambiar: lo que había en la RAM ya no se reconoce */
	cargado[0] = cargado[1] = cargado[2] = 0xFF;
	eleccion[0] = 0;
	eleccion[1] = (nOp > 1) ? 1 : 0;
	escElegido = 0;
}

/* Archivo del personaje que eligió el jugador j */
static uint8_t ArchivoDe(uint8_t j) {
	return opArch[eleccion[j]];
}

/* Paleta del jugador j: la de la opción elegida. Si los dos eligieron
 * exactamente la misma (p. ej. RYU y RYU), el jugador 2 usa la siguiente
 * versión de color del archivo, si la hay, para que no sean idénticos. */
static uint8_t PaletaDe(uint8_t j) {
	uint8_t pal = opPal[eleccion[j]];
	const uint8_t np = nPalArch[ArchivoDe(j)];
	if (j == 1 && eleccion[0] == eleccion[1] && np > 1)
		pal = (uint8_t) ((pal + 1) % np);
	return pal;
}

/* Nombre de lo que va a usar el jugador j (con el cambio de color de arriba) */
static const char *NombreDe(uint8_t j) {
	for (uint8_t i = 0; i < nOp; i++) {
		if (opArch[i] == ArchivoDe(j) && opPal[i] == PaletaDe(j))
			return nomOp[i];
	}
	return nomOp[eleccion[j]];
}

/* Copia a la RAM lo elegido: escenario, personaje de J1 y de J2, y los
 * búferes donde se descomprimen los cuadros. Si es lo mismo que ya está
 * cargado no lee nada. Si algo falla, deja el texto del error en avisoTxt. */
static uint8_t CargarPelea(void) {
	const uint8_t esc = (nEsc > 0) ? escElegido : 0xFE;   /* 0xFE = fondo liso */
	const char *falla = "";
	uint8_t r = REC_OK;
	char *t;

	const uint8_t a0 = ArchivoDe(0), a1 = ArchivoDe(1);

	if (cargado[0] == a0 && cargado[1] == a1 && cargado[2] == esc) {
		/* Ya está en la RAM: solo poner los colores elegidos */
		Personaje_Paleta(&pjCargado[0], PaletaDe(0));
		Personaje_Paleta(&pjCargado[1], PaletaDe(1));
		return REC_OK;
	}

	LCD_Clear(C_NEGRO);
	Centrado("CARGANDO...", 100, 2, C_BLANCO, C_NEGRO);

	Ram_Liberar();
	Fondo_Quitar();
	cargado[0] = cargado[1] = cargado[2] = 0xFF;

	/* 1. Escenario */
	if (esc != 0xFE) {
		r = Fondo_Cargar(archEsc[esc]);
		falla = archEsc[esc];
	}
	/* 2. Personaje del jugador 1 */
	if (r == REC_OK) {
		r = Personaje_Cargar(archPj[a0], &pjCargado[0], PaletaDe(0));
		falla = archPj[a0];
	}
	/* 3. Personaje del jugador 2: si es el mismo, comparte los cuadros (no
	 *    se carga dos veces) y solo cambia la paleta */
	if (r == REC_OK) {
		if (a1 == a0) {
			pjCargado[1] = pjCargado[0];
			Personaje_Paleta(&pjCargado[1], PaletaDe(1));
		} else {
			r = Personaje_Cargar(archPj[a1], &pjCargado[1], PaletaDe(1));
			falla = archPj[a1];
		}
	}
	/* 4. Un búfer por jugador, del tamaño del cuadro más grande */
	for (uint8_t i = 0; i < 2 && r == REC_OK; i++) {
		bufCuadro[i] = Ram_Pedir(pjCargado[i].maxCuadro);
		cuadroEnBuf[i] = NULL;
		if (bufCuadro[i] == NULL) {
			r = REC_ERR_RAM;
			falla = archPj[ArchivoDe(i)];
		}
	}

	if (r != REC_OK) {
		Ram_Liberar();
		Fondo_Quitar();
		Pegar(avisoTxt[0], "NO SE PUDO CARGAR", 40);
		Pegar(avisoTxt[1], falla, 40);
		switch (r) {
		case REC_ERR_SD:      t = Pegar(avisoTxt[2], "LA SD NO RESPONDE", 40); break;
		case REC_ERR_ABRIR:   t = Pegar(avisoTxt[2], "NO SE ABRIO EL ARCHIVO", 40); break;
		case REC_ERR_DATOS:   t = Pegar(avisoTxt[2], SD_CodigoFatFs() ? "ERROR AL LEER LA SD"
		                                                          : "ARCHIVO INCOMPLETO", 40); break;
		case REC_ERR_FORMATO: t = Pegar(avisoTxt[2], "EL ARCHIVO NO SIRVE", 40); break;
		default:              t = Pegar(avisoTxt[2], "NO CABE EN LA RAM", 40); break;
		}
		if (r == REC_ERR_RAM) {
			t = Pegar(avisoTxt[3], "HAY ", 40);
			t = PegarNum(t, RAM_ALMACEN / 1024);
			Pegar(t, " KB: ELIGE OTRA COMBINACION", 40);
		} else {
			t = Pegar(avisoTxt[3], "CODIGO FATFS ", 40);
			PegarNum(t, SD_CodigoFatFs());
		}
		return r;
	}

	cargado[0] = a0;
	cargado[1] = a1;
	cargado[2] = esc;

	/* Cuánta RAM quedó ocupada, para saber qué tan cerca está el límite */
	t = Pegar(avisoTxt[0], "RAM: ", 40);
	t = PegarNum(t, (Ram_Usada() + 1023) / 1024);
	t = Pegar(t, " DE ", 40);
	t = PegarNum(t, RAM_ALMACEN / 1024);
	Pegar(t, " KB", 40);
	Centrado(avisoTxt[0], 130, 1, C_GRIS, C_NEGRO);

	/* Lo que sobró del almacén: bloques del escenario ya descomprimidos */
	Fondo_UsarRamLibre();
	HAL_Delay(600);
	return REC_OK;
}

/* ------------------------------------------------------------------------ */
/* Botones en los menús                                                      */
/* ------------------------------------------------------------------------ */

/* Botones que el jugador j acaba de presionar (flanco de subida) */
static uint8_t Nuevos(uint8_t j) {
	uint8_t in = LeerEntrada(j);
	uint8_t nuevo = in & (uint8_t) ~prevJ[j];
	prevJ[j] = in;
	return nuevo;
}

/* Devuelve 1 si algún jugador acaba de presionar A */
static uint8_t PresionaronB(void) {
	uint8_t in = LeerEntrada(0) | LeerEntrada(1);
	uint8_t nuevo = in & (uint8_t) ~prevMenu;
	prevMenu = in;
	return (nuevo & BTN_A) != 0;
}

/* ------------------------------------------------------------------------ */
/* Estados del juego                                                         */
/* ------------------------------------------------------------------------ */

static void EntrarElegirPj(void);
static void EntrarElegirEsc(void);

/* Una línea al pie del título: qué hay en la SD */
static void DibujarEstadoSD(void) {
	char txt[41];
	char *t;
	FillRect(0, 196, ANCHO, 28, C_NEGRO);
	if (nPj == 0) {
		t = Pegar(txt, "NO HAY PERSONAJES EN LA SD (FATFS ", 40);
		t = PegarNum(t, SD_CodigoFatFs());
		Pegar(t, ")", 40);
		Centrado(txt, 200, 1, C_J1, C_NEGRO);
		Centrado("COPIA LOS .SPR Y .ESC A LA SD", 214, 1, C_GRIS, C_NEGRO);
		return;
	}
	t = Pegar(txt, "SD: ", 40);
	t = PegarNum(t, nOp);
	t = Pegar(t, nOp == 1 ? " PERSONAJE, " : " PERSONAJES, ", 40);
	t = PegarNum(t, nEsc);
	Pegar(t, nEsc == 1 ? " ESCENARIO" : " ESCENARIOS", 40);
	Centrado(txt, 204, 1, C_GRIS, C_NEGRO);
}

static void EntrarTitulo(void) {
	estadoJuego = EST_TITULO;
	ticksEstado = 0;
	prevMenu = 0xFF;      /* evita que un botón ya presionado cuente */
	LCD_Clear(C_NEGRO);
	LCD_Print("STREET FIGHTER", 48, 70, 2, C_AMARILLO, C_NEGRO);
	LCD_Print("J1 vs J2", 96, 100, 2, C_BLANCO, C_NEGRO);
	DibujarEstadoSD();
	Game_Sonido(SND_MENU);
}

static void TickTitulo(void) {
	/* Texto parpadeante */
	if (ticksEstado % 30 == 1)
		LCD_Print("PRESIONA A", 80, 160, 2, C_BLANCO, C_NEGRO);
	else if (ticksEstado % 30 == 16)
		LCD_Print("          ", 80, 160, 2, C_BLANCO, C_NEGRO);

	if (PresionaronB()) {
		if (nPj == 0) {                 /* quizá ya metieron la tarjeta */
			BuscarArchivos();
			DibujarEstadoSD();
		}
		if (nPj > 0)
			EntrarElegirPj();
	}
}

/* ---- Elegir personajes -------------------------------------------------- */

#define PANEL_W   144
#define PANEL_Y   32
#define PIES_Y    150     /* fila donde van los pies del retrato */

static int16_t PanelX(uint8_t j) {
	return (j == 0) ? 8 : ANCHO - 8 - PANEL_W;
}

static void DibujarListo(uint8_t j) {
	const int16_t x0 = PanelX(j);
	FillRect(x0, 184, PANEL_W, 18, C_NEGRO);
	if (listo[j])
		TextoEn("LISTO", x0, PANEL_W, 184, 2, C_LISTO, C_NEGRO);
	else
		TextoEn("A: ELEGIR", x0, PANEL_W, 188, 1, C_GRIS, C_NEGRO);
}

/* Recuadro de un jugador: retrato (pose en guardia), flechas y nombre */
static void DibujarPanel(uint8_t j) {
	const int16_t x0 = PanelX(j);
	const char *nombre = NombreDe(j);

	FillRect(x0, PANEL_Y, PANEL_W, 184 - PANEL_Y, C_NEGRO);
	TextoEn(j ? "J2" : "J1", x0, PANEL_W, PANEL_Y, 2, j ? C_J2 : C_J1, C_NEGRO);
	/* El retrato se lee directo de la SD: no toca lo que hay en la RAM */
	if (Personaje_Vista(archPj[ArchivoDe(j)], PaletaDe(j), x0 + PANEL_W / 2,
			PIES_Y, j == 1, C_NEGRO) != REC_OK)
		TextoEn("SIN IMAGEN", x0, PANEL_W, 96, 1, C_J1, C_NEGRO);
	if (nOp > 1) {
		LCD_Print("<", x0, 92, 2, C_BLANCO, C_NEGRO);
		LCD_Print(">", x0 + PANEL_W - 16, 92, 2, C_BLANCO, C_NEGRO);
	}
	if (strlen(nombre) * 16 <= PANEL_W)
		TextoEn(nombre, x0, PANEL_W, 160, 2, C_BLANCO, C_NEGRO);
	else
		TextoEn(nombre, x0, PANEL_W, 164, 1, C_BLANCO, C_NEGRO);
	DibujarListo(j);
}

static void EntrarElegirPj(void) {
	estadoJuego = EST_ELEGIR_PJ;
	ticksEstado = 0;
	listo[0] = listo[1] = 0;
	ticksListos = 0;
	prevJ[0] = prevJ[1] = 0xFF;
	for (uint8_t j = 0; j < 2; j++) {
		if (eleccion[j] >= nOp)
			eleccion[j] = 0;
	}
	LCD_Clear(C_NEGRO);
	Centrado("ELIGE TU PELEADOR", 6, 2, C_AMARILLO, C_NEGRO);
	Centrado("< >: CAMBIAR   A: ELEGIR   B: REGRESAR", 222, 1, C_GRIS, C_NEGRO);
	DibujarPanel(0);
	DibujarPanel(1);
}

static void TickElegirPj(void) {
	for (uint8_t j = 0; j < 2; j++) {
		uint8_t n = Nuevos(j);
		if (n == 0)
			continue;
		if (listo[j]) {
			if (n & BTN_B) {                /* se arrepintió */
				listo[j] = 0;
				DibujarListo(j);
			}
			continue;
		}
		if (n & BTN_B) {
			EntrarTitulo();
			return;
		}
		if (n & BTN_A) {
			listo[j] = 1;
			DibujarListo(j);
			continue;
		}
		if ((n & (BTN_IZQ | BTN_DER)) && nOp > 1) {
			const uint8_t paletaJ2Antes = PaletaDe(1);
			if (n & BTN_IZQ)
				eleccion[j] = (uint8_t) ((eleccion[j] + nOp - 1) % nOp);
			else
				eleccion[j] = (uint8_t) ((eleccion[j] + 1) % nOp);
			DibujarPanel(j);
			/* Si J1 llegó o salió de la misma opción que J2, cambia el color de J2 */
			if (j == 0 && PaletaDe(1) != paletaJ2Antes)
				DibujarPanel(1);
		}
	}
	/* Los dos listos: una pausa corta para que se vea y a elegir escenario */
	if (listo[0] && listo[1]) {
		if (++ticksListos > TICKS_SEGUNDO / 2)
			EntrarElegirEsc();
	} else {
		ticksListos = 0;
	}
}

/* ---- Elegir escenario --------------------------------------------------- */

static void DibujarEscenario(void) {
	char txt[24];
	char *t;
	if (nEsc == 0) {
		LCD_Clear(FONDO_SIN_SD);
		Centrado("NO HAY ESCENARIOS EN LA SD", 110, 1, C_NEGRO, FONDO_SIN_SD);
	} else if (Fondo_Vista(archEsc[escElegido]) != REC_OK) {
		LCD_Clear(C_NEGRO);
		Centrado("NO SE PUDO LEER", 100, 2, C_J1, C_NEGRO);
		Centrado(archEsc[escElegido], 124, 1, C_BLANCO, C_NEGRO);
	}
	/* Vista previa del escenario, con el nombre encima */
	Centrado("ELIGE ESCENARIO", 4, 2, C_AMARILLO, C_NEGRO);
	FillRect(0, 202, ANCHO, ALTO - 202, C_NEGRO);
	t = txt;
	if (nEsc > 1)
		t = Pegar(t, "< ", 4);
	t = Pegar(t, nEsc ? nomEsc[escElegido] : "FONDO LISO", 15);
	if (nEsc > 1)
		Pegar(t, " >", 4);
	Centrado(txt, 204, (strlen(txt) * 16 <= ANCHO) ? 2 : 1, C_BLANCO, C_NEGRO);
	Centrado("A: PELEAR   B: REGRESAR", 226, 1, C_GRIS, C_NEGRO);
}

static void EntrarElegirEsc(void) {
	estadoJuego = EST_ELEGIR_ESC;
	ticksEstado = 0;
	prevJ[0] = prevJ[1] = 0xFF;
	if (escElegido >= nEsc)
		escElegido = 0;
	DibujarEscenario();
}

/* ---- Aviso de error al cargar --------------------------------------------- */

static void EntrarAviso(void) {
	estadoJuego = EST_AVISO;
	ticksEstado = 0;
	prevJ[0] = prevJ[1] = 0xFF;
	LCD_Clear(C_NEGRO);
	Centrado(avisoTxt[0], 60, 2, C_J1, C_NEGRO);
	Centrado(avisoTxt[1], 96, 1, C_BLANCO, C_NEGRO);
	Centrado(avisoTxt[2], 116, 1, C_BLANCO, C_NEGRO);
	Centrado(avisoTxt[3], 132, 1, C_GRIS, C_NEGRO);
	Centrado("A: REGRESAR", 180, 2, C_BLANCO, C_NEGRO);
}

static void TickAviso(void) {
	if ((Nuevos(0) | Nuevos(1)) & (BTN_A | BTN_B))
		EntrarElegirPj();
}

/* ---- Pelea ---------------------------------------------------------------- */

static void EntrarPelea(void) {
	estadoJuego = EST_PELEA;
	ticksEstado = 0;
	finRonda = 0;
	ticksFin = 0;
	tiempo = TIEMPO_RONDA;
	demo = !entradaReal;
	semilla += HAL_GetTick();

	/* Línea del piso: la del escenario si la trae y es razonable */
	pisoY = (Fondo_Listo() && fondoPiso >= 180 && fondoPiso <= ALTO)
	        ? (int16_t) fondoPiso : PISO_NORMAL;

	for (uint8_t i = 0; i < 2; i++) {
		Peleador *p = &pl[i];
		p->pj = &pjCargado[i];
		p->x = (i == 0) ? p->pj->margen + X_INICIAL
		                : ANCHO - p->pj->margen - X_INICIAL - p->pj->cuerpoW;
		p->y = pisoY - p->pj->cuerpoH;
		p->vy = 0;
		p->dir = (i == 0) ? 1 : -1;
		p->vida = VIDA_MAX;
		p->entrada = 0;
		p->entradaPrev = 0xFF;
		Cambiar(p, P_QUIETO);
		/* Todavía no hay nada suyo pintado */
		capa[i][CUERPO] = CAPA_VACIA;
		capa[i][BRAZO]  = CAPA_VACIA;
		cuadroEnBuf[i] = NULL;
	}
	encima = 0;

	/* Escenario completo: como no hay capas, Repintar solo pinta el fondo.
	 * Los peleadores aparecen en DibujarPelea, al ver que no están pintados. */
	Repintar((Caja) { 0, 0, ANCHO, ALTO });

	DibujarVida(0);
	DibujarVida(1);
	DibujarTiempo();
	DibujarPelea();
	Game_Sonido(SND_PELEA);
}

static void TickElegirEsc(void) {
	uint8_t n = Nuevos(0) | Nuevos(1);
	if (n & BTN_B) {
		EntrarElegirPj();
	} else if (n & BTN_A) {
		if (CargarPelea() == REC_OK)
			EntrarPelea();
		else
			EntrarAviso();
	} else if ((n & (BTN_IZQ | BTN_DER)) && nEsc > 1) {
		if (n & BTN_IZQ)
			escElegido = (uint8_t) ((escElegido + nEsc - 1) % nEsc);
		else
			escElegido = (uint8_t) ((escElegido + 1) % nEsc);
		DibujarEscenario();
		prevJ[0] = prevJ[1] = 0xFF;     /* lo presionado mientras dibujaba no cuenta */
	}
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

static void TickPelea(void) {
	uint8_t i;

	/* Si alguien conecta un mando durante la demostración, volver al menú */
	if (demo && entradaReal) {
		EntrarTitulo();
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
	/* Revancha: de vuelta a elegir, con la elección anterior ya puesta. Si
	 * no cambian nada, la siguiente pelea no vuelve a leer la SD. */
	if (PresionaronB() || (demo && ticksEstado > 3 * TICKS_SEGUNDO))
		EntrarElegirPj();
}

/* ------------------------------------------------------------------------ */
/* Funciones públicas                                                        */
/* ------------------------------------------------------------------------ */

void Game_Init(void) {
	BuscarArchivos();
	ultimoTick = HAL_GetTick();
	EntrarTitulo();
}

void Game_Update(void) {
	uint32_t ahora = HAL_GetTick();
	if (ahora - ultimoTick < TICK_MS)
		return;             /* todavía no toca*/
	ultimoTick = ahora;
	ticksEstado++;

	switch (estadoJuego) {
	case EST_TITULO:     TickTitulo();    break;
	case EST_ELEGIR_PJ:  TickElegirPj();  break;
	case EST_ELEGIR_ESC: TickElegirEsc(); break;
	case EST_AVISO:      TickAviso();     break;
	case EST_PELEA:      TickPelea();     break;
	case EST_GANADOR:    TickGanador();   break;
	}
}

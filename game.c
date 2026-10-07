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
 * El escenario está en la microSD (fondo.c / fondo.h). Se copia a la RAM
 * antes de cada pelea y de ahí se lee al repintar. Sin tarjeta el juego
 * funciona con fondo liso.
 */

#include <stddef.h>
#include "game.h"
#include "ili9341.h"   /* también incluye main.h (HAL) */
#include "sprites.h"
#include "fondo.h"     /* escenario: se carga de la microSD a la RAM */

/* Escenarios: archivos en la raíz de la microSD. Se usan por turnos, uno
 * distinto en cada pelea; si alguno no está en la tarjeta se salta. Para
 * agregar otro basta con copiar su archivo a la SD y ponerlo en la lista. */
static const char *const ESCENARIOS[] = { "Fondo.bin", "Honda.bin" };
#define N_ESCENARIOS   (sizeof ESCENARIOS / sizeof ESCENARIOS[0])

/* ------------------------------------------------------------------------ */
/* Configuración                                                             */
/* ------------------------------------------------------------------------ */
#define ANCHO          320
#define ALTO           240
#define TICK_MS        33        /* ~30 cuadros por segundo                  */
#define TICKS_SEGUNDO  30

#define PISO_NORMAL    200       /* y de los pies si el escenario no la trae */
#define X_INICIAL      20        /* al empezar, cuánto se separa cada peleador
                                    de su tope en el borde de la pantalla    */
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
	const uint8_t  *px;       /* píxeles del cuadro; NULL = rectángulo liso  */
	const uint16_t *paleta;
	uint16_t color;           /* color del rectángulo, o de la silueta       */
	uint8_t  voltear;         /* 1 = espejo horizontal (mira a la izquierda) */
	uint8_t  silueta;         /* 1 = todo el sprite de un solo color         */
} Capa;

enum {
	CUERPO,     /* el sprite del peleador (o un rectángulo gris si está en KO) */
	BRAZO       /* rectángulo del ataque, para personajes sin sprite de ataque */
};

/* Personaje de cada jugador. Para cambiarlo basta con cambiar esta tabla
 * (personajes disponibles: ryu, ryuAzul, deeJay). */
static const Personaje *const PERSONAJE[2] = { &ryu, &deeJay };

/* ------------------------------------------------------------------------ */
/* Variables                                                                 */
/* ------------------------------------------------------------------------ */
static EstadoJuego estadoJuego;
static Peleador    pl[2];
/* Último resultado de cargar cada escenario de la lista (FONDO_OK = bien)
 * y el código de FatFs que lo acompañó. Se muestran en el menú. */
static uint8_t     errorEsc[N_ESCENARIOS];
static uint8_t     fatfsEsc[N_ESCENARIOS];
static uint8_t     escenario;        /* índice del escenario que toca cargar */
static uint8_t     escenarioRam = 0xFF;  /* índice del que está en la RAM    */
static uint8_t     sinTarjeta;       /* la SD no respondió: no reintentar    */
static int16_t     pisoY = PISO_NORMAL;  /* y donde se paran los peleadores  */
static uint32_t    ultimoTick;
static uint16_t    ticksEstado;      /* ticks desde que se entró al estado   */
static uint8_t     tiempo, tiempoDib;
static uint8_t     finRonda;         /* 1 = ya hay ganador, esperando        */
static uint16_t    ticksFin;
static uint8_t     ganador;          /* 1, 2 o 0 = empate                    */
static uint8_t     prevMenu;         /* botones del tick anterior en menús   */
static uint8_t     demo;             /* 1 = pelea automática de demostración */
static uint32_t    semilla = 12345;

static void CargarEscenario(void);

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

/* Repinta una zona de la pantalla con todo lo que debe verse en ella. */
static void Repintar(Caja c) {
	const uint8_t abajo = (uint8_t) (1 - encima);
	int16_t i, y;

	c = Recortar(c);
	if (c.w <= 0 || c.h <= 0)
		return;

	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
	SetWindows(c.x, c.y, c.x + c.w - 1, c.y + c.h - 1);
	for (y = c.y; y < c.y + c.h; y++) {
		/* Capas, de atrás hacia adelante */
		for (i = 0; i < c.w; i++)
			fila[i] = Fondo_Color(c.x + i, y);
		PintarCapa(&capa[abajo][CUERPO],  c.x, c.w, y);
		PintarCapa(&capa[encima][CUERPO], c.x, c.w, y);
		PintarCapa(&capa[0][BRAZO], c.x, c.w, y);
		PintarCapa(&capa[1][BRAZO], c.x, c.w, y);

		/* La fila ya tiene los colores finales: a la pantalla */
		for (i = 0; i < c.w; i++) {
			LCD_DATA((uint8_t) (fila[i] >> 8));
			LCD_DATA((uint8_t) fila[i]);
		}
	}
	HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
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

/* Cuadro del ataque que toca en el tick t. El último cuadro de la animación
 * (brazo o pierna extendidos) se ve durante los ticks activos; los demás se
 * reparten antes, como preparación, y después al revés, al recoger. */
static uint8_t CuadroAtaque(const Animacion *an, const Ataque *a, uint8_t t) {
	const uint8_t ultimo = (uint8_t) (an->n - 1);
	uint8_t recoger, k;

	if (ultimo == 0 || (t >= a->ini && t <= a->fin))
		return ultimo;
	if (t < a->ini)
		return (uint8_t) (t * ultimo / a->ini);

	recoger = (uint8_t) (a->duracion - 1 - a->fin);    /* ticks que quedan */
	k = (uint8_t) (t - a->fin - 1);                    /* 0, 1, 2...       */
	if (k >= recoger)
		return 0;
	return (uint8_t) (ultimo - 1 - k * ultimo / recoger);
}

/* Cuadro de sprite que le toca al peleador en este tick */
static const Cuadro *CuadroActual(const Peleador *p) {
	const Personaje *pj = p->pj;

	if (Atacando(p)) {
		const uint8_t k = p->estado - P_GOLPE;         /* 0 golpe, 1 patada */
		const Animacion *an = &pj->ataque[k];
		if (an->n > 0)
			return &an->cuadros[CuadroAtaque(an, &ATAQUES[k], p->t)];
	} else if (p->estado == P_CAMINA) {
		uint8_t k = (uint8_t) ((ticksEstado / TICKS_CUADRO) % pj->caminar.n);
		/* Si camina alejándose del rival, la animación corre al revés */
		if (p->entrada & ((p->dir > 0) ? BTN_IZQ : BTN_DER))
			k = (uint8_t) (pj->caminar.n - 1 - k);
		return &pj->caminar.cuadros[k];
	}
	/* Quieto, saltando, recibiendo un golpe, o atacando sin sprite de
	 * ataque: la pose en guardia */
	return &pj->caminar.cuadros[0];
}

static const Capa CAPA_VACIA = { { 0, 0, 0, 0 }, NULL, NULL, 0, 0, 0 };

/* Capa del cuerpo: el cuadro de sprite que toca, colocado en la pantalla.
 * El cuadro se alinea con el cuerpo por los pies (su última fila va en el
 * piso) y por el centro (ver "eje" en sprites.h). */
static Capa CapaCuerpo(const Peleador *p) {
	Capa c = CAPA_VACIA;
	const Cuadro *q;
	int16_t centro, pies;

	if (p->estado == P_KO) {            /* todavía no hay sprite de KO */
		c.caja = CajaCuerpo(p);
		c.color = C_GRIS;
		return c;
	}

	q = CuadroActual(p);
	centro = p->x + p->pj->cuerpoW / 2;
	pies = p->y + p->pj->cuerpoH;

	c.caja.w = q->w;
	c.caja.h = q->h;
	c.caja.y = pies - q->h;
	/* Mirando a la derecha, "eje" columnas quedan a la izquierda del centro.
	 * Volteado quedan a la derecha, y a la izquierda las w - eje restantes. */
	c.caja.x = (p->dir > 0) ? centro - q->eje : centro - (q->w - q->eje);
	c.px = q->px;
	c.paleta = p->pj->paleta;
	c.voltear = (p->dir < 0);
	c.silueta = (p->estado == P_DANO);  /* al recibir un golpe: todo blanco */
	c.color = C_BLANCO;
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
	return Igual(a->caja, b->caja) && a->px == b->px && a->paleta == b->paleta
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
	for (i = 0; i < nZonas; i++)
		Repintar(zonas[i]);

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
/* Copia el texto s al final de d (como máximo n letras) y devuelve dónde
 * quedó el final, para seguir pegando. */
static char *Pegar(char *d, const char *s, uint8_t n) {
	while (*s && n--)
		*d++ = *s++;
	*d = 0;
	return d;
}

/* Dos renglones al pie del menú: qué escenario está cargado para la próxima
 * pelea y, si algún archivo de la lista no se pudo cargar, cuál y por qué.
 * El primer número es el error de fondo.h y el segundo el código de FatFs
 * (ERROR 2, FATFS 04 = el archivo no está en la tarjeta). */
static void DibujarEstadoEscenarios(void) {
	char txt[41];
	char *t;

	t = Pegar(txt, "ESCENARIO: ", 40);
	Pegar(t, Fondo_Listo() ? ESCENARIOS[escenarioRam] : "NINGUNO", 12);
	LCD_Print(txt, 8, 204, 1, C_GRIS, C_NEGRO);

	for (uint8_t i = 0; i < N_ESCENARIOS; i++) {
		if (errorEsc[i] == FONDO_OK)
			continue;
		t = Pegar(txt, "FALLO ", 40);
		t = Pegar(t, ESCENARIOS[i], 12);
		t = Pegar(t, ": ERROR 0, FATFS 00", 40);
		t[-11] = (char) ('0' + errorEsc[i]);
		t[-2]  = (char) ('0' + fatfsEsc[i] / 10 % 10);
		t[-1]  = (char) ('0' + fatfsEsc[i] % 10);
		LCD_Print(txt, 8, 220, 1, C_J1, C_NEGRO);
		break;                          /* solo cabe uno: el primero */
	}
}

static void EntrarMenu(void) {
	estadoJuego = EST_MENU;
	ticksEstado = 0;
	prevMenu = 0xFF;      /* evita que un botón ya presionado cuente */
	LCD_Clear(C_NEGRO);
	LCD_Print("STREET FIGHTER", 48, 70, 2, C_AMARILLO, C_NEGRO);
	LCD_Print("J1 vs J2", 96, 100, 2, C_BLANCO, C_NEGRO);
	DibujarEstadoEscenarios();
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
	if (PresionaronA() || (demo && ticksEstado > 3 * TICKS_SEGUNDO)) {
		CargarEscenario();    /* el de la próxima pelea */
		EntrarMenu();
	}
}

/* ------------------------------------------------------------------------ */
/* Funciones públicas                                                        */
/* ------------------------------------------------------------------------ */
/* Deja en la RAM el escenario que toca y pasa el turno al siguiente. Leer
 * la tarjeta tarda, por eso se hace entre peleas y no durante una. */
static void CargarEscenario(void) {
	if (sinTarjeta)
		return;
	for (uint8_t k = 0; k < N_ESCENARIOS; k++) {
		uint8_t i = (uint8_t) ((escenario + k) % N_ESCENARIOS);
		uint8_t error = FONDO_OK;
		if (i != escenarioRam || !Fondo_Listo()) {  /* si no, ya está cargado */
			error = Fondo_Cargar(ESCENARIOS[i]);
			fatfsEsc[i] = Fondo_CodigoFatFs();
		}
		errorEsc[i] = error;

		if (error == FONDO_OK) {
			escenarioRam = i;
			escenario = (uint8_t) ((i + 1) % N_ESCENARIOS);
			/* Línea del piso: la del archivo si la trae y es razonable */
			pisoY = (fondoPiso >= 180 && fondoPiso <= ALTO) ? (int16_t) fondoPiso
			                                                : PISO_NORMAL;
			return;
		}
		if (error == FONDO_ERR_SD) {                /* no hay tarjeta */
			sinTarjeta = 1;
			break;
		}
	}
	pisoY = PISO_NORMAL;                            /* fondo liso */
}

void Game_Init(void) {
	/* Si main.c acaba de guardar "Fondo.bin" ya quedó en la RAM y no se
	 * vuelve a leer. */
	if (Fondo_Listo())
		escenarioRam = 0;
	CargarEscenario();
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

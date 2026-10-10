#!/usr/bin/env python3
"""
convertir.py - Genera los archivos de la microSD del juego (Proyecto 2, IE3054).

Los personajes y los escenarios ya no van en la flash: se guardan en la SD,
comprimidos, y el STM32 carga a la RAM solo los que se eligen en el menú.

Uso (desde esta carpeta, con Python 3):

  1) Personajes: lee sprites.c (el mismo formato de siempre) y crea un
     archivo .SPR por cada juego de cuadros. Ryu y ryuAzul usan los mismos
     cuadros, así que quedan en un solo RYU.SPR con dos paletas (la segunda
     se usa cuando los dos jugadores eligen al mismo personaje).

        python convertir.py sprites sprites.c

  2) Escenarios: crea un archivo .ESC a partir de
       - un Fondo.bin viejo (formato FND1, el que grababa el STM32),
       - un arreglo RGB565 de 320 de ancho en un .h/.c (Bitmaps.h),
       - o una imagen PNG/JPG/BMP (necesita Pillow: pip install pillow).

        python convertir.py escenario Fondo.bin --nombre "RYU" -o RYU.ESC
        python convertir.py escenario Bitmaps.h --arreglo Escenario2 -o RYU.ESC
        python convertir.py escenario honda.png --piso 200 -o HONDA.ESC

  3) Revisar un archivo: lo descomprime y guarda un PNG para verlo en la PC.

        python convertir.py ver RYU.SPR
        python convertir.py ver RYU.ESC

Copiar los .SPR y .ESC a la raíz de la microSD (FAT32). El menú del juego
muestra todos los que encuentre; para agregar uno basta con copiarlo.

Formato de compresión (LZ, el mismo que descomprime recursos.c):
  Grupos de 8 elementos. Cada grupo empieza con un byte de banderas; el bit 0
  es el primer elemento. Bit en 1: un byte literal. Bit en 0: una copia de
  2 bytes (little-endian) v, que repite (v >> 10) + 3 bytes que están
  (v & 0x3FF) + 1 bytes atrás en lo ya descomprimido.
"""

import argparse
import os
import re
import struct
import sys

# ---------------------------------------------------------------------------
# Compresor LZ
# ---------------------------------------------------------------------------

VENTANA = 1024          # 10 bits de distancia
MIN_COPIA = 3
MAX_COPIA = 63 + 3      # 6 bits de largo


def _mejor_copia(datos, i, cadenas):
    n = len(datos)
    if i + MIN_COPIA > n:
        return 0, 0
    clave = datos[i:i + MIN_COPIA]
    mejor, dist = 0, 0
    lista = cadenas.get(clave)
    if not lista:
        return 0, 0
    tope = min(MAX_COPIA, n - i)
    for j in reversed(lista):
        if i - j > VENTANA:
            break
        l = MIN_COPIA
        while l < tope and datos[j + l] == datos[i + l]:
            l += 1
        if l > mejor:
            mejor, dist = l, i - j
            if l == tope:
                break
    return mejor, dist


def comprimir(datos: bytes) -> bytes:
    """Compresión LZ con búsqueda codiciosa y un paso de evaluación perezosa."""
    datos = bytes(datos)
    n = len(datos)
    cadenas = {}
    sal = bytearray()
    banderas_pos = -1
    nbits = 8
    i = 0

    def agregar_posicion(p):
        if p + MIN_COPIA <= n:
            cadenas.setdefault(datos[p:p + MIN_COPIA], []).append(p)

    def nuevo_elemento(es_literal):
        nonlocal banderas_pos, nbits
        if nbits == 8:
            banderas_pos = len(sal)
            sal.append(0)
            nbits = 0
        if es_literal:
            sal[banderas_pos] |= 1 << nbits
        nbits += 1

    while i < n:
        l, d = _mejor_copia(datos, i, cadenas)
        if l >= MIN_COPIA and i + 1 < n:
            # Evaluación perezosa: si empezando un byte después hay una copia
            # bastante más larga, conviene poner este byte como literal.
            agregar_posicion(i)
            l2, _ = _mejor_copia(datos, i + 1, cadenas)
            cadenas[datos[i:i + MIN_COPIA]].pop()
            if l2 > l + 1:
                l = 0
        if l >= MIN_COPIA:
            nuevo_elemento(False)
            v = (d - 1) | ((l - MIN_COPIA) << 10)
            sal += struct.pack('<H', v)
            for p in range(i, i + l):
                agregar_posicion(p)
            i += l
        else:
            nuevo_elemento(True)
            sal.append(datos[i])
            agregar_posicion(i)
            i += 1
    return bytes(sal)


def descomprimir(datos: bytes, n: int) -> bytes:
    sal = bytearray()
    i = 0
    banderas, nbits = 0, 0
    while len(sal) < n:
        if nbits == 0:
            banderas = datos[i]
            i += 1
            nbits = 8
        if banderas & 1:
            sal.append(datos[i])
            i += 1
        else:
            v = datos[i] | (datos[i + 1] << 8)
            i += 2
            d = (v & 0x3FF) + 1
            l = (v >> 10) + MIN_COPIA
            if d > len(sal):
                raise ValueError('datos LZ dañados')
            for _ in range(l):
                if len(sal) >= n:
                    break
                sal.append(sal[-d])
        banderas >>= 1
        nbits -= 1
    return bytes(sal)


# ---------------------------------------------------------------------------
# Personajes (.SPR)
# ---------------------------------------------------------------------------
#
#   bytes 0-3    'P' 'J' '0' '2'   (el firmware también acepta los 'PJ01' viejos)
#   4-19         nombre (ASCII, relleno con ceros)
#   20, 21, 22   cuerpoW, cuerpoH, margen
#   23           nPaletas
#   24-25        nColores (entradas por paleta, la 0 es transparente)
#   26-27        nCuadros
#   28-29        maxCuadro: el cuadro más grande, w * h
#   30-31        0
#   32-43        6 animaciones {n, activo}: caminar, ko, cubre, dano, golpe, patada
#   44-51        2 zonas de golpe {dx, dy, w, h}: golpe, patada
#   52-55        tamDatos: bytes de cuadros comprimidos
#   56           nPaletas * nColores colores RGB565
#   después      nPaletas nombres de 16 bytes (ASCII, relleno con ceros): el
#                nombre de cada versión de color en el menú ("RYU", "RYU AZUL")
#   después      nCuadros * {w, h, eje, 0, tam (4 bytes)}, en el orden de
#                las animaciones de arriba
#   después      los cuadros comprimidos, uno tras otro

ANIMS = ['caminar', 'ko', 'cubre', 'dano', 'golpe', 'patada']


def nombre_bonito(var):
    """deeJay -> 'DEE JAY'"""
    return re.sub(r'(?<=[a-z0-9])([A-Z])', r' \1', var).upper()


def leer_sprites_c(ruta):
    s = open(ruta, encoding='utf-8', errors='replace').read()
    s_sin_com = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s_sin_com = re.sub(r'//[^\n]*', '', s_sin_com)

    pixeles = {}
    for m in re.finditer(r'const\s+uint8_t\s+(\w+)\s*\[([^\]]*)\]\s*=\s*\{(.*?)\};', s_sin_com, re.S):
        pixeles[m.group(1)] = bytes(int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(3)))

    paletas = {}
    for m in re.finditer(r'const\s+uint16_t\s+(\w+)\s*((?:\[[^\]]*\])+)\s*=\s*\{(.*?)\};', s_sin_com, re.S):
        nombre, dims, cuerpo = m.group(1), m.group(2), m.group(3)
        if dims.count('[') == 2:
            filas = re.findall(r'\{([^{}]*)\}', cuerpo)
            paletas[nombre] = [[int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', f)] for f in filas]
        else:
            paletas[nombre] = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', cuerpo)]

    cuadros = {}
    for m in re.finditer(r'const\s+Cuadro\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};', s_sin_com, re.S):
        lista = []
        for e in re.finditer(r'\{\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', m.group(2)):
            px, w, h, eje = e.group(1), int(e.group(2)), int(e.group(3)), int(e.group(4))
            if px not in pixeles:
                sys.exit(f'No encontré los píxeles de {px}')
            if len(pixeles[px]) != w * h:
                sys.exit(f'{px}: tiene {len(pixeles[px])} píxeles y debería tener {w}x{h}')
            lista.append((px, w, h, eje))
        cuadros[m.group(1)] = lista

    personajes = []
    for m in re.finditer(r'const\s+Personaje\s+(\w+)\s*=\s*\{(.*?)\n\};', s_sin_com, re.S):
        var, cuerpo = m.group(1), m.group(2)

        def campo(nombre):
            mm = re.search(r'\.' + nombre + r'\s*=\s*([^,\n]+(?:\{[^}]*\})?)', cuerpo)
            return mm.group(1).strip() if mm else None

        pj = {'var': var}
        pal = campo('paleta')
        mm = re.match(r'(\w+)\s*(?:\[\s*(\d+)\s*\])?', pal)
        tabla = paletas[mm.group(1)]
        pj['paleta'] = tabla[int(mm.group(2))] if mm.group(2) is not None else tabla
        for c in ('cuerpoW', 'cuerpoH', 'margen'):
            pj[c] = int(campo(c), 0)
        for a in ('caminar', 'ko', 'cubre', 'dano'):
            mm = re.search(r'\.' + a + r'\s*=\s*\{\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', cuerpo)
            pj[a] = (mm.group(1), int(mm.group(2)), int(mm.group(3)))
        mm = re.search(r'\.ataque\s*=\s*\{\s*\{\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}\s*,\s*\{\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', cuerpo)
        pj['golpe'] = (mm.group(1), int(mm.group(2)), int(mm.group(3)))
        pj['patada'] = (mm.group(4), int(mm.group(5)), int(mm.group(6)))
        mm = re.search(r'\.zona\s*=\s*\{\s*\{([^}]*)\}\s*,\s*\{([^}]*)\}', cuerpo)
        pj['zona'] = [tuple(int(x) for x in re.findall(r'\d+', mm.group(k))) for k in (1, 2)]
        personajes.append(pj)

    if not personajes:
        sys.exit('No encontré ningún "const Personaje" en ' + ruta)
    return personajes, cuadros, pixeles


def frames_de(pj, cuadros):
    lista = []
    anims = []
    for a in ANIMS:
        arr, n, activo = pj[a]
        if arr == '0' or n == 0:
            anims.append((0, 0))
            continue
        fs = cuadros[arr][:n]
        anims.append((len(fs), activo))
        lista += fs
    return lista, anims


def escribir_spr(ruta, nombre, grupo, cuadros, pixeles):
    base = grupo[0]
    frames, anims = frames_de(base, cuadros)
    ncol = max(len(pj['paleta']) for pj in grupo)
    paletas = []
    for pj in grupo:
        p = list(pj['paleta']) + [0] * (ncol - len(pj['paleta']))
        paletas.append(p)
    datos = bytearray()
    tabla = bytearray()
    for px, w, h, eje in frames:
        crudo = pixeles[px]
        if max(crudo) >= ncol:
            sys.exit(f'{px} usa el color {max(crudo)} y la paleta solo tiene {ncol}')
        z = comprimir(crudo)
        assert descomprimir(z, w * h) == crudo
        tabla += struct.pack('<BBBBI', w, h, eje, 0, len(z))
        datos += z
    max_cuadro = max(w * h for _, w, h, _ in frames)
    enc = struct.pack('<4s16sBBBBHHHH', b'PJ02', nombre.encode('ascii')[:15],
                      base['cuerpoW'], base['cuerpoH'], base['margen'], len(paletas),
                      ncol, len(frames), max_cuadro, 0)
    for n, act in anims:
        enc += struct.pack('<BB', n, act)
    for z in base['zona']:
        enc += struct.pack('<BBBB', *z)
    enc += struct.pack('<I', len(datos))
    assert len(enc) == 56
    pal = b''.join(struct.pack('<H', c) for p in paletas for c in p)
    nombres = b''.join(nombre_bonito(pj['var']).encode('ascii')[:15].ljust(16, b'\0') for pj in grupo)
    with open(ruta, 'wb') as f:
        f.write(enc + pal + nombres + tabla + datos)
    crudo = sum(w * h for _, w, h, _ in frames)
    ram = len(datos) + 12 * len(frames) + len(pal)
    print(f'{ruta}: {" / ".join(nombre_bonito(pj["var"]) for pj in grupo)}, {len(frames)} cuadros, {len(paletas)} paleta(s). '
          f'Sin comprimir {crudo / 1024:.1f} kB -> {len(datos) / 1024:.1f} kB. '
          f'RAM al cargarlo: {ram / 1024:.1f} kB + {max_cuadro / 1024:.1f} kB por jugador que lo use')


def cmd_sprites(args):
    personajes, cuadros, pixeles = leer_sprites_c(args.entrada)
    grupos = []
    for pj in personajes:
        clave = tuple(pj[a][:2] for a in ANIMS) + (pj['cuerpoW'], pj['cuerpoH'], pj['margen'], tuple(pj['zona']))
        for g in grupos:
            if g[0] == clave:
                g[1].append(pj)
                break
        else:
            grupos.append((clave, [pj]))
    os.makedirs(args.salida, exist_ok=True)
    for _, grupo in grupos:
        var = grupo[0]['var']
        archivo = re.sub(r'[^A-Za-z0-9]', '', var).upper()[:8] + '.SPR'
        escribir_spr(os.path.join(args.salida, archivo), nombre_bonito(var), grupo, cuadros, pixeles)


# ---------------------------------------------------------------------------
# Escenarios (.ESC)
# ---------------------------------------------------------------------------
#
#   bytes 0-3    'E' 'S' 'C' '1'
#   4-19         nombre (ASCII, relleno con ceros)
#   20-21        ancho (siempre 320)
#   22-23        alto (hasta 240; si es menor se centra en la pantalla)
#   24-25        nColores de la paleta (1 a 256)
#   26-27        fila de la pantalla donde se paran los peleadores; 0 = la
#                normal del juego
#   28-29        filasBloque: filas por bloque comprimido (8)
#   30-31        nBloques
#   32-35        tamDatos
#   36           paleta: nColores colores RGB565
#   después      nBloques tamaños (2 bytes cada uno)
#   después      los bloques comprimidos: cada uno son filasBloque filas de
#                320 índices de paleta (el último puede tener menos)

ANCHO = 320
FILAS_BLOQUE = 8


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def a_rgb(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def colores_de(im):
    """Píxeles de una imagen RGB de Pillow, en RGB565"""
    b = im.tobytes()
    return [rgb565(b[i], b[i + 1], b[i + 2]) for i in range(0, len(b), 3)]


def indexar(colores565):
    paleta, indice, px = [], {}, bytearray()
    for c in colores565:
        k = indice.get(c)
        if k is None:
            if len(paleta) == 256:
                return None
            k = indice[c] = len(paleta)
            paleta.append(c)
        px.append(k)
    return paleta, px


def leer_escenario(args):
    ruta = args.entrada
    ext = os.path.splitext(ruta)[1].lower()
    piso = 0
    if ext in ('.h', '.c'):
        if not args.arreglo:
            sys.exit('Con un .h/.c hay que decir el nombre del arreglo: --arreglo Escenario2')
        s = open(ruta, encoding='utf-8', errors='replace').read()
        m = re.search(r'\b' + re.escape(args.arreglo) + r'\s*\[[^\]]*\]\s*(?:PROGMEM\s*)?=\s*\{(.*?)\}', s, re.S)
        if not m:
            sys.exit(f'No encontré el arreglo {args.arreglo} en {ruta}')
        col = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(1))]
        if len(col) % ANCHO:
            sys.exit(f'El arreglo tiene {len(col)} píxeles: no es de {ANCHO} de ancho')
        r = indexar(col)
        if r is None:
            sys.exit('La imagen tiene más de 256 colores; conviértala desde un PNG')
        paleta, px = r
    elif ext == '.bin':
        d = open(ruta, 'rb').read()
        if d[:4] != b'FND1':
            sys.exit(f'{ruta} no es un Fondo.bin (FND1)')
        ancho, alto, ncol, piso = struct.unpack_from('<HHHH', d, 4)
        paleta = list(struct.unpack_from(f'<{ncol}H', d, 12))
        px = bytearray(d[12 + 2 * ncol: 12 + 2 * ncol + ancho * alto])
        if ancho != ANCHO or len(px) != ancho * alto:
            sys.exit(f'{ruta} está incompleto')
    else:
        try:
            from PIL import Image
        except ImportError:
            sys.exit('Para leer imágenes hay que instalar Pillow: pip install pillow')
        im = Image.open(ruta).convert('RGB')
        if im.width != ANCHO:
            alto = round(im.height * ANCHO / im.width)
            im = im.resize((ANCHO, alto), Image.LANCZOS)
            print(f'Aviso: la imagen se escaló a {ANCHO}x{alto}')
        if im.height > 240:
            sobra = im.height - 240
            im = im.crop((0, sobra // 2, ANCHO, sobra // 2 + 240))
            print('Aviso: la imagen se recortó a 240 de alto')
        r = indexar(colores_de(im))
        if r is None:
            im = im.quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).convert('RGB')
            print('Aviso: la imagen tenía más de 256 colores; se redujo a 256')
            r = indexar(colores_de(im))
        paleta, px = r
    if args.piso is not None:
        piso = args.piso
    alto = len(px) // ANCHO
    if not 1 <= alto <= 240:
        sys.exit(f'Alto no válido: {alto}')
    return paleta, bytes(px), alto, piso


def cmd_escenario(args):
    paleta, px, alto, piso = leer_escenario(args)
    nombre = args.nombre or os.path.splitext(os.path.basename(args.salida or args.entrada))[0]
    nombre = nombre.upper()
    salida = args.salida or (re.sub(r'[^A-Za-z0-9]', '', nombre)[:8] or 'FONDO') + '.ESC'
    bloques = []
    blk = ANCHO * FILAS_BLOQUE
    for i in range(0, len(px), blk):
        z = comprimir(px[i:i + blk])
        assert descomprimir(z, len(px[i:i + blk])) == px[i:i + blk]
        bloques.append(z)
    datos = b''.join(bloques)
    enc = struct.pack('<4s16sHHHHHHI', b'ESC1', nombre.encode('ascii', 'replace')[:15], ANCHO, alto,
                      len(paleta), piso, FILAS_BLOQUE, len(bloques), len(datos))
    assert len(enc) == 36
    with open(salida, 'wb') as f:
        f.write(enc)
        f.write(b''.join(struct.pack('<H', c) for c in paleta))
        f.write(b''.join(struct.pack('<H', len(b)) for b in bloques))
        f.write(datos)
    ram = len(datos) + 4 * len(bloques)
    print(f'{salida}: "{nombre}", {ANCHO}x{alto}, {len(paleta)} colores, piso {piso or "normal"}. '
          f'Sin comprimir {len(px) / 1024:.1f} kB -> {len(datos) / 1024:.1f} kB. '
          f'RAM al cargarlo: {ram / 1024:.1f} kB')


# ---------------------------------------------------------------------------
# Revisar un archivo
# ---------------------------------------------------------------------------

def cmd_ver(args):
    try:
        from PIL import Image
    except ImportError:
        sys.exit('Para guardar el PNG hay que instalar Pillow: pip install pillow')
    d = open(args.archivo, 'rb').read()
    salida = args.salida or os.path.splitext(args.archivo)[0] + '.png'
    if d[:4] == b'ESC1':
        nombre, ancho, alto, ncol, piso, fb, nb, tam = struct.unpack_from('<16sHHHHHHI', d, 4)
        pal = struct.unpack_from(f'<{ncol}H', d, 36)
        p = 36 + 2 * ncol
        tams = struct.unpack_from(f'<{nb}H', d, p)
        p += 2 * nb
        px = bytearray()
        for k, t in enumerate(tams):
            n = min(fb, alto - k * fb) * ancho
            px += descomprimir(d[p:p + t], n)
            p += t
        im = Image.new('RGB', (ancho, alto))
        im.putdata([a_rgb(pal[i]) for i in px])
        im.save(salida)
        print(f'{nombre.rstrip(bytes(1)).decode()}: {ancho}x{alto}, {ncol} colores, piso {piso} -> {salida}')
    elif d[:4] in (b'PJ01', b'PJ02'):
        (nombre, cw, ch, margen, npal, ncol, ncu, maxc, _) = struct.unpack_from('<16sBBBBHHHH', d, 4)
        p = 56
        pals = [struct.unpack_from(f'<{ncol}H', d, p + 2 * ncol * k) for k in range(npal)]
        p += 2 * ncol * npal
        if d[:4] == b'PJ02':
            print('Versiones:', ', '.join(d[p + 16 * k:p + 16 * k + 16].rstrip(bytes(1)).decode() for k in range(npal)))
            p += 16 * npal
        tabla = [struct.unpack_from('<BBBBI', d, p + 8 * k) for k in range(ncu)]
        p += 8 * ncu
        imgs = []
        for w, h, eje, _, t in tabla:
            imgs.append((w, h, descomprimir(d[p:p + t], w * h)))
            p += t
        W = sum(w for w, _, _ in imgs) + 4 * len(imgs)
        H = max(h for _, h, _ in imgs)
        hoja = Image.new('RGB', (W, H * npal), (40, 40, 40))
        for k, pal in enumerate(pals):
            x = 0
            for w, h, px in imgs:
                im = Image.new('RGBA', (w, h))
                im.putdata([(0, 0, 0, 0) if i == 0 else a_rgb(pal[i]) + (255,) for i in px])
                hoja.paste(im, (x, k * H + H - h), im)
                x += w + 4
        hoja.save(salida)
        print(f'{nombre.rstrip(bytes(1)).decode()}: {ncu} cuadros, {npal} paleta(s) -> {salida}')
    else:
        sys.exit('No es un archivo .SPR ni .ESC')


def main():
    ap = argparse.ArgumentParser(description='Genera los archivos de la microSD del juego.')
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('sprites', help='sprites.c -> un .SPR por personaje')
    a.add_argument('entrada')
    a.add_argument('-o', '--salida', default='.', help='carpeta donde guardar (por defecto, esta)')
    a.set_defaults(f=cmd_sprites)
    a = sub.add_parser('escenario', help='Fondo.bin / Bitmaps.h / imagen -> .ESC')
    a.add_argument('entrada')
    a.add_argument('-o', '--salida')
    a.add_argument('--nombre', help='nombre que se ve en el menú (máx. 15 letras)')
    a.add_argument('--arreglo', help='nombre del arreglo, si la entrada es un .h/.c')
    a.add_argument('--piso', type=int, help='fila de la pantalla donde se paran los peleadores')
    a.set_defaults(f=cmd_escenario)
    a = sub.add_parser('ver', help='guarda un PNG con el contenido de un .SPR o .ESC')
    a.add_argument('archivo')
    a.add_argument('-o', '--salida')
    a.set_defaults(f=cmd_ver)
    args = ap.parse_args()
    args.f(args)


if __name__ == '__main__':
    main()

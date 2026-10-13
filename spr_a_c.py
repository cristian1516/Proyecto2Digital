#!/usr/bin/env python3
"""
spr_a_c.py - Mete los personajes (.SPR) en la flash del STM32.

Convierte cada .SPR en un archivo .c con sus bytes (comprimidos, igual que
en la SD) y escribe personajes_flash.c con la lista que lee el juego.

Uso (desde la carpeta herramientas):
    python spr_a_c.py RYU.SPR DEEJAY.SPR NOVIAPAL.SPR NOVIAANG.SPR
Opcional:  -o ../Core/Src   carpeta donde se escriben los .c (esa es la de
                            siempre, no hace falta ponerla)

Escribe en Core/Src:
    pj_ryu.c, pj_deejay.c, ...   un arreglo const por personaje (no editar)
    personajes_flash.c           la lista: orden del menú y nombres

Los nombres del menú quedan escritos como texto en personajes_flash.c y se
pueden cambiar ahí a mano (no hay que volver a correr este script).
El orden en que se pasan los archivos es el orden del menú.
Después: compilar y grabar el STM32.
"""
import os
import re
import struct
import sys


def leer_spr(ruta):
    b = open(ruta, 'rb').read()
    if b[:4] not in (b'PJ01', b'PJ02'):
        sys.exit('%s no es un .SPR (no empieza con PJ01/PJ02)' % ruta)
    n_pal = b[23]
    n_col = struct.unpack('<H', b[24:26])[0]
    nombre = b[4:20].split(b'\0')[0].decode('latin-1')
    nombres = []
    if b[3:4] == b'2':
        o = 56 + n_pal * n_col * 2
        for i in range(n_pal):
            nombres.append(b[o + 16 * i:o + 16 * i + 16].split(b'\0')[0].decode('latin-1'))
    else:
        nombres = [nombre] + ['%s %d' % (nombre, i + 1) for i in range(1, n_pal)]
    return b, nombres


def identificador(archivo):
    base = os.path.splitext(os.path.basename(archivo))[0].lower()
    return 'pj_' + re.sub(r'[^a-z0-9_]', '_', base)


def escribir_arreglo(ruta, ident, archivo, datos):
    with open(ruta, 'w', newline='\n') as f:
        f.write('/* %s - generado por herramientas/spr_a_c.py a partir de %s.\n'
                ' * Son los bytes del .SPR tal cual (comprimidos). No editar a mano:\n'
                ' * los nombres del menú se cambian en personajes_flash.c. */\n'
                % (os.path.basename(ruta), os.path.basename(archivo)))
        f.write('#include <stdint.h>\n\n')
        f.write('/* aligned(4): las paletas se leen directo de aquí como uint16_t */\n')
        f.write('const uint8_t %s[%d] __attribute__((aligned(4))) = {\n' % (ident, len(datos)))
        for i in range(0, len(datos), 16):
            f.write('\t' + ', '.join('0x%02X' % x for x in datos[i:i + 16]) + ',\n')
        f.write('};\n')


def main():
    args = sys.argv[1:]
    salida = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'Core', 'Src')
    if '-o' in args:
        k = args.index('-o')
        salida = args[k + 1]
        del args[k:k + 2]
    if not args:
        sys.exit(__doc__)

    entradas = []
    for ruta in args:
        datos, nombres = leer_spr(ruta)
        ident = identificador(ruta)
        archivo = os.path.basename(ruta).upper()
        if len(archivo) > 12:
            sys.exit('%s: el nombre debe ser de 8 letras + .SPR' % archivo)
        c = os.path.join(salida, ident + '.c')
        escribir_arreglo(c, ident, ruta, datos)
        entradas.append((archivo, ident, len(datos), nombres))
        print('%-14s -> %-18s %6.1f KB  %s' % (archivo, ident + '.c', len(datos) / 1024,
                                             ', '.join(nombres)))

    with open(os.path.join(salida, 'personajes_flash.c'), 'w', newline='\n') as f:
        f.write('/*\n * personajes_flash.c - Personajes guardados en la flash (comprimidos).\n'
                ' * Generado por herramientas/spr_a_c.py; se puede editar a mano.\n *\n'
                ' * Cada renglón es un personaje del menú, en este orden. "nombres" es lo\n'
                ' * que se ve en pantalla, uno por cada versión de color (máximo 15\n'
                ' * letras, en MAYÚSCULAS y sin acentos ni Ñ). Si se deja NULL se usa el\n'
                ' * nombre que trae el archivo.\n */\n')
        f.write('#include "personajes_flash.h"\n\n')
        for _, ident, tam, _ in entradas:
            f.write('extern const uint8_t %s[%d];\n' % (ident, tam))
        f.write('\nconst PersonajeFlash PERSONAJES_FLASH[] = {\n')
        for archivo, ident, tam, nombres in entradas:
            noms = ', '.join('"%s"' % n for n in nombres[:4])
            f.write('\t{ "%s", %s, sizeof %s, { %s } },\n' % (archivo, ident, ident, noms))
        f.write('};\n\n')
        f.write('const uint8_t N_PERSONAJES_FLASH = sizeof PERSONAJES_FLASH / sizeof PERSONAJES_FLASH[0];\n')
    total = sum(e[2] for e in entradas)
    print('personajes_flash.c: %d personajes, %.1f KB de flash' % (len(entradas), total / 1024))
    print('Borra de Core/Src los pj_*.c de personajes que ya no estén en la lista.')


if __name__ == '__main__':
    main()

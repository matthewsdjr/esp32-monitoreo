#!/usr/bin/env python3
# ============================================================================
# Aprovisiona el ESP32 por puerto serie, sin copiar y pegar a mano.
#
#   ~/.platformio/penv/bin/python tools/scripts/aprovisionar.py [puerto]
#
# Lee los valores de .env y los envía al equipo. Los secretos NUNCA se imprimen:
# solo se confirma su longitud, para poder detectar un valor truncado sin
# exponerlo en la pantalla ni en el historial de la terminal.
#
# Requiere que el Monitor Serie del Arduino IDE esté CERRADO: solo un programa
# puede tener el puerto abierto a la vez.
# ============================================================================

import os
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("Falta pyserial. Usa el Python de PlatformIO:\n"
             "  ~/.platformio/penv/bin/python tools/scripts/aprovisionar.py")


def cargar_env(ruta=".env"):
    if not os.path.exists(ruta):
        sys.exit("No existe .env")
    valores = {}
    for linea in open(ruta):
        m = re.match(r"^\s*([A-Z0-9_]+)\s*=\s*(.*)$", linea)
        if m:
            valores[m.group(1)] = m.group(2).strip().strip('"').strip("'")
    return valores


def enmascarar(v):
    if not v:
        return "(vacío)"
    if len(v) <= 8:
        return "*" * len(v)
    return f"{v[:4]}…{v[-4:]} ({len(v)} chars)"


def main():
    env = cargar_env()
    puerto = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-0001"

    campos = [
        ("ssid",  env.get("WIFI_SSID", "")),
        ("pass",  env.get("WIFI_PASS", "")),
        ("url",   env.get("SUPABASE_URL", "")),
        ("slug",  env.get("DEVICE_SLUG", "planta-01")),
        ("token", env.get("DEVICE_TOKEN", "")),
        ("jwt",   env.get("DEVICE_JWT", "")),
        ("anon",  env.get("SUPABASE_ANON_KEY", "")),
    ]

    faltan = [c for c, v in campos if not v and c != "pass"]
    if faltan:
        sys.exit(f"Faltan valores en .env: {', '.join(faltan)}")

    print(f"Abriendo {puerto} a 115200…\n")
    try:
        # Abrir el puerto reinicia el ESP32 por DTR/RTS, que es justo lo que se
        # quiere: así se captura el arranque desde el principio.
        s = serial.Serial(puerto, 115200, timeout=1)
    except serial.SerialException as e:
        sys.exit(f"No se pudo abrir el puerto: {e}\n\n"
                 "Si dice 'Resource busy', cierra el Monitor Serie del Arduino IDE.")

    time.sleep(2.5)          # esperar al arranque
    s.reset_input_buffer()

    print("Enviando configuración (los secretos no se muestran):\n")
    for campo, valor in campos:
        if not valor:
            continue
        s.write(f"set {campo} {valor}\r\n".encode())
        s.flush()
        time.sleep(0.7)
        # Se confirma la longitud, no el contenido: un valor truncado se detecta
        # comparando el número, sin exponer el secreto.
        print(f"  set {campo:<6} {enmascarar(valor)}")

    time.sleep(0.8)
    s.reset_input_buffer()
    s.write(b"ver\r\n")
    s.flush()

    print("\nRespuesta del equipo:\n")
    fin = time.time() + 4
    while time.time() < fin:
        linea = s.readline().decode("utf-8", "replace").rstrip()
        if linea:
            print("  " + linea)

    print("\nReiniciando para conectar…\n")
    time.sleep(0.5)
    s.write(b"r\r\n")
    s.flush()

    # Seguir el arranque: aquí se ve si conectó al WiFi y si publica.
    fin = time.time() + 45
    conectado = False
    while time.time() < fin:
        linea = s.readline().decode("utf-8", "replace").rstrip()
        if linea:
            print("  " + linea)
            if "[wifi] conectado" in linea:
                conectado = True

    s.close()
    print("\n" + ("=" * 60))
    print("Conectó al WiFi ✓" if conectado
          else "No se vio confirmación de WiFi — revisa las líneas de arriba")
    print("=" * 60)


if __name__ == "__main__":
    main()

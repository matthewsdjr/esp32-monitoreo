# Puesta en marcha del equipo — paso a paso

Guía para pasar de un ESP32 en la caja a datos reales en el dashboard.
Tiempo estimado: **una hora**, la mayor parte en el cableado.

El backend ya está configurado y validado. Esto es solo el hardware.

---

## Antes de empezar: qué necesitas

| Cosa | Para qué |
|---|---|
| Multímetro | Verificar los 3.3 V del HX711 (paso 1). **No es opcional** |
| Cable USB de datos | Muchos cables de carga NO llevan datos y el equipo no aparece |
| Pesa patrón (500 g o 1 kg) | Calibrar la báscula (paso 8) |
| Cable blindado para la celda | Reducir ruido en la señal |

---

## Paso 1 · Verificar el HX711 ⚠

**Hazlo antes de conectar nada al ESP32.** Es lo único de todo el montaje que
puede dañarlo de forma permanente, y no da ningún síntoma inmediato: el equipo
funciona semanas y luego el pin deja de responder.

1. Alimenta **solo** el módulo HX711 (sin conectarlo al ESP32).
2. Mide con el multímetro entre el pin `DT` y `GND`.

| Lectura | Qué significa |
|---|---|
| **≤ 3.3 V** | Correcto, sigue al paso 2 |
| **~5 V** | **Detente.** Alimenta el HX711 desde `3V3` y vuelve a medir |

Si tras cambiar a 3.3 V sigue dando 5 V, pon un divisor resistivo en `DT`:
1 kΩ en serie y 2 kΩ a tierra.

> El HX711 funciona perfectamente a 3.3 V. Se pierde algo de resolución porque
> el rango escala con la tensión de excitación, pero para una báscula de proceso
> es irrelevante.

---

## Paso 2 · Cablear

Con todo **desconectado de la corriente**.

### HX711 → ESP32

| HX711 | ESP32 |
|---|---|
| `VCC` | `3V3` ⚠ no 5 V |
| `GND` | `GND` |
| `DT` | `GPIO 4` |
| `SCK` | `GPIO 5` |

### Celda de carga → HX711

| Cable | HX711 |
|---|---|
| Rojo | `E+` |
| Negro | `E-` |
| Blanco | `A-` |
| Verde | `A+` |

### SHT31 → ESP32

| SHT31 | ESP32 |
|---|---|
| `VIN` | `3V3` |
| `GND` | `GND` |
| `SDA` | `GPIO 21` |
| `SCL` | `GPIO 22` |

### MAX6675 ×2 → ESP32

`SCK` y `SO` van **compartidos** entre los dos módulos; cada uno lleva su `CS`.

| Señal | ESP32 | Termopar 1 | Termopar 2 |
|---|---|---|---|
| `VCC` | `3V3` | ✓ | ✓ |
| `GND` | `GND` | ✓ | ✓ |
| `SCK` | `GPIO 18` | compartido | compartido |
| `SO` | `GPIO 19` | compartido | compartido |
| `CS` | `GPIO 23` | ✓ | — |
| `CS` | `GPIO 25` | — | ✓ |

Termopar tipo K: **rojo a `−`, amarillo a `+`**. Invertidos, la lectura baja
cuando la temperatura sube.

Detalle completo y consejos de ruido en [`CABLEADO.md`](CABLEADO.md).

---

## Paso 3 · Conectar el ESP32 y encontrar el puerto

Conecta el USB y ejecuta:

```bash
pio device list
```

Debe aparecer algo como `/dev/cu.usbserial-0001` o `/dev/cu.SLAB_USBtoUART`.

**Si no aparece nada**, en orden de probabilidad:

1. El cable es solo de carga. Prueba otro.
2. Falta el driver del conversor USB-serie. Mira el chip junto al conector:
   - **CP2102** → driver de Silicon Labs
   - **CH340/CH9102** → driver de WCH

---

## Paso 4 · Cargar el firmware

```bash
pio run -d firmware -t upload
```

Si aparece `Failed to connect to ESP32`, mantén pulsado el botón **BOOT**
mientras empieza la carga y suéltalo cuando veas `Writing at 0x...`.

---

## Paso 5 · Configurar el equipo

Genera las líneas exactas a pegar (evita transcribir un JWT de 256 caracteres):

```bash
node tools/scripts/lineas-aprovisionamiento.mjs "NOMBRE_DE_TU_RED" "CONTRASEÑA"
```

Abre el monitor serie:

```bash
pio device monitor -b 115200
```

Verás que el equipo anuncia que está sin configurar. **Pega las líneas una por
una**, luego:

```
ver      → revisa la configuración (los secretos salen enmascarados)
r        → reinicia y conecta
```

> **Alternativa sin computadora:** el equipo también levanta un WiFi propio
> llamado `Monitoreo-xxxxx`. Te conectas desde el celular, se abre solo un
> formulario y llenas los campos. Funciona, pero teclear el JWT en un celular es
> penoso — por eso existe la vía serie.

Nada de esto queda en el código: todo va cifrado a la memoria del equipo.

---

## Paso 6 · Verificar que conectó

En el monitor serie, tras el reinicio:

```
=== Monitoreo ESP32 · 2.1.0 ===
[bascula] offset=0 factor=420.000 (0 taras previas)
[sht31] presente
[max6675] listos
[wifi] conectado · IP 192.168.1.87 · -58 dBm
[local] página de servicio en http://monitoreo.local
[sistema] tareas iniciadas · escribe 'd' para diagnóstico

[peso=12.4 temp_amb=22.3 hum=51.8 tc1=24.1 tc2=24.3] faults=0x00 heap=245678
```

`faults=0x00` significa que los cinco canales responden. Si no:

| Valor | Canal en falla |
|---|---|
| `0x01` | Báscula |
| `0x02` | Temperatura ambiente |
| `0x04` | Humedad |
| `0x08` | Termopar 1 |
| `0x10` | Termopar 2 |

Se suman: `0x18` son ambos termopares.

Escribe `d` para un diagnóstico completo con el estado de cada canal.

---

## Paso 7 · Verificar en el dashboard

Abre <https://matthewsdjr.github.io/esp32-monitoreo/>

En menos de un minuto debe aparecer:

- **En línea** en verde, con la señal WiFi real
- Las cinco tarjetas con valores actualizándose cada 2 segundos
- En *Conexión del equipo*: `Internet: Conectado`

Si sigue como fuera de línea después de dos minutos, mira el monitor serie: ahí
estará el motivo (`[red]` con el código de error).

---

## Paso 8 · Tarar y calibrar la báscula

Desde el monitor serie, la página local (`http://monitoreo.local`) o el
dashboard público (requiere el PIN de operador).

**Tara** — con la báscula **vacía** y quieta:

```
t
```

Si responde `TARA RECHAZADA: la señal no está estable`, espera unos segundos a
que se asiente y repite. El rechazo es intencional: tarar sobre una señal que
deriva fija un cero equivocado que después contamina todo el histórico sin dejar
rastro de su origen.

**Calibración** — coloca la pesa patrón y escribe su valor en gramos:

```
cal 1000
```

Verifica: retira el peso (debe marcar ~0 g), vuelve a ponerlo (debe marcar el
valor real ±2 g).

> **Si el peso sale negativo al cargar**, los cables `A+` y `A−` de la celda
> están intercambiados. El firmware rechaza un factor negativo precisamente para
> que esto se detecte al calibrar y no meses después.

---

## Problemas frecuentes

| Síntoma | Causa probable |
|---|---|
| `[sht31] AUSENTE` | Cableado I²C, o el sensor está en `0x45` en vez de `0x44` |
| `termopar abierto` | Sonda desconectada o rota |
| `fallo de bus SPI` | Cableado de `SCK`/`SO`/`CS`, o módulo sin alimentación |
| Peso errático | Ruido: usa cable blindado, aléjalo de cables de potencia |
| Reinicios aleatorios | Fuente insuficiente. Se necesita **1 A**; el WiFi pica a 500 mA |
| No conecta al WiFi | ¿Es red de 5 GHz? El ESP32 **solo funciona en 2.4 GHz** |
| Conecta pero no publica | Revisa `[red]` en el monitor. `401` = token incorrecto |

**Comandos del monitor serie:**

| Comando | Efecto |
|---|---|
| `d` | Diagnóstico completo |
| `ver` | Configuración actual (secretos enmascarados) |
| `t` | Tara |
| `cal <gramos>` | Calibrar con peso patrón |
| `set <campo> <valor>` | Cambiar un dato de configuración |
| `olvidar` | Borrar credenciales y volver al portal |
| `r` | Reiniciar |

---

## Después

Con el equipo publicando, quedan dos cosas:

1. **Cambiar el PIN de operador** por uno propio:
   `node tools/scripts/fijar-pin-operador.mjs TU_PIN`
2. **Dejarlo correr unos días** antes de configurar umbrales de alarma. Con
   datos reales del proceso podrás elegirlos con evidencia; puestos a ojo, o no
   saltan nunca o saltan tanto que el operador aprende a ignorarlos.

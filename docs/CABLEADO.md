# Cableado y conexiones

**Equipo:** ESP32 DevKit v1 · **Firmware:** 2.0.0-fase2

---

## ⚠ Antes de energizar: niveles lógicos del HX711

Es el único punto del montaje que puede dañar el ESP32 de forma permanente, y no
da ningún síntoma inmediato.

Si el módulo HX711 se alimenta con **5 V**, su pin `DT` entrega lógica de 5 V
hacia un GPIO del ESP32, que trabaja a 3.3 V y **no es tolerante a 5 V**. El
equipo puede funcionar durante meses y después degradar el pin: primero
lecturas erráticas, luego el GPIO deja de responder.

Tres soluciones, de mejor a peor:

1. **Alimentar el HX711 a 3.3 V.** Opera correctamente en ese rango. Se pierde
   algo de resolución —el rango de la celda escala con la tensión de
   excitación—, pero para una báscula de proceso es irrelevante.
2. **Divisor resistivo en DT:** 1 kΩ en serie y 2 kΩ a tierra. Cuesta dos
   resistencias.
3. **Conversor de nivel bidireccional.** Correcto pero innecesario para una sola
   línea.

**Verificación:** con el equipo encendido, medir con multímetro entre `DT` y
tierra. Debe leer ≤ 3.3 V. Si marca ~5 V, corregir antes de seguir.

---

## Tabla de conexiones

### HX711 — celda de carga

| HX711 | ESP32 | Nota |
|---|---|---|
| `VCC` | `3V3` | **No 5 V.** Ver aviso arriba |
| `GND` | `GND` | |
| `DT` | `GPIO 4` | Datos |
| `SCK` | `GPIO 5` | Reloj |

Celda de carga al HX711, según el código de colores habitual:

| Cable | HX711 |
|---|---|
| Rojo | `E+` |
| Negro | `E-` |
| Blanco | `A-` |
| Verde | `A+` |

Si el peso sale **negativo** al cargar, intercambiar blanco y verde. El firmware
rechaza un factor de calibración negativo justamente para que esto se detecte al
calibrar y no después.

### SHT31 — temperatura y humedad

| SHT31 | ESP32 |
|---|---|
| `VIN` | `3V3` |
| `GND` | `GND` |
| `SDA` | `GPIO 21` |
| `SCL` | `GPIO 22` |

Dirección I²C `0x44` (con `ADDR` a masa o al aire). Con `ADDR` a `VIN` pasa a
`0x45`, y hay que cambiarlo en `config.h`.

### MAX6675 ×2 — termopares tipo K

`SCK` y `SO` **compartidos**; cada módulo con su propio `CS`.

| Señal | ESP32 | Termopar 1 | Termopar 2 |
|---|---|---|---|
| `VCC` | `3V3` | ✓ | ✓ |
| `GND` | `GND` | ✓ | ✓ |
| `SCK` | `GPIO 18` | compartido | compartido |
| `SO` | `GPIO 19` | compartido | compartido |
| `CS` | `GPIO 23` | ✓ | — |
| `CS` | `GPIO 25` | — | ✓ |

Compartir el bus es correcto: el MAX6675 pone `SO` en alta impedancia mientras
su `CS` está en alto. El firmware los lee **alternados**, nunca simultáneamente
— dos `CS` activos a la vez ponen dos chips a manejar la misma línea y el
resultado es basura.

Termopar tipo K al módulo: **rojo a `−`, amarillo a `+`**. Invertidos, la lectura
baja cuando la temperatura sube.

---

## Diagrama

```
                    ESP32 DevKit v1
                  ┌─────────────────┐
      HX711 DT ───┤ GPIO 4          │
     HX711 SCK ───┤ GPIO 5          │
                  │                 │
    MAX6675 SCK ──┤ GPIO 18   3V3 ├──── VCC de todos los módulos
     MAX6675 SO ──┤ GPIO 19   GND ├──── GND común
                  │                 │
      SHT31 SDA ──┤ GPIO 21         │
      SHT31 SCL ──┤ GPIO 22         │
                  │                 │
    MAX6675 CS1 ──┤ GPIO 23         │
    MAX6675 CS2 ──┤ GPIO 25         │
                  └─────────────────┘
```

Los pines elegidos no tocan pines de *strapping* (0, 2, 12, 15) ni entran en
conflicto con ADC2, que queda inutilizable mientras el WiFi está activo. No
requieren cambios para la Fase 3.

---

## Alimentación

Fuente de **al menos 1 A**. El ESP32 tiene picos de ~500 mA al transmitir WiFi.
Una caída de tensión en ese instante se manifiesta de dos formas que parecen no
tener relación entre sí:

- Reinicios aparentemente aleatorios del equipo.
- Lecturas erráticas del HX711, porque su tensión de excitación cae con la
  alimentación.

Si aparecen ambos síntomas a la vez, la fuente es el primer sospechoso, antes que
el firmware.

---

## Reducción de ruido en la celda de carga

El HX711 amplifica microvoltios: es el elemento más sensible del montaje.

- **Cable blindado** desde la celda, con la malla a tierra **en un solo extremo**
  (el del HX711). Conectarla en ambos crea un bucle de masa que empeora el ruido.
- **Separación de cables de potencia.** Un contactor o variador cerca acopla
  ruido de conmutación directamente.
- **Condensador de 100 nF** entre `VCC` y `GND` del módulo, lo más cerca posible.
- **Montaje mecánico rígido.** Una celda mal sujeta deriva con la temperatura y
  con la vibración; ninguna cantidad de filtrado corrige eso.

El firmware añade una mediana de 9 muestras que descarta picos aislados, pero
filtrar no sustituye a un buen cableado: un filtro puede ocultar el ruido
mientras la deriva sigue corrompiendo la medición.

---

## Prueba de humo

Con todo conectado, abrir la consola serie a **115200 baudios**. A los pocos
segundos debe aparecer:

```
=== Monitoreo ESP32 · 2.0.0-fase2 ===
[bascula] offset=0 factor=420.000 (0 taras previas)
[sht31] presente
[max6675] listos
[sistema] tareas iniciadas · escribe 'd' para diagnóstico

[peso=12.4 temp_amb=22.3 hum=51.8 tc1=24.1 tc2=24.3] faults=0x00 heap=245678
```

| Comando | Efecto |
|---|---|
| `d` | Diagnóstico completo: estado por canal, heap, contadores |
| `t` | Tara (exige señal estable) |
| `cal 1000` | Calibrar con un patrón de 1000 g |
| `r` | Reinicio |

### Interpretación de `faults`

| Valor | Significado |
|---|---|
| `0x00` | Todo correcto |
| `0x01` | Báscula |
| `0x02` | Temperatura ambiente |
| `0x04` | Humedad |
| `0x08` | Termopar 1 |
| `0x10` | Termopar 2 |

Se suman: `0x18` son ambos termopares.

### Fallas comunes

| Síntoma | Causa probable |
|---|---|
| `[sht31] AUSENTE` | Cableado I²C, o dirección `0x45` en vez de `0x44` |
| `termopar abierto` | Sonda desconectada o rota |
| `fallo de bus SPI` | Cableado de `SCK`/`SO`/`CS`, o módulo sin alimentación |
| `peso` en `FUERA_RANGO` | Celda desconectada, o factor de calibración sin ajustar |
| Peso negativo al cargar | Cables `A+`/`A−` de la celda intercambiados |
| `TARA RECHAZADA` | La señal no se ha asentado. Esperar y reintentar |

El firmware **distingue** termopar abierto de fallo de bus a propósito: son
fallas con acciones correctivas distintas —cambiar la sonda frente a revisar el
cableado—, y colapsarlas en un genérico "error" hace perder tiempo en planta.

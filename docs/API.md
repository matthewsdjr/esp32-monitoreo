# Contrato de datos — CONGELADO

**Versión 1.0** · 2026-08-12

Este documento es la fuente de verdad del formato de datos. Lo implementan tres
piezas que deben mantenerse sincronizadas:

| Pieza | Archivo |
|---|---|
| Backend | `supabase/functions/_shared/contrato.ts` |
| Firmware | `firmware/src/config.h` (Fase 3) |
| Simulador | `tools/simulator/simulador.mjs` |

Cambiar este contrato exige cambiar las tres. Por eso está congelado al cierre
de la Fase 1: el firmware y el dashboard se desarrollan en paralelo contra él.

---

## 1. Canales y máscara de fallas

`readings.faults` es un entero donde cada bit indica que un canal está en falla
en ese instante. **Este mapeo es contrato**: alterarlo sin actualizar el firmware
hace que las fallas se atribuyan al sensor equivocado.

| Canal | Columna | Unidad | Bit | Valor de `faults` |
|---|---|---|---|---|
| `peso` | `peso_g` | g | 0 | 1 |
| `temp_amb` | `temp_amb_c` | °C | 1 | 2 |
| `hum` | `hum_pct` | %HR | 2 | 4 |
| `tc1` | `tc1_c` | °C | 3 | 8 |
| `tc2` | `tc2_c` | °C | 4 | 16 |

Ejemplo: `faults = 24` (8 + 16) significa ambos termopares en falla.

**Regla:** si un canal está en falla, su valor debe enviarse como `null`.
Enviar un número junto con el bit de falla activo es contradictorio y el
dashboard lo mostrará como falla, ignorando el número.

---

## 2. Ruta A — Tiempo real (broadcast)

Latencia baja, sin persistencia. No consume invocaciones de Edge Function.

```
POST {SUPABASE_URL}/realtime/v1/api/broadcast
apikey: {ANON_KEY}
Authorization: Bearer {DEVICE_JWT}
Content-Type: application/json
```

```json
{
  "messages": [{
    "topic": "telemetria:planta-01",
    "event": "lectura",
    "private": true,
    "payload": {
      "ts": "2026-08-12T18:30:00.000Z",
      "peso_g": 1248.3,
      "temp_amb_c": 22.4,
      "hum_pct": 51.8,
      "tc1_c": 178.25,
      "tc2_c": 172.50,
      "faults": 0
    }
  }]
}
```

**Cadencia: cada 2 segundos.** No bajar de ahí: a 1 s el consumo sube a
~2.6 M mensajes/mes y excede el límite gratuito de 2 M. Ver ARQUITECTURA.md §6.

**`private: true` es obligatorio.** Activa la evaluación de las políticas RLS
sobre `realtime.messages`. Sin él, el mensaje se rechaza.

**Por qué HTTP y no WebSocket:** implementar el protocolo Phoenix (canales,
heartbeats, reconexión) sobre un ESP32 es mucho código frágil. Un POST con
`Connection: keep-alive` consume exactamente lo mismo en cuota y es trivial de
sostener desde el firmware.

**Autorización:** el `DEVICE_JWT` lleva el claim `device_slug`. La política de
la migración 0002 solo permite publicar en `telemetria:{device_slug}`. Un equipo
no puede publicar en el canal de otro, y la `anon key` —que es pública— no
permite publicar en absoluto.

---

## 3. Ruta B — Persistencia (lotes)

```
POST {SUPABASE_URL}/functions/v1/ingest
Authorization: Bearer {DEVICE_TOKEN}
Content-Type: application/json
```

```json
{
  "device": "planta-01",
  "samples": [
    {
      "ts": "2026-08-12T18:29:30.000Z",
      "peso_g": 1247.9,
      "temp_amb_c": 22.4,
      "hum_pct": 51.9,
      "tc1_c": 177.75,
      "tc2_c": 172.25,
      "faults": 0
    },
    {
      "ts": "2026-08-12T18:29:35.000Z",
      "peso_g": 1248.1,
      "temp_amb_c": 22.4,
      "hum_pct": 51.8,
      "tc1_c": null,
      "tc2_c": 172.50,
      "faults": 8
    }
  ],
  "health": {
    "rssi": -62,
    "uptime_s": 86400,
    "free_heap": 184320,
    "reconnects": 3,
    "fw": "1.0.0"
  }
}
```

**Cadencia: cada 30 segundos**, con las muestras acumuladas a 5 s.
Presupuesto: 2 880 peticiones/día ≈ 86 400/mes sobre un límite de 500 000.

### Respuesta correcta — `200`

```json
{
  "ok": true,
  "recibidas": 6,
  "ultima_ts": "2026-08-12T18:29:55.000Z",
  "servidor_ts": "2026-08-12T18:29:56.412Z"
}
```

El firmware debe vaciar su búfer **solo** al recibir `200`.

`servidor_ts` permite al ESP32 detectar deriva de su propio reloj sin una
consulta NTP adicional.

### Errores

| HTTP | `codigo` | Causa | Qué debe hacer el firmware |
|---|---|---|---|
| 400 | `device_invalido` | `device` ausente o mal formado | Error de configuración: no reintentar |
| 400 | `samples_vacio` | Arreglo vacío | No reintentar |
| 400 | `lote_muy_grande` | > 120 muestras | Partir el lote |
| 400 | `ts_futuro` | Reloj adelantado > 2 min | Resincronizar NTP y reintentar |
| 400 | `ts_antiguo` | Muestra de más de 7 días | Descartar esas muestras |
| 400 | `faults_invalido` | Máscara fuera de rango | No reintentar |
| 401 | — | Token inválido o equipo desactivado | **No reintentar.** Requiere intervención |
| 413 | — | Cuerpo > 256 KB | Partir el lote |
| 429 | — | Más de 10 peticiones/min | Backoff exponencial |
| 500 | — | Error del servidor | Conservar búfer y reintentar con backoff |

**Regla de oro:** ante `4xx` que no sea `429`, no reintentar el mismo lote — se
entraría en un bucle infinito consumiendo cuota. Ante `429` y `5xx`, sí
reintentar con backoff exponencial (tope 60 s).

### Idempotencia

`readings` tiene índice único sobre `(device_id, ts)` y la inserción usa
`ignoreDuplicates`. Reenviar un lote ya recibido devuelve `200` sin duplicar
filas.

Esto es lo que permite que el firmware reintente sin miedo tras un timeout
ambiguo, y que el drenado del búfer offline sea agresivo. **El firmware no
necesita rastrear qué envió con éxito**, que es la principal fuente de bugs en
este tipo de sistemas.

---

## 4. Lectura desde el dashboard

Todas con `apikey: {ANON_KEY}`, solo lectura, sin autenticación de usuario.

| Propósito | Petición |
|---|---|
| Estado del equipo | `GET /rest/v1/devices_publico?slug=eq.planta-01` |
| Definición de canales | `GET /rest/v1/sensors?device_id=eq.{id}&order=orden` |
| Última lectura | `GET /rest/v1/latest_readings?device_id=eq.{id}` |
| Histórico ≤ 24 h | `GET /rest/v1/readings?device_id=eq.{id}&ts=gte.{desde}&order=ts` |
| Histórico > 24 h | `GET /rest/v1/readings_5m?device_id=eq.{id}&bucket=gte.{desde}&order=bucket` |
| Alarmas activas | `GET /rest/v1/alerts?device_id=eq.{id}&cerrada_at=is.null` |
| Umbrales | `GET /rest/v1/thresholds?select=*,sensors(slug)` |

**Selección de fuente:** el dashboard elige `readings` o `readings_5m` según el
rango. El umbral es 24 h. Consultar `readings` para un mes descargaría cientos
de miles de puntos al navegador.

### Suscripciones en vivo

| Canal | Tipo | Contenido |
|---|---|---|
| `telemetria:{slug}` | broadcast, `private: true` | Lectura cada 2 s |
| `alerts` | postgres_changes | Alta y cierre de alarmas |
| `latest_readings` | postgres_changes | Respaldo si el broadcast falla |

`readings` **no** está en la publicación de Realtime a propósito: emitiría un
evento por cada fila insertada hacia todos los clientes conectados, agotando la
cuota sin aportar nada que el broadcast no cubra ya.

### Única escritura permitida a anon

```
POST /rest/v1/rpc/reconocer_alerta
{ "p_alert_id": 42, "p_por": "Supervisor Turno A" }
```

Función `SECURITY DEFINER` de alcance mínimo: solo estampa quién reconoció y
cuándo. No puede cerrar, borrar ni alterar valores de la alarma.

---

## 5. Límites

| Límite | Valor | Definido en |
|---|---|---|
| Muestras por lote | 120 | `LIMITES.MAX_MUESTRAS_LOTE` |
| Tamaño del cuerpo | 256 KB | `LIMITES.MAX_BYTES_CUERPO` |
| Peticiones por minuto | 10 por equipo | `LIMITES.MAX_PETICIONES_MIN` |
| Deriva hacia el futuro | 120 s | `LIMITES.DERIVA_FUTURA_MAX_S` |
| Antigüedad máxima | 7 días | `LIMITES.ANTIGUEDAD_MAX_DIAS` |
| Retención de crudo | 14 días | `fn_purgar()` |
| Retención de agregados | 24 meses | `fn_purgar()` |

---

## 6. Comandos hacia el equipo (tara, calibración)

El ESP32 está detrás de NAT y no acepta conexiones entrantes: no se le puede
"llamar". Los comandos viajan **de vuelta en la respuesta al POST de `/ingest`**
que el propio equipo hace cada 30 s. Cero conexiones nuevas, cero puertos
abiertos, cero invocaciones extra.

El precio es la latencia: hasta 30 s entre pulsar el botón y la ejecución. Por
eso la interfaz muestra el estado del comando en vez de fingir que fue
instantáneo.

### Encolar — `POST /functions/v1/comando`

```json
{
  "device": "planta-01",
  "comando": "tara",
  "pin": "PIN_DE_OPERADOR",
  "solicitado_por": "Supervisor Turno A",
  "parametros": null
}
```

| Comando | Parámetros | Efecto en el equipo |
|---|---|---|
| `tara` | — | Toma el peso actual como cero y lo guarda en NVS |
| `calibrar` | `{"peso_conocido_g": 1000}` | Ajusta el factor de escala con un peso patrón |
| `reiniciar` | — | Reinicio controlado |
| `recargar_umbrales` | — | Relee `thresholds` para la evaluación local |

**El PIN es obligatorio.** El dashboard es público: sin esta barrera cualquiera
con el enlace podría poner la báscula en cero a media producción. Se valida
contra un hash en el servidor, con límite de 5 intentos por IP cada 15 min. Se
fija con `node tools/scripts/fijar-pin-operador.mjs <PIN>`; mientras esté vacío,
los comandos remotos están **desactivados**.

| HTTP | Causa |
|---|---|
| 401 | PIN incorrecto |
| 409 | Ya hay un comando de ese tipo pendiente |
| 429 | Demasiados intentos fallidos |
| 503 | PIN no configurado: comandos desactivados |

### Entrega — respuesta de `/ingest`

```json
{
  "ok": true,
  "recibidas": 6,
  "servidor_ts": "2026-08-13T18:29:56.412Z",
  "comandos": [
    { "id": 42, "comando": "tara", "parametros": null }
  ]
}
```

Los comandos se marcan `entregado` en cuanto salen en esta respuesta. Si el
equipo se reinicia antes de ejecutarlos, se pierden — **y eso es lo correcto**:
reintentar una tara a ciegas sobre una báscula que ya volvió a cargarse haría
más daño que no hacer nada.

Caducan a los 10 minutos sin entregar. Una tara que llega tres días tarde,
cuando el equipo por fin enciende, es peor que ninguna tara.

### Acuse — siguiente `POST /ingest`

```json
{
  "device": "planta-01",
  "samples": [ /* … */ ],
  "resultados": [
    { "id": 42, "ok": true, "detalle": { "offset_anterior": 8421, "offset_nuevo": 8395 } }
  ]
}
```

Un `resultados` mal formado se descarta en silencio en vez de rechazar el lote:
perder el acuse de una tara es molesto; perder 30 s de lecturas del proceso es
peor.

**Responsabilidad del firmware:** ejecutar cada comando recibido y reportar su
resultado en el siguiente lote. Si no lo reporta, el comando queda en
`entregado` y la interfaz lo muestra como no confirmado — que es información
honesta, no un fallo.

# Plan de Trabajo — Sistema de Monitoreo Remoto ESP32

**Documento complementario a** [`ARQUITECTURA.md`](./ARQUITECTURA.md)
**Fecha:** 2026-08-12

Estimación total: **19–26 días de trabajo efectivo** para una persona. Con dedicación de medio tiempo, aproximadamente 7–9 semanas calendario.

Las fases 2, 3 y 4 son en buena medida paralelizables si hay más de una persona: firmware y frontend solo se acoplan a través del contrato de datos, que queda congelado al final de la Fase 1.

---

## Fase 0 — Verificación y preparación
**2 días · Bloqueante para todo lo demás**

Esta fase existe porque los tres hallazgos que pueden hacer descarrilar el proyecto se descubren aquí, no en la semana seis.

| # | Tarea | Entregable |
|---|---|---|
| 0.1 | Confirmar con TI: tipo de seguridad de la red de planta (PSK vs. Enterprise), salida HTTPS al 443 sin filtrado, ausencia de portal cautivo, registro de MAC si aplica | Correo o ticket con la confirmación |
| 0.2 | Prueba de humo: ESP32 conectado a la red haciendo `GET https://supabase.com` con validación de certificado | Sketch de prueba y captura del resultado |
| 0.3 | Verificar niveles lógicos del HX711 (§10 de arquitectura) y corregir si opera a 5 V | Medición con multímetro documentada |
| 0.4 | Crear proyecto Supabase, repositorio GitHub público, bot de Telegram | Credenciales en gestor de secretos |
| 0.5 | Migrar de Arduino IDE a PlatformIO; estructura del monorepo; `.gitignore` y `gitleaks` en CI | Repositorio inicializado que compila |

> **Si 0.1 o 0.2 fallan**, hay que detenerse y evaluar el plan B (conectividad celular). Descubrirlo aquí cuesta dos días; descubrirlo en la Fase 5 cuesta el proyecto.

---

## Fase 1 — Contrato de datos y backend
**3 días**

| # | Tarea | Entregable |
|---|---|---|
| 1.1 | Migraciones SQL del esquema completo (§5.1) | `supabase/migrations/0001_init.sql` |
| 1.2 | Políticas RLS y vista pública de `devices` | `0002_rls.sql` verificado con pruebas de intento de escritura anónima |
| 1.3 | Edge Function `/ingest`: validación de token, rate limiting, inserción por lotes, UPSERT a `latest_readings` | Función desplegada con pruebas |
| 1.4 | Trigger de evaluación de umbrales + Edge Function `/notify` (Telegram y correo) | Alarma de prueba recibida en el celular |
| 1.5 | Trabajos `pg_cron` de rollup y purga | Ejecución verificada con datos sintéticos |
| 1.6 | **Congelar el contrato del payload** y documentarlo | `docs/API.md` con esquemas JSON de ambas rutas |
| 1.7 | Generador de datos sintéticos | Script que alimenta el backend sin hardware |

El punto 1.7 no es opcional. Permite que el desarrollo del frontend avance sin depender del ESP32, y desbloquea trabajo en paralelo desde este momento.

---

## Fase 2 — Firmware: refactor y sensores
**5 días**

| # | Tarea | Entregable |
|---|---|---|
| 2.1 | Reestructurar a modelo de tareas FreeRTOS (§4.2) | `main.cpp` con las siete tareas |
| 2.2 | Driver HX711 no bloqueante con filtro de mediana y media móvil | `sensors/hx711.cpp` |
| 2.3 | Driver SHT31 con reintento de inicialización y validación de rango — **elimina el `while(1)` bloqueante** | `sensors/sht31.cpp` |
| 2.4 | Driver MAX6675 con cadencia de 250 ms y lectura de trama cruda para detectar termopar abierto | `sensors/max6675.cpp` |
| 2.5 | Máquina de estados por canal (OK / STALE / OUT_RANGE / FAULT / DISABLED) | `sensors/state.cpp` |
| 2.6 | Persistencia en NVS: factor de calibración, tara, umbrales, credenciales | `storage/nvs.cpp` |
| 2.7 | Pruebas unitarias de filtros y máquina de estados | Suite en `firmware/test/` |

**Criterio de salida:** las cinco lecturas salen por consola serie con su estado, sostenido durante 1 h sin reinicios ni fugas de memoria, y desconectar cualquier sensor no afecta a los demás.

---

## Fase 3 — Firmware: red y conectividad
**4 días**

| # | Tarea | Entregable |
|---|---|---|
| 3.1 | Portal cautivo de provisionamiento WiFi (WiFiManager) — credenciales fuera del código | `net/wifi.cpp` |
| 3.2 | Cliente TLS con CA embebida y validación estricta de certificado | `net/tls.cpp` |
| 3.3 | Ruta A: cliente WebSocket a Supabase Realtime, broadcast cada 2 s | `net/realtime.cpp` |
| 3.4 | Ruta B: cliente HTTPS con lotes cada 30 s y backoff exponencial | `net/ingest.cpp` |
| 3.5 | Búfer circular en RAM + respaldo en LittleFS; drenado ordenado al reconectar | `storage/buffer.cpp` |
| 3.6 | NTP, heartbeat de telemetría del equipo, watchdog | `net/ntp.cpp`, `net/health.cpp` |
| 3.7 | OTA por HTTPS con partición dual y rollback | `net/ota.cpp` |
| 3.8 | Servidor web local para calibración y diagnóstico en sitio | `ui/local_server.cpp` |

**Criterio de salida:** desconectar el router 30 min y volverlo a conectar deja el histórico completo, sin huecos y con las marcas de tiempo originales.

---

## Fase 4 — Dashboard
**5 días**

| # | Tarea | Entregable |
|---|---|---|
| 4.1 | Andamiaje Vite + React + TS + Tailwind; `base` correcto para Pages | `web/` compilando |
| 4.2 | Cliente Supabase, tipos TypeScript generados desde el esquema | `lib/supabase.ts`, `types/db.ts` |
| 4.3 | Hook de tiempo real con degradación a polling y reconexión automática | `hooks/useRealtime.ts` |
| 4.4 | Tarjetas de sensor con semáforo, tendencia y sparkline | `components/SensorCard.tsx` |
| 4.5 | Barra de estado del equipo: online/offline, RSSI, uptime, antigüedad del dato | `components/StatusBar.tsx` |
| 4.6 | Vista histórica: selector de rango, selección automática de fuente (crudo vs. 5 min), zoom, bandas de alarma | `components/HistoryChart.tsx` |
| 4.7 | Tabla de alarmas con reconocimiento | `components/AlertTable.tsx` |
| 4.8 | Exportación CSV y PNG | `lib/export.ts` |
| 4.9 | Vista de diagnóstico (RSSI, heap, latencia `received_at − ts`) | `components/Diagnostics.tsx` |
| 4.10 | Responsivo, tema claro/oscuro, PWA instalable, accesibilidad AA | Auditoría Lighthouse ≥ 90 |
| 4.11 | Workflow de despliegue a GitHub Pages | `deploy-pages.yml` funcionando |

Todo esto se desarrolla contra el generador sintético de la Fase 1; no requiere hardware presente.

---

## Fase 5 — Integración y calibración
**3 días**

| # | Tarea | Entregable |
|---|---|---|
| 5.1 | Integración extremo a extremo con hardware real | Dato físico visible en la URL pública |
| 5.2 | Calibración del HX711 con pesa patrón; procedimiento documentado y repetible | `docs/CALIBRACION.md` + certificado de calibración |
| 5.3 | Verificación de termopares contra referencia (hielo 0 °C y agua en ebullición) | Tabla de error documentada |
| 5.4 | Ajuste de umbrales de alarma con el proceso real | `thresholds` poblada y validada |
| 5.5 | Medición de latencia extremo a extremo | Reporte contra el criterio de ≤ 3 s p95 |

---

## Fase 6 — Endurecimiento y pruebas
**3 días**

| # | Tarea | Entregable |
|---|---|---|
| 6.1 | Prueba de resistencia de 72 h; vigilancia de heap y reinicios | Reporte de estabilidad |
| 6.2 | Inyección de fallas: desconectar cada sensor, cortar WiFi, cortar energía, saturar la celda | Matriz de fallas vs. comportamiento observado |
| 6.3 | Prueba de carga del dashboard con visitantes concurrentes | Reporte de comportamiento y límite práctico |
| 6.4 | Auditoría de seguridad: intento de escritura anónima, escaneo de secretos en todo el historial, revisión de RLS | Reporte sin hallazgos abiertos |
| 6.5 | Validación cruzada de navegadores y dispositivos móviles | Matriz de compatibilidad |
| 6.6 | Verificación de los 9 criterios de aceptación | Lista firmada |

---

## Fase 7 — Documentación y entrega
**2 días**

| # | Tarea | Entregable |
|---|---|---|
| 7.1 | Diagrama de cableado y tabla de conexiones | `docs/CABLEADO.md` |
| 7.2 | Manual de operación para usuario final, en español y sin jerga | `docs/OPERACION.md` |
| 7.3 | Runbook: qué hacer ante cada falla común | `docs/RUNBOOK.md` |
| 7.4 | README con capturas, URL del dashboard y guía de arranque | `README.md` |
| 7.5 | Capacitación al personal de planta | Sesión impartida |
| 7.6 | Plan de mantenimiento: recalibración trimestral, revisión de cuotas, actualización de CA | `docs/MANTENIMIENTO.md` |

---

## Camino crítico

```
Fase 0 ──> Fase 1 ──┬──> Fase 2 ──> Fase 3 ──┬──> Fase 5 ──> Fase 6 ──> Fase 7
                    └──> Fase 4 ─────────────┘
```

El cuello de botella real es la cadena de firmware (Fases 2 y 3, nueve días). El frontend corre en paralelo desde el fin de la Fase 1 gracias al generador de datos sintéticos. Si hay dos personas, el calendario se comprime a unos 15 días.

---

## Hitos de revisión

| Hito | Al terminar | Qué se demuestra |
|---|---|---|
| **H1** | Fase 0 | La red permite el diseño; no hay sorpresas de infraestructura |
| **H2** | Fase 1 | Datos sintéticos fluyendo; contrato congelado |
| **H3** | Fase 3 | Hardware real publicando a la nube de forma estable |
| **H4** | Fase 4 | Dashboard público en línea con datos sintéticos |
| **H5** | Fase 5 | Sistema completo con dato físico calibrado |
| **H6** | Fase 7 | Entrega formal |

---

## Acciones inmediatas

Tres cosas que conviene hacer antes de escribir una línea de código nuevo:

1. **Rotar la contraseña de la red WiFi si ya se compartió fuera de canales seguros**, y en cualquier caso no dejar que llegue nunca al repositorio público. El provisionamiento por portal cautivo (Fase 3.1) es la solución permanente.
2. **Levantar el ticket con TI** (Fase 0.1). Suele ser lo de mayor tiempo de espera y no depende del equipo de desarrollo.
3. **Confirmar que la exposición pública de los datos de proceso es intencional** (§7.5 de arquitectura). Cambiar a acceso autenticado más adelante es sencillo, pero un dato ya publicado no se puede despublicar.

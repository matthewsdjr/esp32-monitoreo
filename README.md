# Monitoreo remoto de sensores ESP32

Telemetría en tiempo real de una línea de producción: dos termopares tipo K
(MAX6675), temperatura y humedad ambiente (SHT31) y una celda de carga (HX711).
Los datos se publican desde un ESP32 hacia la nube y se visualizan en un
dashboard público alojado en GitHub Pages.

**Dashboard:** <https://matthewsdjr.github.io/esp32-monitoreo/>

> Backend en producción y conectado. La página muestra datos reales del equipo
> `planta-01`; mientras el ESP32 no esté en línea, aparecerá como fuera de línea
> con su última lectura conocida.

---

## Cómo funciona

El ESP32 vive detrás del NAT de la red local, y GitHub Pages solo sirve
contenido estático por HTTPS. Por eso la página **no consulta al ESP32**: el
flujo va al revés. El equipo publica hacia afuera y el dashboard se suscribe.

```
ESP32 ──┬─ broadcast cada 2 s ──→ Supabase Realtime ──→ Dashboard (tiempo real)
        │
        └─ lote HTTPS cada 30 s ─→ Edge Function ──→ PostgreSQL ──→ Dashboard (histórico)
```

Las dos rutas existen porque optimizan cosas distintas: la primera da latencia
de 2 s sin tocar disco ni consumir invocaciones; la segunda da histórico,
alarmas y auditoría. Juntas mantienen el proyecto dentro del plan gratuito.

El detalle completo está en [`docs/ARQUITECTURA.md`](docs/ARQUITECTURA.md).

---

## Arranque rápido

El dashboard funciona **sin backend ni hardware**: si no encuentra credenciales
de Supabase arranca en modo demostración con datos simulados en el navegador.

```bash
cd web
npm install
npm run dev
```

Abre <http://localhost:5173>. Deberías ver las cinco tarjetas actualizándose
cada 2 segundos, el histórico y una alarma activa de ejemplo.

---

## Estructura

| Carpeta | Contenido |
|---|---|
| `docs/` | Arquitectura, plan de trabajo y contrato de datos |
| `supabase/` | Migraciones SQL, Edge Functions y pruebas |
| `web/` | Dashboard (Vite + React + TypeScript) |
| `tools/` | Alta de equipos, emisión de JWT y simulador del ESP32 |
| `firmware/` | Firmware del ESP32 — sensores, red, búfer offline y OTA |

---

## Puesta en marcha del backend

```bash
# 1. Aplicar el esquema al proyecto de Supabase
supabase link --project-ref TU_PROYECTO
supabase db push
supabase functions deploy ingest
supabase functions deploy notify

# 2. Dar de alta el equipo (muestra el token UNA sola vez)
cp .env.example .env          # y completar
node tools/scripts/registrar-equipo.mjs planta-01 "Línea 1" "Planta principal"
node tools/scripts/emitir-jwt.mjs planta-01

# 2b. Habilitar los comandos remotos (tara). Sin PIN quedan DESACTIVADOS.
node tools/scripts/fijar-pin-operador.mjs TU_PIN

# 3. Simular el ESP32 para verificar el flujo completo
node tools/simulator/simulador.mjs

# 4. Verificar que el canal en vivo es seguro (no basta con mirar el HTTP:
#    el endpoint responde 202 antes de evaluar la autorización)
node tools/scripts/probar-canal-vivo.mjs
```

Verificar las migraciones contra un Postgres desechable, sin tocar producción:

```bash
./tools/scripts/test-db.sh
```

---

## Firmware

```bash
make -C firmware test     # lógica: filtros, calibración, estados (sin hardware)
pio run -d firmware       # compilar para ESP32
pio run -d firmware -t upload
pio device monitor -b 115200
```

Cableado y prueba de humo en [`docs/CABLEADO.md`](docs/CABLEADO.md). **Antes de
energizar**, verificar los niveles lógicos del HX711: alimentado a 5 V puede
dañar de forma permanente un GPIO del ESP32.

### Puesta en marcha del equipo

**Guía completa paso a paso: [`docs/PUESTA-EN-MARCHA.md`](docs/PUESTA-EN-MARCHA.md)**

El firmware no lleva ninguna credencial dentro. Se aprovisiona por puerto serie
(recomendado: permite pegar el JWT de 256 caracteres) o por portal cautivo desde
el celular. Todo queda cifrado en la NVS del equipo.

```bash
./tools/scripts/generar-ca.sh https://TU_PROYECTO.supabase.co   # anclas TLS
pio run -d firmware -t upload
# Configura el equipo por USB leyendo los valores de .env (recomendado)
~/.platformio/penv/bin/python tools/scripts/aprovisionar.py
```

También se puede a mano: `lineas-aprovisionamiento.mjs` imprime las líneas para
pegarlas en `pio device monitor -b 115200`, o llenar el formulario del portal
cautivo desde el celular.

Ya en marcha, el equipo publica una página de servicio en `http://monitoreo.local`
(solo desde la red de planta) con estado, diagnóstico, tara y calibración —
útil precisamente cuando no hay internet y el dashboard público no sirve.

---

## Seguridad

Este repositorio es **público**. Tres reglas:

1. **Las credenciales WiFi nunca entran al código.** El ESP32 se aprovisiona con
   un portal cautivo y las guarda cifradas en NVS.
2. **El token de ingesta del equipo tampoco.** Vive en la NVS; en la base solo
   se guarda su hash SHA-256.
3. **El botón de tara exige un PIN de operador.** La página es pública: sin esa
   barrera, cualquiera con el enlace podría poner la báscula en cero a media
   producción. El PIN se valida en el servidor y nunca viaja en el bundle.
4. **La `anon key` de Supabase sí es pública**, por diseño. Su seguridad no
   depende del secreto sino de las políticas RLS, que solo conceden lectura.
   La `service_role` jamás debe aparecer en el frontend ni en el firmware.

Un workflow de CI escanea todo el historial de git en cada push
([`gitleaks`](.github/workflows/secret-scan.yml)). Un secreto en un commit sigue
siendo público aunque se borre después.

---

## Despliegue

Cada push a `main` que toque `web/` construye y publica el dashboard
automáticamente. Antes del primer despliegue:

1. En el repositorio: **Settings → Pages → Source: GitHub Actions**.
2. En **Settings → Secrets and variables → Actions → Variables**, agregar
   `VITE_SUPABASE_URL`, `VITE_SUPABASE_ANON_KEY` y `VITE_DEVICE_SLUG`.

Sin esas variables el despliegue funciona igual, pero publica el modo
demostración.

---

## Estado

| Fase | Estado |
|---|---|
| 0 · Verificación de red y preparación | Pendiente — requiere confirmación de TI |
| 1 · Contrato de datos y backend | ✅ Completa y probada |
| 2 · Firmware: sensores | ✅ Compila y probado (74 aserciones) |
| 3 · Firmware: red | ✅ Compila y probado (90 aserciones) |
| 4 · Dashboard | ✅ En línea con datos reales |
| 5 · Integración y calibración | ✅ ESP32 real publicando; falta calibrar |
| 6 · Endurecimiento y pruebas | Pendiente |
| 7 · Documentación y entrega | En curso |

Plan detallado en [`docs/PLAN-DE-TRABAJO.md`](docs/PLAN-DE-TRABAJO.md).

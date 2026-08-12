-- ============================================================================
-- 0001 · Esquema base del sistema de monitoreo
-- ============================================================================
-- Ver docs/ARQUITECTURA.md §5 para la justificación del modelo.
-- Decisión clave: `readings` es una FILA ANCHA (todos los canales por instante)
-- en vez del modelo largo (sensor_id, ts, valor). Con un conjunto fijo de 5
-- canales es ~5x más eficiente en almacenamiento e índices, y hace trivial la
-- consulta multi-serie para las gráficas. La columna `extra jsonb` conserva la
-- extensibilidad si se agregan sensores sin necesidad de migrar.
-- ============================================================================

create extension if not exists pgcrypto;

-- ----------------------------------------------------------------------------
-- Equipos
-- ----------------------------------------------------------------------------
create table if not exists public.devices (
  id           uuid primary key default gen_random_uuid(),
  slug         text not null unique
               check (slug ~ '^[a-z0-9][a-z0-9-]{1,38}[a-z0-9]$'),
  nombre       text not null,
  ubicacion    text,

  -- SHA-256 (hex) del token de ingesta. Ver nota de seguridad abajo.
  token_hash   text not null,
  token_rotado_at timestamptz,

  fw_version   text,
  last_seen_at timestamptz,

  -- Telemetría del propio equipo, sobrescrita en cada heartbeat
  rssi         smallint,
  uptime_s     integer,
  free_heap    integer,
  reconnects   integer,

  activo       boolean not null default true,
  created_at   timestamptz not null default now()
);

-- Nota de seguridad sobre `token_hash`:
-- Se usa SHA-256 y no un KDF lento (Argon2/bcrypt) de forma deliberada. Esos
-- están diseñados para secretos de BAJA entropía elegidos por humanos, donde
-- hay que encarecer la fuerza bruta. El token de ingesta lo genera el script
-- de alta con 32 bytes de aleatoriedad criptográfica (256 bits): es
-- computacionalmente inatacable por fuerza bruta, y SHA-256 evita añadir
-- decenas de milisegundos de CPU a CADA petición de ingesta.

comment on column public.devices.token_hash is
  'SHA-256 hex del token de ingesta (32 bytes aleatorios). Nunca se guarda el token en claro.';

-- ----------------------------------------------------------------------------
-- Definición de canales
-- ----------------------------------------------------------------------------
create table if not exists public.sensors (
  id          bigserial primary key,
  device_id   uuid not null references public.devices(id) on delete cascade,

  slug        text not null,   -- 'peso' | 'temp_amb' | 'hum' | 'tc1' | 'tc2'
  etiqueta    text not null,   -- 'Báscula tolva 1'
  tipo        text not null check (tipo in ('masa','temperatura','humedad','otro')),
  unidad      text not null,   -- 'g' | '°C' | '%HR'

  -- Rango físicamente plausible. Fuera de esto la lectura se marca OUT_RANGE:
  -- casi siempre significa cableado suelto o sensor dañado, no un valor real.
  min_fisico  double precision,
  max_fisico  double precision,

  -- Bit que este canal ocupa en readings.faults
  bit_falla   smallint not null check (bit_falla between 0 and 14),

  decimales   smallint not null default 1,
  orden       smallint not null default 0,
  activo      boolean  not null default true,

  unique (device_id, slug),
  unique (device_id, bit_falla)
);

-- ----------------------------------------------------------------------------
-- Lecturas crudas
-- ----------------------------------------------------------------------------
create table if not exists public.readings (
  id          bigserial primary key,
  device_id   uuid not null references public.devices(id) on delete cascade,

  -- Reloj del dispositivo (sincronizado por NTP)
  ts          timestamptz not null,
  -- Reloj del servidor al recibir. La diferencia ts <-> received_at revela
  -- latencia de red y drenado de búfer offline sin ninguna ambigüedad.
  received_at timestamptz not null default now(),

  peso_g      double precision,
  temp_amb_c  double precision,
  hum_pct     double precision,
  tc1_c       double precision,
  tc2_c       double precision,

  -- Máscara de bits: 1 = canal en falla en este instante. Ver sensors.bit_falla.
  faults      smallint not null default 0,

  extra       jsonb,

  -- Idempotencia: si el ESP32 reintenta un lote tras un timeout ambiguo, la
  -- reinserción no duplica filas. Es la razón por la que el drenado del búfer
  -- offline puede ser agresivo sin ensuciar el histórico.
  unique (device_id, ts)
);

create index if not exists readings_device_ts_idx
  on public.readings (device_id, ts desc);

-- Índice parcial: las consultas de "¿qué se rompió?" solo miran filas con falla.
create index if not exists readings_faults_idx
  on public.readings (device_id, ts desc)
  where faults <> 0;

-- ----------------------------------------------------------------------------
-- Última lectura por equipo (una sola fila, se sobrescribe)
-- ----------------------------------------------------------------------------
-- Existe para que el dashboard pinte al instante en la carga inicial sin tener
-- que ordenar la tabla grande, y para que Realtime emita un payload mínimo.
create table if not exists public.latest_readings (
  device_id   uuid primary key references public.devices(id) on delete cascade,
  ts          timestamptz not null,
  received_at timestamptz not null default now(),
  payload     jsonb not null
);

-- ----------------------------------------------------------------------------
-- Umbrales de alarma
-- ----------------------------------------------------------------------------
create table if not exists public.thresholds (
  id             bigserial primary key,
  sensor_id      bigint not null references public.sensors(id) on delete cascade,

  warn_low       double precision,
  warn_high      double precision,
  alarm_low      double precision,
  alarm_high     double precision,

  -- Histéresis: la alarma no cierra hasta que el valor regresa este margen
  -- DENTRO del umbral. Sin esto, una señal con ruido oscilando sobre el límite
  -- genera decenas de eventos por minuto y el operador acaba ignorándolos todos.
  histeresis     double precision not null default 0 check (histeresis >= 0),

  -- La condición debe sostenerse N segundos antes de disparar. Filtra picos
  -- transitorios que no representan un problema real de proceso.
  duracion_min_s integer not null default 30 check (duracion_min_s >= 0),

  activo         boolean not null default true,
  updated_at     timestamptz not null default now(),

  unique (sensor_id),

  constraint umbrales_coherentes check (
    (warn_low  is null or alarm_low  is null or warn_low  >= alarm_low) and
    (warn_high is null or alarm_high is null or warn_high <= alarm_high)
  )
);

-- ----------------------------------------------------------------------------
-- Historial de eventos de alarma
-- ----------------------------------------------------------------------------
create table if not exists public.alerts (
  id             bigserial primary key,
  sensor_id      bigint not null references public.sensors(id) on delete cascade,
  device_id      uuid   not null references public.devices(id) on delete cascade,

  nivel          text not null check (nivel in ('warning','alarm')),
  -- 'falla' cubre sensor desconectado / termopar abierto / error de bus, que
  -- operativamente es tan grave como rebasar un umbral: el proceso queda ciego.
  direccion      text not null check (direccion in ('low','high','falla')),

  valor_disparo  double precision,
  valor_pico     double precision,
  umbral         double precision,

  abierta_at     timestamptz not null default now(),
  cerrada_at     timestamptz,

  reconocida_por text,
  reconocida_at  timestamptz,

  notificada_at  timestamptz
);

-- Una sola alarma abierta por sensor y nivel: impide duplicados si el trigger
-- se ejecuta varias veces mientras la condición persiste.
create unique index if not exists alerts_abierta_unica_idx
  on public.alerts (sensor_id, nivel)
  where cerrada_at is null;

create index if not exists alerts_device_abierta_idx
  on public.alerts (device_id, abierta_at desc);

-- ----------------------------------------------------------------------------
-- Agregados de 5 minutos
-- ----------------------------------------------------------------------------
-- El dashboard consulta esta tabla para rangos > 24 h. Sin ella, la gráfica de
-- "último mes" tendría que descargar ~500 000 puntos al navegador.
create table if not exists public.readings_5m (
  device_id       uuid not null references public.devices(id) on delete cascade,
  bucket          timestamptz not null,

  peso_g_avg      double precision,
  peso_g_min      double precision,
  peso_g_max      double precision,
  temp_amb_c_avg  double precision,
  temp_amb_c_min  double precision,
  temp_amb_c_max  double precision,
  hum_pct_avg     double precision,
  hum_pct_min     double precision,
  hum_pct_max     double precision,
  tc1_c_avg       double precision,
  tc1_c_min       double precision,
  tc1_c_max       double precision,
  tc2_c_avg       double precision,
  tc2_c_min       double precision,
  tc2_c_max       double precision,

  n_muestras      integer not null,
  n_fallas        integer not null default 0,

  primary key (device_id, bucket)
);

create index if not exists readings_5m_bucket_idx
  on public.readings_5m (device_id, bucket desc);

-- ----------------------------------------------------------------------------
-- Vista pública de equipos
-- ----------------------------------------------------------------------------
-- La tabla `devices` contiene token_hash y jamás se expone. El dashboard lee
-- esta vista, que omite toda columna sensible.
create or replace view public.devices_publico
with (security_invoker = true) as
select
  d.id,
  d.slug,
  d.nombre,
  d.ubicacion,
  d.fw_version,
  d.last_seen_at,
  d.rssi,
  d.uptime_s,
  d.free_heap,
  d.reconnects,
  d.activo,
  -- Estado derivado en el servidor para que todos los clientes coincidan
  case
    when d.last_seen_at is null                         then 'nunca_visto'
    when d.last_seen_at > now() - interval '3 minutes'  then 'online'
    when d.last_seen_at > now() - interval '30 minutes' then 'intermitente'
    else 'offline'
  end as estado_conexion
from public.devices d
where d.activo;

comment on view public.devices_publico is
  'Proyección segura de devices para el dashboard anónimo. Excluye token_hash.';

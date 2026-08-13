-- ============================================================================
-- 0002 · Row Level Security y autorización de Realtime
-- ============================================================================
-- Modelo de seguridad (ver docs/ARQUITECTURA.md §7):
--
--   anon          -> SOLO LECTURA de datos de telemetría. Es el rol de
--                    cualquier visitante del dashboard. Su clave (anon key) es
--                    pública por diseño; la seguridad NO depende de ocultarla,
--                    depende de estas políticas.
--   device_*      -> JWT firmado, exclusivo del ESP32. Solo puede publicar en
--                    el canal Realtime de SU equipo. No puede escribir en
--                    ninguna tabla.
--   service_role  -> Solo dentro de las Edge Functions. Nunca sale del
--                    servidor. Omite RLS.
--
-- Regla operativa: si una tabla tiene RLS activo y NINGUNA política para una
-- operación, esa operación queda denegada. Por eso abajo solo se declaran
-- políticas de SELECT: insert/update/delete quedan cerrados por omisión.
-- ============================================================================

-- ----------------------------------------------------------------------------
-- Activar RLS en todo
-- ----------------------------------------------------------------------------
alter table public.devices         enable row level security;
alter table public.sensors         enable row level security;
alter table public.readings        enable row level security;
alter table public.latest_readings enable row level security;
alter table public.thresholds      enable row level security;
alter table public.alerts          enable row level security;
alter table public.readings_5m     enable row level security;

-- `devices` queda SIN políticas: totalmente inaccesible para anon.
-- El dashboard usa la vista devices_publico, que no expone token_hash.

-- ----------------------------------------------------------------------------
-- Privilegios de tabla
-- ----------------------------------------------------------------------------
-- Una política RLS `to anon` NO sirve de nada si el rol no tiene además el
-- privilegio GRANT sobre la tabla: los privilegios se evalúan ANTES que RLS.
--
-- Supabase concede por defecto acceso a `anon` sobre el esquema public, así que
-- omitir estos GRANT "funciona" en un proyecto nuevo. Pero eso hace que el
-- modelo de seguridad dependa de una configuración ambiental invisible en el
-- código: aplicado a un Postgres limpio, o a un proyecto con los permisos por
-- defecto endurecidos, el dashboard no leería absolutamente nada.
--
-- Se declaran de forma explícita para que las migraciones sean autosuficientes
-- y el permiso concedido quede legible junto a la política que lo acota.
grant usage on schema public to anon, authenticated;

grant select on public.sensors         to anon, authenticated;
grant select on public.readings        to anon, authenticated;
grant select on public.latest_readings to anon, authenticated;
grant select on public.thresholds      to anon, authenticated;
grant select on public.alerts          to anon, authenticated;
grant select on public.readings_5m     to anon, authenticated;

-- Ningún INSERT/UPDATE/DELETE en ninguna parte: la única escritura que puede
-- originar un visitante es a través de funciones acotadas (reconocer_alerta) o
-- de una Edge Function con service_role.

-- ----------------------------------------------------------------------------
-- Lectura pública de telemetría
-- ----------------------------------------------------------------------------
create policy "lectura publica de sensores"
  on public.sensors for select to anon, authenticated using (true);

create policy "lectura publica de lecturas"
  on public.readings for select to anon, authenticated using (true);

create policy "lectura publica de ultima lectura"
  on public.latest_readings for select to anon, authenticated using (true);

create policy "lectura publica de umbrales"
  on public.thresholds for select to anon, authenticated using (true);

create policy "lectura publica de alarmas"
  on public.alerts for select to anon, authenticated using (true);

create policy "lectura publica de agregados"
  on public.readings_5m for select to anon, authenticated using (true);

-- La vista devices_publico se declaró con security_invoker = true, por lo que
-- consultarla evalúa RLS con los permisos de quien la invoca. Como `devices`
-- no tiene políticas, hay que conceder acceso explícito a la vista:
grant select on public.devices_publico to anon, authenticated;
-- ...y, dado security_invoker, permitir la lectura subyacente SOLO a través de
-- las columnas que la vista proyecta. Se logra con una política restringida:
create policy "lectura de equipos via vista"
  on public.devices for select to anon, authenticated using (activo);
-- Nota: esto haría legible `devices` directamente. Para impedirlo se revocan
-- los privilegios de tabla, que se evalúan ANTES que RLS:
revoke all on public.devices from anon, authenticated;
grant select (id, slug, nombre, ubicacion, fw_version, last_seen_at,
              rssi, uptime_s, free_heap, reconnects, activo)
  on public.devices to anon, authenticated;
-- Resultado: `select token_hash from devices` -> error de permiso a nivel de
-- columna. La defensa es doble (privilegios + RLS), no una sola.

-- ============================================================================
-- Autorización de Realtime (Ruta A — tiempo real)
-- ============================================================================
-- PROBLEMA QUE RESUELVE ESTA SECCIÓN:
-- La anon key es pública. Si el ESP32 publicara en el canal Realtime usando esa
-- clave, cualquiera que abriera el dashboard tendría la credencial necesaria
-- para inyectar lecturas falsas en el canal. El dashboard mostraría 200 °C sin
-- que nada esté caliente, y no habría forma de distinguirlo del dato real.
--
-- SOLUCIÓN: canal PRIVADO con autorización por RLS sobre realtime.messages.
--   - Publicar  -> requiere un JWT de dispositivo con el claim `device_slug`
--                  que coincida exactamente con el topic. Ese JWT vive solo en
--                  la NVS del ESP32.
--   - Suscribir -> permitido a cualquiera (anon). Es un dashboard público.
-- ============================================================================

alter table realtime.messages enable row level security;

-- Cualquiera puede LEER el canal de telemetría (dashboard público)
create policy "suscripcion publica a telemetria"
  on realtime.messages for select to anon, authenticated
  using (
    realtime.messages.extension = 'broadcast'
    and realtime.topic() like 'telemetria:%'
  );

-- Solo el equipo dueño del topic puede PUBLICAR en él
create policy "solo el equipo publica en su canal"
  on realtime.messages for insert to authenticated
  with check (
    realtime.messages.extension = 'broadcast'
    and realtime.topic() = 'telemetria:' || coalesce(
          (select auth.jwt() ->> 'device_slug'), '<sin-claim>'
        )
  );

-- ----------------------------------------------------------------------------
-- Publicación Realtime sobre tablas (para alarmas)
-- ----------------------------------------------------------------------------
-- Las alarmas sí deben llegar al dashboard aunque nadie esté mirando el canal
-- de telemetría, así que se emiten como cambios de tabla.
alter publication supabase_realtime add table public.alerts;
alter publication supabase_realtime add table public.latest_readings;

-- `readings` NO se agrega a la publicación a propósito: emitiría un evento por
-- cada fila insertada (17 280/día) hacia todos los clientes conectados, lo que
-- agotaría la cuota de Realtime sin aportar nada. El tiempo real va por el
-- canal broadcast; el histórico se consulta por REST bajo demanda.

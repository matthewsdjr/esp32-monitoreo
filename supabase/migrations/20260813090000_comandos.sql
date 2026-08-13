-- ============================================================================
-- 0005 · Canal de comandos hacia el equipo
-- ============================================================================
-- Hasta aquí el flujo era de una sola dirección: el ESP32 publica, el dashboard
-- lee. La tara remota necesita el sentido contrario, y eso reabre la restricción
-- del §2 de la arquitectura: el equipo está detrás de NAT y no acepta
-- conexiones entrantes. No se le puede "llamar".
--
-- SOLUCIÓN: cola de comandos. El equipo ya hace un POST a /ingest cada 30 s;
-- la RESPUESTA de ese POST lleva los comandos pendientes. Cero conexiones
-- nuevas, cero invocaciones extra, cero puertos abiertos. El precio es la
-- latencia: hasta 30 s entre pulsar el botón y que el equipo lo ejecute, y por
-- eso la interfaz muestra el estado del comando en vez de fingir que fue
-- instantáneo.
-- ============================================================================

create table if not exists public.device_commands (
  id             bigserial primary key,
  device_id      uuid not null references public.devices(id) on delete cascade,

  comando        text not null check (comando in (
                   'tara',            -- poner la báscula en cero
                   'calibrar',        -- fijar factor con peso patrón conocido
                   'reiniciar',       -- reinicio controlado del equipo
                   'recargar_umbrales'-- releer thresholds desde la nube
                 )),
  parametros     jsonb,

  -- Quién lo pidió. No es autenticación: es trazabilidad. La autorización la
  -- hace el PIN de operador que valida la Edge Function /comando.
  solicitado_por text not null,
  solicitado_at  timestamptz not null default now(),

  -- Ciclo de vida
  entregado_at   timestamptz,   -- el equipo lo recibió en su respuesta de /ingest
  ejecutado_at   timestamptz,   -- el equipo reportó haberlo terminado
  resultado      jsonb,         -- p. ej. {"ok":true,"offset_anterior":8421,"offset_nuevo":8395}

  -- Un comando que nunca se entrega no debe quedar colgado para siempre:
  -- si el equipo estaba apagado, ejecutar una tara al encender tres días
  -- después sería peor que no hacer nada.
  expira_at      timestamptz not null default now() + interval '10 minutes',

  estado         text not null default 'pendiente'
                 check (estado in ('pendiente','entregado','ejecutado','fallido','expirado'))
);

create index if not exists device_commands_pendientes_idx
  on public.device_commands (device_id, solicitado_at)
  where estado = 'pendiente';

create index if not exists device_commands_recientes_idx
  on public.device_commands (device_id, solicitado_at desc);

-- Un solo comando pendiente por tipo y equipo: si el operador pulsa "tarar"
-- cinco veces porque no ve respuesta inmediata, se ejecuta UNA tara, no cinco.
create unique index if not exists device_commands_unico_pendiente_idx
  on public.device_commands (device_id, comando)
  where estado = 'pendiente';

-- ----------------------------------------------------------------------------
-- Seguridad
-- ----------------------------------------------------------------------------
alter table public.device_commands enable row level security;

-- Lectura pública: el operador necesita ver el estado del comando que pidió, y
-- que quede a la vista de todos quién taró y cuándo es bueno para la planta.
-- El GRANT es tan necesario como la política: los privilegios se evalúan ANTES
-- que RLS, y sin él la política no concede nada.
grant select on public.device_commands to anon, authenticated;

create policy "lectura publica de comandos"
  on public.device_commands for select to anon, authenticated using (true);

-- Sin políticas de escritura: encolar un comando solo es posible a través de la
-- Edge Function /comando, que corre con service_role y exige el PIN de operador.
--
-- POR QUÉ EL PIN: el dashboard es PÚBLICO. Sin esta barrera, cualquiera con el
-- enlace podría poner la báscula en cero a media producción. El PIN nunca viaja
-- dentro del bundle —el operador lo teclea—, y en la base solo vive su hash.

insert into public.app_config (clave, valor, descripcion) values
  ('operador_pin_hash', '', 'SHA-256 del PIN de operador. Vacío = comandos remotos DESACTIVADOS.')
on conflict (clave) do nothing;

-- Con el PIN vacío la Edge Function rechaza todo. Es el valor por defecto a
-- propósito: un despliegue recién hecho no debe aceptar comandos remotos hasta
-- que alguien decida explícitamente el PIN.

-- ----------------------------------------------------------------------------
-- Caducidad de comandos no entregados
-- ----------------------------------------------------------------------------
create or replace function public.fn_expirar_comandos()
returns integer
language plpgsql security definer set search_path = public as $$
declare n integer;
begin
  update public.device_commands
     set estado = 'expirado'
   where estado = 'pendiente'
     and expira_at < now();
  get diagnostics n = row_count;
  return n;
end;
$$;

select cron.schedule('expirar-comandos', '* * * * *', $$select public.fn_expirar_comandos()$$);

-- ----------------------------------------------------------------------------
-- Vista de últimos comandos por equipo
-- ----------------------------------------------------------------------------
create or replace view public.comandos_recientes
with (security_invoker = true) as
select
  c.id, c.device_id, c.comando, c.parametros, c.solicitado_por,
  c.solicitado_at, c.entregado_at, c.ejecutado_at, c.resultado, c.estado,
  d.slug as device_slug
from public.device_commands c
join public.devices d on d.id = c.device_id
where c.solicitado_at > now() - interval '24 hours'
order by c.solicitado_at desc;

grant select on public.comandos_recientes to anon, authenticated;

alter publication supabase_realtime add table public.device_commands;

-- ============================================================================
-- Stubs para validar las migraciones en un Postgres limpio
-- ============================================================================
-- Supabase provee roles, esquemas y extensiones que no existen en la imagen
-- oficial de Postgres. Este archivo los emula con lo mínimo indispensable para
-- que las migraciones se apliquen y las pruebas funcionales corran en local.
--
-- NO se despliega a producción. Solo lo usa tools/scripts/test-db.sh.
-- ============================================================================

-- Roles de Supabase
do $$
begin
  if not exists (select 1 from pg_roles where rolname = 'anon') then
    create role anon nologin noinherit;
  end if;
  if not exists (select 1 from pg_roles where rolname = 'authenticated') then
    create role authenticated nologin noinherit;
  end if;
  if not exists (select 1 from pg_roles where rolname = 'service_role') then
    create role service_role nologin noinherit bypassrls;
  end if;
end $$;

grant usage on schema public to anon, authenticated, service_role;

-- ----------------------------------------------------------------------------
-- Esquema auth
-- ----------------------------------------------------------------------------
create schema if not exists auth;

-- Devuelve los claims del JWT actual. En las pruebas se inyecta con
--   set local request.jwt.claims = '{"device_slug":"planta-01"}';
create or replace function auth.jwt() returns jsonb
language sql stable as $$
  select coalesce(
    nullif(current_setting('request.jwt.claims', true), '')::jsonb,
    '{}'::jsonb
  );
$$;
grant usage on schema auth to anon, authenticated, service_role;

-- ----------------------------------------------------------------------------
-- Esquema realtime
-- ----------------------------------------------------------------------------
create schema if not exists realtime;

create table if not exists realtime.messages (
  id         bigserial primary key,
  topic      text not null,
  extension  text not null,
  payload    jsonb,
  inserted_at timestamptz not null default now()
);

-- En Supabase, realtime.topic() lee el topic del canal en curso desde la
-- configuración de la sesión. El stub replica esa mecánica.
create or replace function realtime.topic() returns text
language sql stable as $$
  select nullif(current_setting('realtime.topic', true), '');
$$;

grant usage on schema realtime to anon, authenticated, service_role;

-- En Supabase esta tabla ya viene con RLS activo y pertenece a
-- supabase_realtime_admin, así que la migración NO lo activa. El stub lo hace
-- para que las pruebas evalúen las políticas igual que en producción.
alter table realtime.messages enable row level security;
grant select, insert on realtime.messages to anon, authenticated;

-- ----------------------------------------------------------------------------
-- Esquema net (stub de pg_net)
-- ----------------------------------------------------------------------------
create schema if not exists net;

create table if not exists net._peticiones_capturadas (
  id      bigserial primary key,
  url     text,
  headers jsonb,
  body    jsonb,
  at      timestamptz default now()
);

-- Captura la llamada en vez de hacerla. Permite comprobar en las pruebas que la
-- notificación SE HABRÍA enviado, sin depender de la red.
create or replace function net.http_post(
  url text,
  body jsonb default '{}'::jsonb,
  params jsonb default '{}'::jsonb,
  headers jsonb default '{}'::jsonb,
  timeout_milliseconds int default 5000
) returns bigint
language plpgsql as $$
declare nid bigint;
begin
  insert into net._peticiones_capturadas (url, headers, body)
  values (url, headers, body) returning id into nid;
  return nid;
end;
$$;

-- ----------------------------------------------------------------------------
-- Esquema cron (stub de pg_cron)
-- ----------------------------------------------------------------------------
create schema if not exists cron;

create table if not exists cron.job (
  jobid    bigserial primary key,
  jobname  text unique,
  schedule text,
  command  text
);

create or replace function cron.schedule(job_name text, schedule text, command text)
returns bigint
language plpgsql as $$
declare jid bigint;
begin
  insert into cron.job (jobname, schedule, command)
  values (job_name, schedule, command)
  on conflict (jobname) do update
    set schedule = excluded.schedule, command = excluded.command
  returning jobid into jid;
  return jid;
end;
$$;

-- ----------------------------------------------------------------------------
-- Publicación de Realtime
-- ----------------------------------------------------------------------------
do $$
begin
  if not exists (select 1 from pg_publication where pubname = 'supabase_realtime') then
    create publication supabase_realtime;
  end if;
end $$;

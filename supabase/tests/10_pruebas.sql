-- ============================================================================
-- Pruebas funcionales del backend
-- ============================================================================
-- Cada bloque falla ruidosamente (raise exception) si el comportamiento no es
-- el esperado. El script completo corre con ON_ERROR_STOP=1.
-- ============================================================================

\set ECHO none
\timing off

create schema if not exists pruebas;

create or replace function pruebas.afirmar(cond boolean, msg text)
returns void language plpgsql as $$
begin
  if cond is not true then
    raise exception 'FALLA: %', msg;
  end if;
  raise notice '  ok · %', msg;
end $$;

-- ============================================================================
-- 1. Estructura
-- ============================================================================
\echo '── 1. Estructura'
do $$
begin
  perform pruebas.afirmar(
    (select count(*) from information_schema.tables
      where table_schema='public'
        and table_name in ('devices','sensors','readings','latest_readings',
                           'thresholds','alerts','readings_5m','app_config',
                           'device_commands')) = 9,
    'las 9 tablas existen');

  perform pruebas.afirmar(
    (select count(*) from public.sensors) = 5,
    'la semilla creó los 5 canales');

  perform pruebas.afirmar(
    (select count(distinct bit_falla) from public.sensors) = 5,
    'cada canal tiene un bit de falla distinto');

  perform pruebas.afirmar(
    (select count(*) from cron.job) = 4,
    'los 4 trabajos programados quedaron registrados');
end $$;

-- ============================================================================
-- 2. Seguridad: anon no puede escribir ni leer secretos
-- ============================================================================
\echo '── 2. Seguridad (RLS y privilegios de columna)'
do $$
declare fallo boolean;
begin
  -- token_hash debe ser inaccesible para anon a nivel de PRIVILEGIO de columna,
  -- que se evalúa antes que RLS.
  perform pruebas.afirmar(
    not has_column_privilege('anon', 'public.devices', 'token_hash', 'SELECT'),
    'anon NO puede leer devices.token_hash');

  perform pruebas.afirmar(
    has_column_privilege('anon', 'public.devices', 'slug', 'SELECT'),
    'anon SÍ puede leer devices.slug');

  perform pruebas.afirmar(
    not has_table_privilege('anon', 'public.app_config', 'SELECT'),
    'anon NO puede leer app_config (secretos de notificación)');

  -- RLS activo en todas las tablas de datos
  perform pruebas.afirmar(
    (select bool_and(relrowsecurity) from pg_class
      where relname in ('devices','sensors','readings','latest_readings',
                        'thresholds','alerts','readings_5m','app_config')
        and relnamespace = 'public'::regnamespace),
    'RLS activo en todas las tablas');

  -- Los privilegios se evalúan ANTES que RLS: una política sin GRANT no
  -- concede nada. Depender de los permisos por defecto de Supabase haría que
  -- las migraciones no fueran autosuficientes.
  perform pruebas.afirmar(
    (select bool_and(has_table_privilege('anon', t, 'SELECT'))
       from unnest(array['public.sensors','public.readings','public.latest_readings',
                         'public.thresholds','public.alerts','public.readings_5m']) t),
    'anon tiene GRANT SELECT explícito en las tablas de telemetría');

  perform pruebas.afirmar(
    not exists (
      select 1 from unnest(array['public.sensors','public.readings','public.latest_readings',
                                 'public.thresholds','public.alerts','public.readings_5m']) t
      where has_table_privilege('anon', t, 'INSERT')
         or has_table_privilege('anon', t, 'UPDATE')
         or has_table_privilege('anon', t, 'DELETE')),
    'anon NO tiene ningún privilegio de escritura en ninguna tabla');

  -- Ninguna política de escritura para anon en ninguna tabla
  perform pruebas.afirmar(
    not exists (
      select 1 from pg_policies
      where schemaname = 'public'
        and cmd in ('INSERT','UPDATE','DELETE')
        and 'anon' = any(roles)),
    'no existe ninguna política de escritura para anon');
end $$;

-- Intento real de escritura como anon: debe ser rechazado
\echo '── 2b. Intento real de escritura anónima'
do $$
declare ok boolean := false;
begin
  begin
    set local role anon;
    insert into public.readings (device_id, ts, peso_g)
    values ('00000000-0000-4000-8000-000000000001', now(), 999);
  exception when insufficient_privilege or others then
    ok := true;
  end;
  reset role;
  perform pruebas.afirmar(ok, 'anon NO puede insertar lecturas (rechazado)');
end $$;

-- ============================================================================
-- 3. Autorización de Realtime
-- ============================================================================
\echo '── 3. Autorización de Realtime'
do $$
declare permitido boolean;
begin
  -- Un equipo solo puede publicar en SU topic
  set local role authenticated;
  set local request.jwt.claims = '{"device_slug":"planta-01"}';
  set local realtime.topic = 'telemetria:planta-01';

  select count(*) = 1 into permitido
  from pg_policies
  where schemaname='realtime' and tablename='messages' and cmd='INSERT';
  reset role;
  perform pruebas.afirmar(permitido, 'existe exactamente una política de publicación');

  perform pruebas.afirmar(
    exists (select 1 from pg_policies
            where schemaname='realtime' and tablename='messages'
              and cmd='SELECT' and 'anon' = any(roles)),
    'anon puede suscribirse al canal de telemetría');

  perform pruebas.afirmar(
    not exists (select 1 from pg_policies
                where schemaname='realtime' and tablename='messages'
                  and cmd='INSERT' and 'anon' = any(roles)),
    'anon NO puede publicar en el canal (no puede falsificar lecturas)');
end $$;

-- ============================================================================
-- 4. Motor de alarmas
-- ============================================================================
\echo '── 4. Motor de alarmas'

-- Helper: inserta una serie de lecturas de tc1 espaciadas 5 s
create or replace function pruebas.serie_tc1(
  p_desde timestamptz, p_n int, p_valor double precision, p_faults smallint default 0
) returns void language plpgsql as $$
declare i int;
begin
  for i in 0 .. p_n - 1 loop
    insert into public.readings (device_id, ts, peso_g, temp_amb_c, hum_pct, tc1_c, tc2_c, faults)
    values ('00000000-0000-4000-8000-000000000001',
            p_desde + make_interval(secs => i * 5),
            1000, 22, 50, p_valor, 100, p_faults)
    on conflict (device_id, ts) do nothing;
  end loop;
end $$;

-- La semilla deja notify_url vacía a propósito (sin destino configurado, no se
-- notifica). Para ejercitar esa ruta hay que darle un destino.
update public.app_config set valor = 'https://ejemplo.invalid/notify' where clave = 'notify_url';
update public.app_config set valor = 'secreto-de-prueba'            where clave = 'notify_secret';

-- 4a. Un pico aislado NO debe disparar alarma (duración mínima = 30 s)
do $$
declare t0 timestamptz := timestamptz '2026-01-01 10:00:00+00';
begin
  delete from public.alerts;
  delete from public.readings;

  perform pruebas.serie_tc1(t0, 12, 100);          -- 60 s normales
  perform pruebas.serie_tc1(t0 + interval '60 s', 1, 260);  -- un solo pico

  perform pruebas.afirmar(
    (select count(*) from public.alerts) = 0,
    'un pico aislado de 5 s NO dispara alarma (filtro de duración mínima)');
end $$;

-- 4b. Condición sostenida SÍ dispara
do $$
declare t0 timestamptz := timestamptz '2026-01-01 11:00:00+00';
begin
  delete from public.alerts;
  delete from public.readings;

  perform pruebas.serie_tc1(t0, 12, 100);                       -- normal
  perform pruebas.serie_tc1(t0 + interval '60 s', 10, 260);     -- 50 s sobre 250

  perform pruebas.afirmar(
    (select count(*) from public.alerts a
      join public.sensors s on s.id = a.sensor_id
     where s.slug='tc1' and a.nivel='alarm' and a.direccion='high'
       and a.cerrada_at is null) = 1,
    'una condición sostenida 50 s SÍ abre alarma');

  perform pruebas.afirmar(
    (select valor_pico from public.alerts order by id desc limit 1) = 260,
    'la alarma registra el valor pico');

  perform pruebas.afirmar(
    (select count(*) from net._peticiones_capturadas) >= 1,
    'se disparó la notificación al abrir la alarma');
end $$;

-- 4c. Histéresis: volver justo por debajo del umbral NO cierra la alarma
do $$
declare t1 timestamptz := timestamptz '2026-01-01 11:01:00+00';
begin
  -- umbral alarm_high = 250, histeresis = 3.0 -> cierra por debajo de 247
  perform pruebas.serie_tc1(t1 + interval '60 s', 8, 249);

  -- 249 °C está por debajo de alarm_high (250) pero dentro de la banda de
  -- histéresis (cierra por debajo de 247), así que la ALARMA sigue abierta.
  perform pruebas.afirmar(
    (select count(*) from public.alerts a
      join public.sensors s on s.id = a.sensor_id
     where s.slug='tc1' and a.nivel='alarm' and a.cerrada_at is null) = 1,
    'a 249 °C (dentro de la banda de histéresis) la ALARMA sigue abierta');

  -- Y como 249 > warn_high (200), se abre además una ADVERTENCIA. Los dos
  -- niveles son independientes: es lo que permite ver "sigue mal, pero menos".
  perform pruebas.afirmar(
    (select count(*) from public.alerts a
      join public.sensors s on s.id = a.sensor_id
     where s.slug='tc1' and a.nivel='warning' and a.cerrada_at is null) = 1,
    'a 249 °C se abre además la advertencia (niveles independientes)');
end $$;

-- 4d. Cruzar la histéresis SÍ cierra
do $$
declare t2 timestamptz := timestamptz '2026-01-01 11:03:00+00';
begin
  perform pruebas.serie_tc1(t2, 8, 240);

  -- 240 < 247 (250 - histéresis) -> la ALARMA cierra...
  perform pruebas.afirmar(
    (select count(*) from public.alerts a
      join public.sensors s on s.id = a.sensor_id
     where s.slug='tc1' and a.nivel='alarm' and a.cerrada_at is null) = 0,
    'a 240 °C (fuera de la banda) la ALARMA cierra');

  -- ...pero 240 sigue por encima de warn_high (200): la advertencia NO cierra.
  -- Este es el escenario "ya no es crítico, pero sigue fuera de especificación",
  -- que es exactamente lo que el operador necesita distinguir.
  perform pruebas.afirmar(
    (select count(*) from public.alerts a
      join public.sensors s on s.id = a.sensor_id
     where s.slug='tc1' and a.nivel='warning' and a.cerrada_at is null) = 1,
    'la advertencia sigue abierta a 240 °C (desescalada, no resuelta)');

  perform pruebas.afirmar(
    (select cerrada_at is not null from public.alerts
      where nivel='alarm' order by id desc limit 1),
    'la alarma cerrada conserva su marca de cierre');
end $$;

-- 4e. Falla de sensor (termopar abierto) genera alarma de tipo 'falla'
do $$
declare t3 timestamptz := timestamptz '2026-01-01 12:00:00+00';
begin
  delete from public.alerts;
  delete from public.readings;

  -- bit_falla de tc1 = 3  ->  máscara = 8
  insert into public.readings (device_id, ts, peso_g, temp_amb_c, hum_pct, tc1_c, tc2_c, faults)
  select '00000000-0000-4000-8000-000000000001',
         t3 + make_interval(secs => i*5), 1000, 22, 50, null, 100, 8
  from generate_series(0, 9) i;

  perform pruebas.afirmar(
    (select count(*) from public.alerts a
      join public.sensors s on s.id = a.sensor_id
     where s.slug='tc1' and a.direccion='falla' and a.cerrada_at is null) = 1,
    'un termopar desconectado sostenido abre alarma de falla');
end $$;

-- 4f. Reconocimiento de alarma
do $$
declare aid bigint;
begin
  select id into aid from public.alerts where cerrada_at is null limit 1;
  perform public.reconocer_alerta(aid, 'Supervisor Turno A');

  perform pruebas.afirmar(
    (select reconocida_por from public.alerts where id = aid) = 'Supervisor Turno A',
    'reconocer_alerta() estampa quién reconoció');

  -- No se puede reconocer dos veces
  begin
    perform public.reconocer_alerta(aid, 'Otro');
    perform pruebas.afirmar(false, 'reconocer dos veces debería fallar');
  exception when raise_exception then
    perform pruebas.afirmar(true, 'no se puede reconocer la misma alarma dos veces');
  end;
end $$;

-- ============================================================================
-- 5. Agregación y purga
-- ============================================================================
\echo '── 5. Agregación y purga'
do $$
declare n int;
begin
  delete from public.readings;
  delete from public.readings_5m;
  delete from public.alerts;

  -- 20 min de datos a 5 s = 240 muestras -> 4 cubos de 5 min
  insert into public.readings (device_id, ts, peso_g, temp_amb_c, hum_pct, tc1_c, tc2_c, faults)
  select '00000000-0000-4000-8000-000000000001',
         now() - make_interval(secs => i*5),
         1000 + i, 22, 50, 100, 100, 0
  from generate_series(0, 239) i;

  perform public.fn_rollup_5m(interval '30 minutes');

  select count(*) into n from public.readings_5m;
  perform pruebas.afirmar(n between 4 and 5, 'el rollup generó los cubos de 5 min (obtenidos: ' || n || ')');

  perform pruebas.afirmar(
    (select sum(n_muestras) from public.readings_5m) = 240,
    'el rollup contabilizó las 240 muestras');

  perform pruebas.afirmar(
    (select bool_and(peso_g_max >= peso_g_avg and peso_g_avg >= peso_g_min)
       from public.readings_5m),
    'min <= avg <= max en los agregados');
end $$;

-- 5b. La purga NO borra crudo sin agregado consolidado
do $$
declare crudas_antes bigint; crudas_despues bigint;
begin
  delete from public.readings;
  delete from public.readings_5m;

  -- Datos viejos SIN rollup previo
  insert into public.readings (device_id, ts, peso_g, temp_amb_c, hum_pct, tc1_c, tc2_c, faults)
  select '00000000-0000-4000-8000-000000000001',
         now() - interval '30 days' - make_interval(secs => i*5),
         1000, 22, 50, 100, 100, 0
  from generate_series(0, 59) i;

  select count(*) into crudas_antes from public.readings;

  -- fn_purgar() consolida primero y solo entonces borra: el dato viejo debe
  -- terminar agregado, no perdido.
  perform public.fn_purgar(interval '14 days', interval '24 months');

  select count(*) into crudas_despues from public.readings;

  perform pruebas.afirmar(crudas_antes = 60, 'se insertaron 60 lecturas antiguas');
  perform pruebas.afirmar(crudas_despues = 0, 'la purga eliminó el crudo antiguo');
  perform pruebas.afirmar(
    (select sum(n_muestras) from public.readings_5m) = 60,
    'las 60 muestras sobreviven en el agregado (no se perdió histórico)');
end $$;

-- ============================================================================
-- 6. Vista pública
-- ============================================================================
\echo '── 6. Vista pública de equipos'
do $$
begin
  perform pruebas.afirmar(
    not exists (
      select 1 from information_schema.columns
      where table_schema='public' and table_name='devices_publico'
        and column_name='token_hash'),
    'devices_publico NO expone token_hash');

  update public.devices set last_seen_at = now() where slug='planta-01';
  perform pruebas.afirmar(
    (select estado_conexion from public.devices_publico where slug='planta-01') = 'online',
    'equipo con heartbeat reciente aparece como online');

  update public.devices set last_seen_at = now() - interval '2 hours' where slug='planta-01';
  perform pruebas.afirmar(
    (select estado_conexion from public.devices_publico where slug='planta-01') = 'offline',
    'equipo sin heartbeat aparece como offline');
end $$;

-- ============================================================================
-- 7. Idempotencia de ingesta
-- ============================================================================
\echo '── 7. Idempotencia'
do $$
declare n1 bigint; n2 bigint; t timestamptz := timestamptz '2026-02-01 08:00:00+00';
begin
  delete from public.readings;
  delete from public.alerts;

  insert into public.readings (device_id, ts, peso_g) values
    ('00000000-0000-4000-8000-000000000001', t, 500);
  select count(*) into n1 from public.readings;

  -- Reintento del mismo lote tras un timeout ambiguo del ESP32
  insert into public.readings (device_id, ts, peso_g) values
    ('00000000-0000-4000-8000-000000000001', t, 500)
  on conflict (device_id, ts) do nothing;
  select count(*) into n2 from public.readings;

  perform pruebas.afirmar(n1 = 1 and n2 = 1,
    'reenviar el mismo lote no duplica lecturas');
end $$;

\echo ''


-- ============================================================================
-- 8. Canal de comandos
-- ============================================================================
\echo '── 8. Canal de comandos'
do $$
declare c1 bigint; c2 bigint; ok boolean := false;
begin
  delete from public.device_commands;

  perform pruebas.afirmar(
    (select valor from public.app_config where clave='operador_pin_hash') = '',
    'los comandos remotos nacen DESACTIVADOS (PIN vacío)');

  perform pruebas.afirmar(
    not has_table_privilege('anon', 'public.device_commands', 'INSERT'),
    'anon NO puede encolar comandos directamente');

  perform pruebas.afirmar(
    has_table_privilege('anon', 'public.device_commands', 'SELECT'),
    'anon SÍ puede consultar el estado de los comandos');

  -- Un solo pendiente por tipo: pulsar "tarar" cinco veces debe encolar UNA.
  insert into public.device_commands (device_id, comando, solicitado_por)
  values ('00000000-0000-4000-8000-000000000001', 'tara', 'Operador')
  returning id into c1;

  begin
    insert into public.device_commands (device_id, comando, solicitado_por)
    values ('00000000-0000-4000-8000-000000000001', 'tara', 'Operador')
    returning id into c2;
  exception when unique_violation then
    ok := true;
  end;
  perform pruebas.afirmar(ok, 'no se puede encolar una segunda tara mientras hay una pendiente');

  -- Una vez ejecutada, sí se puede pedir otra
  update public.device_commands set estado='ejecutado', ejecutado_at=now() where id=c1;
  insert into public.device_commands (device_id, comando, solicitado_por)
  values ('00000000-0000-4000-8000-000000000001', 'tara', 'Operador')
  returning id into c2;
  perform pruebas.afirmar(c2 is not null,
    'tras ejecutarse, se puede encolar una tara nueva');

  -- Caducidad
  update public.device_commands
     set expira_at = now() - interval '1 minute'
   where id = c2;
  perform public.fn_expirar_comandos();

  perform pruebas.afirmar(
    (select estado from public.device_commands where id=c2) = 'expirado',
    'un comando no entregado caduca (no se ejecuta una tara de hace 3 días)');

  perform pruebas.afirmar(
    (select count(*) from public.device_commands where estado='pendiente') = 0,
    'no quedan comandos pendientes colgados');
end $$;

-- Comando inválido rechazado por la restricción
do $$
declare ok boolean := false;
begin
  begin
    insert into public.device_commands (device_id, comando, solicitado_por)
    values ('00000000-0000-4000-8000-000000000001', 'formatear_todo', 'X');
  exception when check_violation then
    ok := true;
  end;
  perform pruebas.afirmar(ok, 'solo se aceptan comandos de la lista blanca');
end $$;

\echo ''
\echo '✓ Todas las pruebas pasaron'

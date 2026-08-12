-- ============================================================================
-- 0004 · Agregación, retención y trabajos programados
-- ============================================================================
-- Sin esto, dos cosas se rompen a los pocos meses:
--   1. La gráfica de "último mes" intenta descargar ~500 000 puntos al
--      navegador y el dashboard se vuelve inusable en celular.
--   2. La base supera los 500 MB del plan gratuito y deja de aceptar escrituras.
-- ============================================================================

create extension if not exists pg_cron;
create extension if not exists pg_net;

-- ----------------------------------------------------------------------------
-- Agregación a cubos de 5 minutos
-- ----------------------------------------------------------------------------
-- Se recalculan los últimos 15 min (3 cubos) en cada corrida, no solo el
-- último. Motivo: el drenado del búfer offline del ESP32 inserta lecturas con
-- marca de tiempo pasada; si solo se agregara el cubo actual, esos datos
-- recuperados nunca entrarían al histórico agregado.
create or replace function public.fn_rollup_5m(
  p_desde timestamptz,
  p_hasta timestamptz default null
)
returns integer
language plpgsql security definer set search_path = public as $$
declare n integer;
begin
  insert into public.readings_5m as r5 (
    device_id, bucket,
    peso_g_avg, peso_g_min, peso_g_max,
    temp_amb_c_avg, temp_amb_c_min, temp_amb_c_max,
    hum_pct_avg, hum_pct_min, hum_pct_max,
    tc1_c_avg, tc1_c_min, tc1_c_max,
    tc2_c_avg, tc2_c_min, tc2_c_max,
    n_muestras, n_fallas
  )
  select
    rd.device_id,
    date_bin('5 minutes', rd.ts, timestamptz '2000-01-01'),
    avg(rd.peso_g),     min(rd.peso_g),     max(rd.peso_g),
    avg(rd.temp_amb_c), min(rd.temp_amb_c), max(rd.temp_amb_c),
    avg(rd.hum_pct),    min(rd.hum_pct),    max(rd.hum_pct),
    avg(rd.tc1_c),      min(rd.tc1_c),      max(rd.tc1_c),
    avg(rd.tc2_c),      min(rd.tc2_c),      max(rd.tc2_c),
    count(*)::int,
    count(*) filter (where rd.faults <> 0)::int
  from public.readings rd
  where rd.ts >= p_desde
    and (p_hasta is null or rd.ts < p_hasta)
  group by 1, 2
  on conflict (device_id, bucket) do update set
    peso_g_avg = excluded.peso_g_avg,
    peso_g_min = excluded.peso_g_min,
    peso_g_max = excluded.peso_g_max,
    temp_amb_c_avg = excluded.temp_amb_c_avg,
    temp_amb_c_min = excluded.temp_amb_c_min,
    temp_amb_c_max = excluded.temp_amb_c_max,
    hum_pct_avg = excluded.hum_pct_avg,
    hum_pct_min = excluded.hum_pct_min,
    hum_pct_max = excluded.hum_pct_max,
    tc1_c_avg = excluded.tc1_c_avg,
    tc1_c_min = excluded.tc1_c_min,
    tc1_c_max = excluded.tc1_c_max,
    tc2_c_avg = excluded.tc2_c_avg,
    tc2_c_min = excluded.tc2_c_min,
    tc2_c_max = excluded.tc2_c_max,
    n_muestras = excluded.n_muestras,
    n_fallas   = excluded.n_fallas;

  get diagnostics n = row_count;
  return n;
end;
$$;

-- Atajo para el trabajo programado: consolida la ventana reciente.
create or replace function public.fn_rollup_5m(p_ventana interval default interval '15 minutes')
returns integer
language sql security definer set search_path = public as $$
  select public.fn_rollup_5m(now() - p_ventana, null::timestamptz);
$$;

-- ----------------------------------------------------------------------------
-- Purga
-- ----------------------------------------------------------------------------
-- IMPORTANTE: se purga sólo lo que YA fue agregado. Si el rollup estuviera
-- caído, esta función no borraría datos aún no consolidados: perder el crudo
-- sin tener el agregado sería una pérdida irreversible del histórico.
create or replace function public.fn_purgar(
  p_retencion_cruda    interval default interval '14 days',
  p_retencion_agregada interval default interval '24 months'
)
returns table (crudas_borradas bigint, agregadas_borradas bigint)
language plpgsql security definer set search_path = public as $$
declare
  v_corte    timestamptz := now() - p_retencion_cruda;
  v_crudas   bigint := 0;
  v_agregadas bigint := 0;
  v_min      timestamptz;
begin
  -- Consolidar desde la lectura MÁS ANTIGUA que exista, no desde una ventana
  -- fija relativa a now().
  --
  -- Por qué importa: si el rollup programado estuvo caído unos días, o si se
  -- acorta la retención, o si el ESP32 drena un búfer offline muy viejo, habrá
  -- crudo anterior a la ventana. Con una ventana fija ese crudo nunca se
  -- agregaría, y como la purga solo borra lo que ya está agregado, quedaría
  -- atrapado para siempre: creciendo sin límite hasta llenar la base.
  select min(ts) into v_min from public.readings;
  if v_min is not null then
    perform public.fn_rollup_5m(v_min, v_corte + interval '1 hour');
  end if;

  with borradas as (
    delete from public.readings rd
    where rd.ts < v_corte
      and exists (
        select 1 from public.readings_5m r5
        where r5.device_id = rd.device_id
          and r5.bucket = date_bin('5 minutes', rd.ts, timestamptz '2000-01-01')
      )
    returning 1
  )
  select count(*) into v_crudas from borradas;

  with borradas as (
    delete from public.readings_5m
    where bucket < now() - p_retencion_agregada
    returning 1
  )
  select count(*) into v_agregadas from borradas;

  -- Alarmas cerradas y reconocidas hace más de un año: ya cumplieron su función
  -- de trazabilidad. Las no reconocidas se conservan siempre.
  delete from public.alerts
  where cerrada_at is not null
    and cerrada_at < now() - interval '12 months'
    and reconocida_at is not null;

  return query select v_crudas, v_agregadas;
end;
$$;

-- ----------------------------------------------------------------------------
-- Cierre de alarmas huérfanas
-- ----------------------------------------------------------------------------
-- Si el ESP32 se apaga mientras una alarma está abierta, no llegarán más
-- lecturas y el trigger nunca la cerrará: quedaría "activa" para siempre en el
-- dashboard. Este trabajo la marca como cerrada y abre en su lugar una alarma
-- de equipo fuera de línea, que es el problema real que hay que atender.
create or replace function public.fn_cerrar_alarmas_huerfanas()
returns integer
language plpgsql security definer set search_path = public as $$
declare n integer;
begin
  update public.alerts a
     set cerrada_at = now()
    from public.devices d
   where a.device_id = d.id
     and a.cerrada_at is null
     and (d.last_seen_at is null or d.last_seen_at < now() - interval '15 minutes');
  get diagnostics n = row_count;
  return n;
end;
$$;

-- ----------------------------------------------------------------------------
-- Programación
-- ----------------------------------------------------------------------------
select cron.schedule('rollup-5m',        '*/5 * * * *', $$select public.fn_rollup_5m()$$);
select cron.schedule('purga-diaria',     '0 9 * * *',   $$select public.fn_purgar()$$);
select cron.schedule('alarmas-huerfanas','*/5 * * * *', $$select public.fn_cerrar_alarmas_huerfanas()$$);
-- 09:00 UTC ≈ 03:00 hora del centro de México, fuera del turno productivo.

-- ============================================================================
-- 0003 · Motor de alarmas
-- ============================================================================
-- Evaluación en el servidor: trazabilidad y notificación.
-- (El ESP32 evalúa además sus propios umbrales localmente para poder accionar
--  un buzzer o relé aunque se caiga internet. Ver ARQUITECTURA.md §9.)
--
-- Dos mecanismos que evitan el problema real de todo sistema de alarmas —que
-- el operador aprenda a ignorarlas—:
--
--   DURACIÓN MÍNIMA: la condición debe sostenerse N segundos. Un pico aislado
--   de ruido en un termopar no dispara nada.
--
--   HISTÉRESIS: la alarma no cierra hasta que el valor regresa un margen DENTRO
--   del umbral. Sin esto, una señal oscilando justo sobre el límite abre y
--   cierra eventos decenas de veces por minuto.
-- ============================================================================

-- ----------------------------------------------------------------------------
-- Configuración del servidor (nunca visible para anon)
-- ----------------------------------------------------------------------------
create table if not exists public.app_config (
  clave text primary key,
  valor text not null,
  descripcion text
);
alter table public.app_config enable row level security;
-- Sin políticas: solo service_role y funciones SECURITY DEFINER pueden leerla.

insert into public.app_config (clave, valor, descripcion) values
  ('notify_url',    '', 'URL de la Edge Function /notify'),
  ('notify_secret', '', 'Secreto compartido para autenticar al trigger ante /notify')
on conflict (clave) do nothing;

-- ----------------------------------------------------------------------------
-- Extracción del valor de un canal desde una fila ancha
-- ----------------------------------------------------------------------------
-- Permite escribir consultas genéricas por canal sin recurrir a SQL dinámico.
create or replace function public.valor_canal(r public.readings, canal text)
returns double precision
language sql immutable parallel safe as $$
  select case canal
    when 'peso'     then r.peso_g
    when 'temp_amb' then r.temp_amb_c
    when 'hum'      then r.hum_pct
    when 'tc1'      then r.tc1_c
    when 'tc2'      then r.tc2_c
    else null
  end;
$$;

-- ----------------------------------------------------------------------------
-- Disparo de notificación
-- ----------------------------------------------------------------------------
create or replace function public.fn_notificar_alerta(p_alert_id bigint)
returns void
language plpgsql security definer set search_path = public, extensions as $$
declare
  v_url    text;
  v_secret text;
begin
  select valor into v_url    from public.app_config where clave = 'notify_url';
  select valor into v_secret from public.app_config where clave = 'notify_secret';

  -- Sin configurar todavía: no es un error, simplemente no se notifica.
  if v_url is null or v_url = '' then
    return;
  end if;

  -- pg_net es asíncrono a propósito: la inserción de lecturas NUNCA debe
  -- quedarse esperando a que responda Telegram. Si la notificación falla, se
  -- pierde el aviso pero el dato queda guardado. Ese es el intercambio correcto.
  perform net.http_post(
    url     := v_url,
    headers := jsonb_build_object(
                 'Content-Type', 'application/json',
                 'X-Notify-Secret', v_secret
               ),
    body    := jsonb_build_object('alert_id', p_alert_id),
    timeout_milliseconds := 5000
  );

  update public.alerts set notificada_at = now() where id = p_alert_id;
exception when others then
  -- Una falla al notificar jamás debe abortar la transacción de ingesta.
  raise warning 'fn_notificar_alerta(%) falló: %', p_alert_id, sqlerrm;
end;
$$;

-- ----------------------------------------------------------------------------
-- Evaluación de umbrales
-- ----------------------------------------------------------------------------
create or replace function public.fn_evaluar_alarmas()
returns trigger
language plpgsql security definer set search_path = public, extensions as $$
declare
  -- %rowtype y no `record`: valor_canal() recibe un parámetro de tipo
  -- public.readings y un `record` genérico no es coercible a ese tipo compuesto.
  ultima     public.readings%rowtype;
  s          record;
  v          double precision;
  en_falla   boolean;
  nivel_obj  text;
  dir_obj    text;
  umbral_obj double precision;
  sostenida  boolean;
  abierta    record;
  nuevo_id   bigint;
  limite_cierre double precision;
begin
  -- Una sola evaluación por equipo y por lote: se usa la lectura más reciente.
  -- Evaluar fila por fila multiplicaría el trabajo sin cambiar el resultado,
  -- porque la comprobación de "sostenida" ya mira la ventana completa.
  for ultima in
    select distinct on (n.device_id) n.*
    from nuevas n
    order by n.device_id, n.ts desc
  loop
    for s in
      select sn.id as sensor_id, sn.slug, sn.etiqueta, sn.bit_falla, sn.device_id,
             t.warn_low, t.warn_high, t.alarm_low, t.alarm_high,
             t.histeresis, t.duracion_min_s
      from public.sensors sn
      join public.thresholds t on t.sensor_id = sn.id and t.activo
      where sn.device_id = ultima.device_id and sn.activo
    loop
      v        := public.valor_canal(ultima, s.slug);
      -- Casts explícitos: no existe operador `&` entre int2 e int4.
      en_falla := (ultima.faults::int & (1 << s.bit_falla::int)) <> 0;
      nuevo_id := null;

      -- ---------------------------------------------------------------
      -- 1. Determinar la condición más grave que aplica ahora mismo
      -- ---------------------------------------------------------------
      nivel_obj := null; dir_obj := null; umbral_obj := null;

      if en_falla or v is null then
        nivel_obj := 'alarm'; dir_obj := 'falla';
      elsif s.alarm_high is not null and v > s.alarm_high then
        nivel_obj := 'alarm';   dir_obj := 'high'; umbral_obj := s.alarm_high;
      elsif s.alarm_low  is not null and v < s.alarm_low  then
        nivel_obj := 'alarm';   dir_obj := 'low';  umbral_obj := s.alarm_low;
      elsif s.warn_high  is not null and v > s.warn_high  then
        nivel_obj := 'warning'; dir_obj := 'high'; umbral_obj := s.warn_high;
      elsif s.warn_low   is not null and v < s.warn_low   then
        nivel_obj := 'warning'; dir_obj := 'low';  umbral_obj := s.warn_low;
      end if;

      -- ---------------------------------------------------------------
      -- 2. Abrir alarma si la condición se sostuvo el tiempo mínimo
      -- ---------------------------------------------------------------
      if nivel_obj is not null then
        select not exists (
          select 1
          from public.readings rd
          where rd.device_id = ultima.device_id
            and rd.ts > ultima.ts - make_interval(secs => s.duracion_min_s)
            and rd.ts <= ultima.ts
            and case
                  when dir_obj = 'falla' then (rd.faults::int & (1 << s.bit_falla::int)) = 0
                                              and public.valor_canal(rd, s.slug) is not null
                  when dir_obj = 'high'  then coalesce(public.valor_canal(rd, s.slug), umbral_obj) <= umbral_obj
                  else                        coalesce(public.valor_canal(rd, s.slug), umbral_obj) >= umbral_obj
                end
        ) into sostenida;
        -- La consulta busca un CONTRAEJEMPLO dentro de la ventana. Si no existe
        -- ninguna muestra que incumpla, la condición se sostuvo.

        if sostenida then
          insert into public.alerts
            (sensor_id, device_id, nivel, direccion, valor_disparo, valor_pico, umbral)
          values
            (s.sensor_id, ultima.device_id, nivel_obj, dir_obj, v, v, umbral_obj)
          on conflict (sensor_id, nivel) where cerrada_at is null
          do nothing
          returning id into nuevo_id;

          if nuevo_id is not null then
            perform public.fn_notificar_alerta(nuevo_id);
          else
            -- Ya estaba abierta: solo se actualiza el peor valor alcanzado.
            update public.alerts a
               set valor_pico = case
                     when dir_obj = 'high' then greatest(coalesce(a.valor_pico, v), v)
                     when dir_obj = 'low'  then least(coalesce(a.valor_pico, v), v)
                     else a.valor_pico end
             where a.sensor_id = s.sensor_id
               and a.nivel = nivel_obj
               and a.cerrada_at is null;
          end if;
        end if;
      end if;

      -- ---------------------------------------------------------------
      -- 3. Cerrar alarmas cuya condición ya no aplica (con histéresis)
      -- ---------------------------------------------------------------
      for abierta in
        select * from public.alerts
        where sensor_id = s.sensor_id and cerrada_at is null
      loop
        if abierta.direccion = 'falla' then
          if not en_falla and v is not null then
            update public.alerts set cerrada_at = ultima.ts where id = abierta.id;
          end if;
        elsif v is not null then
          -- El valor debe regresar `histeresis` por DENTRO del umbral, no solo
          -- rozarlo. Ahí está la diferencia entre un evento y cincuenta.
          limite_cierre := case
            when abierta.direccion = 'high' then abierta.umbral - s.histeresis
            else                                 abierta.umbral + s.histeresis
          end;

          if (abierta.direccion = 'high' and v < limite_cierre)
          or (abierta.direccion = 'low'  and v > limite_cierre) then
            update public.alerts set cerrada_at = ultima.ts where id = abierta.id;
          end if;
        end if;
      end loop;

    end loop;
  end loop;

  return null;
end;
$$;

-- Trigger a nivel de SENTENCIA con tabla de transición: se ejecuta una vez por
-- lote (~6 filas), no una vez por fila.
drop trigger if exists trg_evaluar_alarmas on public.readings;
create trigger trg_evaluar_alarmas
  after insert on public.readings
  referencing new table as nuevas
  for each statement
  execute function public.fn_evaluar_alarmas();

-- ----------------------------------------------------------------------------
-- Reconocimiento de alarmas desde el dashboard
-- ----------------------------------------------------------------------------
-- Escritura acotada: anon NO puede tocar la tabla `alerts` (RLS lo impide),
-- pero sí puede invocar esta función, que solo permite estampar quién reconoció
-- y cuándo. No puede cerrar, borrar ni alterar valores.
create or replace function public.reconocer_alerta(p_alert_id bigint, p_por text)
returns public.alerts
language plpgsql security definer set search_path = public as $$
declare res public.alerts;
begin
  if p_por is null or length(trim(p_por)) < 2 or length(p_por) > 60 then
    raise exception 'Nombre de quien reconoce inválido';
  end if;

  update public.alerts
     set reconocida_por = trim(p_por),
         reconocida_at  = now()
   where id = p_alert_id
     and reconocida_at is null
  returning * into res;

  if res.id is null then
    raise exception 'La alarma no existe o ya fue reconocida';
  end if;
  return res;
end;
$$;

revoke all on function public.reconocer_alerta(bigint, text) from public;
grant execute on function public.reconocer_alerta(bigint, text) to anon, authenticated;

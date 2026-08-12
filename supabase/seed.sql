-- ============================================================================
-- Semilla: equipo de ejemplo, definición de canales y umbrales iniciales
-- ============================================================================
-- El token_hash de aquí es un valor de relleno SIN token real asociado, para
-- que el equipo semilla no pueda escribir. El alta real se hace con:
--     node tools/scripts/registrar-equipo.mjs
-- que genera 32 bytes aleatorios, guarda su hash y muestra el token UNA sola vez.
-- ============================================================================

insert into public.devices (id, slug, nombre, ubicacion, token_hash, fw_version)
values (
  '00000000-0000-4000-8000-000000000001',
  'planta-01',
  'Línea de producción 1',
  'Planta principal',
  'SIN_TOKEN_ASIGNADO',
  '0.0.0'
)
on conflict (slug) do nothing;

-- ----------------------------------------------------------------------------
-- Canales
-- ----------------------------------------------------------------------------
-- `bit_falla` fija la posición de cada canal en la máscara readings.faults.
-- Este orden es CONTRATO: el firmware lo replica en config.h. Cambiarlo aquí
-- sin cambiarlo allá hace que las fallas se atribuyan al sensor equivocado.
insert into public.sensors
  (device_id, slug, etiqueta, tipo, unidad, min_fisico, max_fisico, bit_falla, decimales, orden)
values
  ('00000000-0000-4000-8000-000000000001', 'peso',     'Báscula (celda de carga)', 'masa',        'g',   -100,   50000, 0, 1, 1),
  ('00000000-0000-4000-8000-000000000001', 'temp_amb', 'Temperatura ambiente',     'temperatura', '°C',    -40,     125, 1, 1, 2),
  ('00000000-0000-4000-8000-000000000001', 'hum',      'Humedad relativa',         'humedad',     '%HR',     0,     100, 2, 1, 3),
  ('00000000-0000-4000-8000-000000000001', 'tc1',      'Termopar 1',               'temperatura', '°C',      0,    1024, 3, 1, 4),
  ('00000000-0000-4000-8000-000000000001', 'tc2',      'Termopar 2',               'temperatura', '°C',      0,    1024, 4, 1, 5)
on conflict (device_id, slug) do nothing;

-- Rangos físicos usados arriba, y por qué:
--   temp_amb  -40..125 °C  → rango de operación del SHT31 (hoja de datos)
--   hum         0..100 %HR → límite físico de la magnitud
--   tc1/tc2     0..1024 °C → rango del MAX6675; su resolución es 0.25 °C y
--                            satura en 1024 °C. Un valor fuera de ahí solo
--                            puede ser termopar abierto o error de bus SPI.
--   peso     -100..50000 g → ajustar al alcance real de la celda instalada.
--                            El negativo permite ver deriva de tara en vez de
--                            recortarla silenciosamente a cero.

-- ----------------------------------------------------------------------------
-- Umbrales iniciales
-- ----------------------------------------------------------------------------
-- VALORES PROVISIONALES. Se ajustan en la Fase 5 contra el proceso real.
insert into public.thresholds
  (sensor_id, warn_low, warn_high, alarm_low, alarm_high, histeresis, duracion_min_s)
select s.id, v.warn_low, v.warn_high, v.alarm_low, v.alarm_high, v.hist, v.dur
from public.sensors s
join (values
  ('temp_amb', 15.0,  28.0,  10.0,  32.0, 0.5,  60),
  ('hum',      30.0,  70.0,  25.0,  80.0, 2.0, 120),
  ('tc1',      null,  200.0, null,  250.0, 3.0,  30),
  ('tc2',      null,  200.0, null,  250.0, 3.0,  30)
) as v(slug, warn_low, warn_high, alarm_low, alarm_high, hist, dur)
  on v.slug = s.slug
where s.device_id = '00000000-0000-4000-8000-000000000001'
on conflict (sensor_id) do nothing;

-- La báscula queda deliberadamente SIN umbrales: cuál es un peso "malo" depende
-- por completo del producto en proceso, y una alarma mal calibrada es peor que
-- ninguna. Se configura en la Fase 5.

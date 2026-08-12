// ============================================================================
// Fuente de datos DEMO — simula el proceso completo en el navegador
// ============================================================================
// Se activa automáticamente cuando no hay VITE_SUPABASE_URL configurada.
// Permite ver y evaluar el dashboard sin backend ni hardware.
//
// El modelo no es ruido aleatorio: es un paseo con retorno a la media más
// eventos de proceso. Un Math.random() plano produce gráficas que no se parecen
// a un proceso real y deja pasar errores de escala, suavizado y ejes que solo se
// ven con datos verosímiles.
// ============================================================================

import type { FuenteDatos } from "./fuente";
import type {
  Alarma,
  DefinicionSensor,
  EstadoEnlace,
  EstadoEquipo,
  Lectura,
  RangoHistorico,
  Umbral,
} from "../tipos";
import { RANGOS } from "../tipos";

const SENSORES: DefinicionSensor[] = [
  { slug: "peso",     etiqueta: "Báscula (celda de carga)", unidad: "g",   decimales: 1, min_fisico: -100, max_fisico: 50000 },
  { slug: "temp_amb", etiqueta: "Temperatura ambiente",     unidad: "°C",  decimales: 1, min_fisico: -40,  max_fisico: 125 },
  { slug: "hum",      etiqueta: "Humedad relativa",         unidad: "%HR", decimales: 1, min_fisico: 0,    max_fisico: 100 },
  { slug: "tc1",      etiqueta: "Termopar 1",               unidad: "°C",  decimales: 1, min_fisico: 0,    max_fisico: 1024 },
  { slug: "tc2",      etiqueta: "Termopar 2",               unidad: "°C",  decimales: 1, min_fisico: 0,    max_fisico: 1024 },
];

const UMBRALES: Umbral[] = [
  { canal: "temp_amb", warn_low: 15, warn_high: 28,  alarm_low: 10, alarm_high: 32 },
  { canal: "hum",      warn_low: 30, warn_high: 70,  alarm_low: 25, alarm_high: 80 },
  { canal: "tc1",      warn_low: null, warn_high: 200, alarm_low: null, alarm_high: 250 },
  { canal: "tc2",      warn_low: null, warn_high: 200, alarm_low: null, alarm_high: 250 },
];

// ----------------------------------------------------------------------------
// Ruido determinista y continuo
// ----------------------------------------------------------------------------
// Determinista a propósito: el histórico debe ser idéntico entre re-renders y
// entre cambios de rango. Con Math.random() la gráfica cambiaría de forma cada
// vez que el usuario alterna 1 h / 24 h, lo que desconcierta e impide comparar
// dos capturas.
//
// CONTINUO también a propósito: un generador con semilla por intervalo produce
// un valor independiente en cada cubo, y al muestrear con un paso distinto al
// del cubo aparece aliasing — saltos verticales que en una gráfica de proceso se
// leen como fallas del sensor que no existen. La suma de senos con frecuencias
// inconmensurables es suave a cualquier resolución de muestreo.
function ruido(tMs: number, frecuencia: number, amplitud: number): number {
  const t = tMs / 60_000;
  return (
    amplitud *
    (Math.sin(t * frecuencia) +
      0.5 * Math.sin(t * frecuencia * 2.7183) +
      0.28 * Math.sin(t * frecuencia * 6.1803)) /
    1.78
  );
}

/**
 * Estado del proceso en un instante dado, calculado de forma cerrada a partir
 * del tiempo. Al no depender del punto anterior, cualquier rango se genera
 * directamente sin recorrer todo el histórico.
 */
function procesoEn(tMs: number): Lectura {
  const h = tMs / 3_600_000; // horas desde época
  const min = tMs / 60_000;

  // Ambiente: ciclo diario suave + deriva lenta
  const temp_amb = 22.5 + Math.sin((h / 24) * 2 * Math.PI) * 2.2 + ruido(tMs, 0.09, 0.35);
  const hum = 52 - Math.sin((h / 24) * 2 * Math.PI) * 6 + ruido(tMs, 0.07, 1.3);

  // Termopares: ciclo de proceso de ~40 min con meseta
  const fase = (min % 40) / 40;
  const rampa = fase < 0.25 ? fase / 0.25
    : fase < 0.7 ? 1
    : fase < 0.85 ? 1 - (fase - 0.7) / 0.15
    : 0;
  const tc1 = 120 + rampa * 95 + Math.sin(min / 3) * 6 + ruido(tMs, 0.55, 2.4);
  const tc2 = 118 + rampa * 88 + Math.sin(min / 3.4) * 5 + ruido(tMs, 0.61, 2.2);

  // Báscula: carga por lotes. Se llena, se mantiene, se descarga.
  const cicloLote = (min % 25) / 25;
  const peso = cicloLote < 0.3
    ? 200 + (cicloLote / 0.3) * 1100
    : cicloLote < 0.8
      ? 1300 + Math.sin(min / 5) * 8
      : 1300 - ((cicloLote - 0.8) / 0.2) * 1100;

  return {
    ts: new Date(tMs).toISOString(),
    peso_g: +(peso + ruido(tMs, 1.3, 3.5)).toFixed(1),
    temp_amb_c: +temp_amb.toFixed(2),
    hum_pct: +hum.toFixed(1),
    tc1_c: +tc1.toFixed(2),
    tc2_c: +tc2.toFixed(2),
    faults: 0,
  };
}

// ----------------------------------------------------------------------------
export class FuenteDemo implements FuenteDatos {
  readonly esDemo = true;
  private arranque = Date.now();

  suscribir(
    alRecibirLectura: (l: Lectura) => void,
    alCambiarEnlace: (e: EstadoEnlace) => void,
  ): () => void {
    alCambiarEnlace("demo");
    alRecibirLectura(procesoEn(Date.now()));

    // 2 s: la misma cadencia que tendrá el ESP32 real.
    const id = setInterval(() => alRecibirLectura(procesoEn(Date.now())), 2000);
    return () => clearInterval(id);
  }

  async cargarEquipo(): Promise<EstadoEquipo> {
    return {
      slug: "planta-01",
      nombre: "Línea de producción 1",
      ubicacion: "Planta principal",
      fw_version: "demo-1.0.0",
      last_seen_at: new Date().toISOString(),
      rssi: -58,
      uptime_s: Math.round((Date.now() - this.arranque) / 1000) + 86_400,
      free_heap: 186_320,
      reconnects: 2,
      estado_conexion: "online",
    };
  }

  async cargarSensores() { return SENSORES; }
  async cargarUmbrales() { return UMBRALES; }

  async cargarHistorico(rango: RangoHistorico): Promise<Lectura[]> {
    const ventana = RANGOS[rango].ms;
    const fin = Date.now();

    // ~360 puntos por gráfica: suficiente resolución para una pantalla ancha,
    // y muy por debajo de lo que degrada el render en un celular.
    const paso = Math.max(5000, Math.round(ventana / 360));
    const puntos: Lectura[] = [];
    for (let t = fin - ventana; t <= fin; t += paso) puntos.push(procesoEn(t));
    return puntos;
  }

  async cargarAlarmas(): Promise<Alarma[]> {
    const ahora = Date.now();
    return [
      {
        id: 1,
        canal: "tc1",
        etiqueta: "Termopar 1",
        nivel: "warning",
        direccion: "high",
        valor_disparo: 214.3,
        valor_pico: 218.7,
        umbral: 200,
        abierta_at: new Date(ahora - 11 * 60_000).toISOString(),
        cerrada_at: null,
        reconocida_por: null,
      },
      {
        id: 2,
        canal: "hum",
        etiqueta: "Humedad relativa",
        nivel: "warning",
        direccion: "high",
        valor_disparo: 71.4,
        valor_pico: 73.9,
        umbral: 70,
        abierta_at: new Date(ahora - 5 * 3_600_000).toISOString(),
        cerrada_at: new Date(ahora - 4.2 * 3_600_000).toISOString(),
        reconocida_por: "Supervisor Turno A",
      },
      {
        id: 3,
        canal: "tc2",
        etiqueta: "Termopar 2",
        nivel: "alarm",
        direccion: "falla",
        valor_disparo: null,
        valor_pico: null,
        umbral: null,
        abierta_at: new Date(ahora - 26 * 3_600_000).toISOString(),
        cerrada_at: new Date(ahora - 25.5 * 3_600_000).toISOString(),
        reconocida_por: "Mantenimiento",
      },
    ];
  }

  async reconocerAlarma(): Promise<void> {
    // En demo no hay nada que persistir; el componente actualiza su estado local.
  }
}

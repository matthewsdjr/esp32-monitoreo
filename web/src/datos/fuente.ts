// ============================================================================
// Capa de datos — interfaz común
// ============================================================================
// El dashboard nunca habla con Supabase directamente. Depende de esta interfaz,
// que tiene dos implementaciones:
//
//   demo.ts      simula el proceso completo dentro del navegador
//   supabase.ts  el backend real
//
// Gracias a esto la Fase 4 se desarrolla y se demuestra sin ESP32 ni proyecto de
// Supabase, y cuando el hardware entra en línea NINGÚN componente cambia: solo
// se completan las variables de entorno.
// ============================================================================

import type {
  Alarma,
  DefinicionSensor,
  EstadoEnlace,
  EstadoEquipo,
  Lectura,
  RangoHistorico,
  Umbral,
} from "../tipos";

export interface FuenteDatos {
  readonly esDemo: boolean;

  /** Se invoca con cada lectura en vivo y con cada cambio del estado del enlace. */
  suscribir(
    alRecibirLectura: (l: Lectura) => void,
    alCambiarEnlace: (e: EstadoEnlace) => void,
  ): () => void;

  cargarEquipo(): Promise<EstadoEquipo>;
  cargarSensores(): Promise<DefinicionSensor[]>;
  cargarUmbrales(): Promise<Umbral[]>;
  cargarHistorico(rango: RangoHistorico): Promise<Lectura[]>;
  cargarAlarmas(): Promise<Alarma[]>;
  reconocerAlarma(id: number, por: string): Promise<void>;
}

import type { FuenteDatos } from "./fuente";
import { FuenteDemo } from "./demo";

/**
 * Selecciona la fuente de datos.
 *
 * Sin VITE_SUPABASE_URL configurada, el dashboard arranca en modo demo en vez
 * de mostrar una pantalla de error. Eso permite `npm run dev` en un clon limpio
 * y ver el sistema funcionando de inmediato — que es como se evalúa el diseño y
 * como se desarrolla mientras el hardware no existe.
 *
 * La carga de Supabase es dinámica para que su cliente no entre en el bundle
 * cuando no se usa.
 */
export async function crearFuente(): Promise<FuenteDatos> {
  const { hayBackendConfigurado, FuenteSupabase } = await import("./supabase");
  if (!hayBackendConfigurado) return new FuenteDemo();

  try {
    const fuente = new FuenteSupabase();
    await fuente.cargarEquipo(); // verificación temprana de conectividad
    return fuente;
  } catch (e) {
    console.error("Backend configurado pero inalcanzable; se usa modo demo.", e);
    return new FuenteDemo();
  }
}

export type { FuenteDatos };

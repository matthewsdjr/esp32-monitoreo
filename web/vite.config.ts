import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// ============================================================================
// BASE_PATH es la causa número uno de "la página se ve en blanco en Pages".
//
// En una *project page* (usuario.github.io/mi-repo) los recursos se sirven bajo
// /mi-repo/, no bajo la raíz. Sin `base` correcto, index.html pide /assets/... ,
// GitHub devuelve 404, y la página carga vacía sin ningún error visible.
//
// El workflow de despliegue lo inyecta automáticamente a partir del nombre del
// repositorio, así que no hay que tocar nada aquí al cambiar de repo.
// ============================================================================
// El cliente de Supabase ya queda en su propio chunk por el import dinámico de
// src/datos/index.ts: no se descarga en modo demo. No hace falta configurarlo.
export default defineConfig({
  base: process.env.BASE_PATH || "/",
  plugins: [react()],
});

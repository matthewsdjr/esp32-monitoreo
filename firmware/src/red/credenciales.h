// ============================================================================
// Credenciales del equipo — NUNCA en el código
// ============================================================================
// El repositorio es PÚBLICO. Ni la contraseña WiFi, ni el token de ingesta, ni
// el JWT del canal en vivo pueden aparecer en un archivo versionado: un secreto
// en un commit sigue siendo público aunque se borre después, y recuperarlo exige
// reescribir historial y rotar la credencial.
//
// Todo se aprovisiona por portal cautivo al primer arranque y vive en NVS, en un
// espacio de nombres propio para que borrar la calibración no borre la red.
// ============================================================================

#pragma once
#include <Arduino.h>
#include <Preferences.h>

namespace red {

struct Credenciales {
  String urlSupabase;    // https://xxxx.supabase.co
  String anonKey;        // pública por diseño, pero se aprovisiona igual
  String slugEquipo;     // planta-01
  String tokenIngesta;   // Bearer para /ingest
  String jwtDispositivo; // autoriza a publicar en el canal en vivo

  bool completas() const {
    return urlSupabase.length() > 10 && slugEquipo.length() > 2 &&
           tokenIngesta.length() > 10;
  }

  /** El canal en vivo es opcional: sin JWT el histórico sigue funcionando. */
  bool puedeTiempoReal() const {
    return jwtDispositivo.length() > 20 && anonKey.length() > 20;
  }
};

class AlmacenCredenciales {
 public:
  void iniciar() { prefs_.begin("red", false); }

  Credenciales leer() {
    Credenciales c;
    c.urlSupabase    = prefs_.getString("url", "");
    c.anonKey        = prefs_.getString("anon", "");
    c.slugEquipo     = prefs_.getString("slug", "");
    c.tokenIngesta   = prefs_.getString("token", "");
    c.jwtDispositivo = prefs_.getString("jwt", "");

    // Una barra final duplicaría la barra al componer las rutas y produciría
    // 404 difíciles de diagnosticar desde un equipo sin consola a la vista.
    while (c.urlSupabase.endsWith("/")) {
      c.urlSupabase.remove(c.urlSupabase.length() - 1);
    }
    return c;
  }

  void guardar(const Credenciales& c) {
    prefs_.putString("url", c.urlSupabase);
    prefs_.putString("anon", c.anonKey);
    prefs_.putString("slug", c.slugEquipo);
    prefs_.putString("token", c.tokenIngesta);
    prefs_.putString("jwt", c.jwtDispositivo);
  }

  void borrar() { prefs_.clear(); }

 private:
  Preferences prefs_;
};

}  // namespace red

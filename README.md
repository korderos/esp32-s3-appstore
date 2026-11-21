# Appstore ESP32-S3-Touch-LCD-3.49 V2

Page web (`docs/`) qui installe une application sur le module par USB, depuis Chrome/Edge (Web Serial + [ESP Web Tools](https://esphome.github.io/esp-web-tools/)).

## Ajouter une application

1. Créer `src/apps/<id>/main.cpp` (avec `setup()`/`loop()`, `#include "../../display.h"`) et `src/apps/<id>/app.json` :
   ```json
   { "name": "Mon app", "version": "1.0.0", "category": "Jeux", "icon": "🎮",
     "color": "#ffb000", "summary": "Une ligne.", "description": "Plus de détails." }
   ```
2. Ajouter `[env:<id>]` dans `platformio.ini` (et l'id dans `default_envs`).
3. `python -m platformio run -e <id>` : les .bin, `manifest.json` et `docs/apps.json` sont mis à jour automatiquement.

## Tester en local

`python -m http.server 8765 --directory docs`, puis ouvrir http://localhost:8765 dans Chrome (Web Serial accepte localhost sans HTTPS).

## Publier sur GitHub Pages

Pousser le dépôt sur GitHub, puis Settings → Pages → « Deploy from a branch », branche `main`, dossier `/docs`.

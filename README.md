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

Pour une app connectée, ajouter `"wifi": true` dans `app.json` (l'appstore affiche alors l'aide Wi-Fi).

## Wi-Fi partagé

Les apps connectées lisent le réseau dans la NVS, espace `wifi`, via `src/apps/common/wifi_shared.h`. L'appstore n'efface pas la NVS : un réseau saisi une fois sert à toutes les apps.

- Saisie depuis l'appstore : à la fin de l'installation, ESP Web Tools propose « Se connecter au Wi-Fi » (protocole [Improv Wi-Fi](https://www.improv-wifi.com/serial/)). L'app doit appeler `improv_begin()` puis `improv_poll()` régulièrement (`improv_delay()` remplace `delay()` pendant les attentes).
- En secours, chaque app garde son portail captif (`<App>-Setup`), qui enregistre au même endroit.
- « Changer de réseau » dans une app oublie le Wi-Fi pour toutes.

## Captures d'écran

Chaque app répond à la ligne `SCREENSHOT` sur le port USB (`src/apps/common/screenshot.h`). Avec l'app installée sur le module :

```
python scripts/screenshot.py meteo            # ajoute docs/screenshots/meteo/N.png à la fiche
python scripts/screenshot.py meteo --reset    # remplace toutes les captures de l'app
python scripts/screenshot.py meteo --name 7j  # nom choisi (remplacé s'il existe)
```

Le script met à jour `screenshots` dans `src/apps/<id>/app.json` et `docs/apps.json` ; l'appstore les affiche dans le cadre du module (miniatures s'il y en a plusieurs). Une nouvelle app doit appeler `screenshot_begin()` et `screenshot_poll()` (ou `improv_set_line_hook(screenshot_handle_line)` si elle utilise Improv).

## Tester en local

`python -m http.server 8765 --directory docs`, puis ouvrir http://localhost:8765 dans Chrome (Web Serial accepte localhost sans HTTPS).

## Publier sur GitHub Pages

Pousser le dépôt sur GitHub, puis Settings → Pages → « Deploy from a branch », branche `main`, dossier `/docs`.

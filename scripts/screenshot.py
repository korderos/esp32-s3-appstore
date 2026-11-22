"""Capture l'écran du module par USB et l'ajoute aux captures de l'app dans l'appstore.

    python scripts/screenshot.py meteo                # ajoute docs/screenshots/meteo/N.png
    python scripts/screenshot.py meteo --name accueil # nom de fichier choisi (remplacé s'il existe)
    python scripts/screenshot.py meteo --reset        # efface les captures de l'app d'abord
    python scripts/screenshot.py meteo --port COM10 --no-store   # simple capture, sans toucher au catalogue

L'app installée sur le module doit être celle indiquée : elle répond à la ligne
"SCREENSHOT" (src/apps/common/screenshot.h). La capture est ajoutée à la liste
"screenshots" de src/apps/<id>/app.json et de docs/apps.json.
"""
import argparse
import json
import os
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial manquant : python -m pip install pyserial")
try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow manquant : python -m pip install pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs")


def default_port():
    with open(os.path.join(ROOT, "platformio.ini"), encoding="utf-8") as f:
        for line in f:
            if line.strip().startswith("upload_port"):
                return line.split("=", 1)[1].strip()
    return "COM10"


def capture(port, timeout=15):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False  # ne pas redémarrer la puce à l'ouverture du port
    s.open()
    try:
        s.reset_input_buffer()
        s.write(b"\nSCREENSHOT\n")
        buf, t0 = b"", time.time()
        while b"#SCREENSHOT" not in buf:
            if time.time() - t0 > timeout:
                sys.exit("Pas de réponse : l'app installée gère-t-elle les captures ? (%r)" % buf[-200:])
            buf += s.read(4096)
            if b"#SCREENSHOT-ERROR" in buf:
                sys.exit("Le module signale une erreur : " + buf.split(b"#SCREENSHOT-ERROR", 1)[1].split(b"\n")[0].decode())
        buf = buf[buf.index(b"#SCREENSHOT"):]
        while b"\n" not in buf:
            buf += s.read(64)
        header, data = buf.split(b"\n", 1)
        _, w, h, fmt = header.decode().split()
        w, h = int(w), int(h)
        need = w * h * 2
        s.timeout = 5
        while len(data) < need:
            chunk = s.read(need - len(data))
            if not chunk:
                raise IOError("capture incomplète (%d/%d octets)" % (len(data), need))
            data += chunk
        data = data[:need]
    finally:
        s.close()

    # RGB565 -> RGB888
    big = fmt == "RGB565BE"
    px = bytearray(w * h * 3)
    for i in range(w * h):
        v = (data[2 * i] << 8 | data[2 * i + 1]) if big else (data[2 * i + 1] << 8 | data[2 * i])
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        px[3 * i] = (r << 3) | (r >> 2)
        px[3 * i + 1] = (g << 2) | (g >> 4)
        px[3 * i + 2] = (b << 3) | (b >> 2)
    return Image.frombytes("RGB", (w, h), bytes(px))


def update_lists(app_id, rel, reset):
    meta_path = os.path.join(ROOT, "src", "apps", app_id, "app.json")
    with open(meta_path, encoding="utf-8") as f:
        meta = json.load(f)
    shots = [] if reset else list(meta.get("screenshots", []))
    if rel not in shots:
        shots.append(rel)
    meta["screenshots"] = shots
    with open(meta_path, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)
        f.write("\n")

    catalog_path = os.path.join(DOCS, "apps.json")
    if os.path.isfile(catalog_path):
        with open(catalog_path, encoding="utf-8") as f:
            catalog = json.load(f)
        for a in catalog.get("apps", []):
            if a.get("id") == app_id:
                a["screenshots"] = shots
        with open(catalog_path, "w", encoding="utf-8") as f:
            json.dump(catalog, f, ensure_ascii=False, indent=2)
    return shots


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("app", help="id de l'app (dossier src/apps/<id>)")
    ap.add_argument("--port", default=None, help="port série (défaut : upload_port de platformio.ini)")
    ap.add_argument("--name", default=None, help="nom du fichier, sans extension")
    ap.add_argument("--reset", action="store_true", help="remplace toutes les captures de l'app")
    ap.add_argument("--no-store", action="store_true", help="enregistre seulement le PNG")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(ROOT, "src", "apps", args.app, "app.json")):
        sys.exit("App inconnue : src/apps/%s/app.json introuvable" % args.app)

    # le lien USB perd parfois quelques octets sur 220 Ko : on recommence
    for attempt in range(3):
        try:
            img = capture(args.port or default_port())
            break
        except IOError as e:
            print("Essai %d : %s" % (attempt + 1, e))
            time.sleep(1)
    else:
        sys.exit("Échec de la capture après 3 essais")
    out_dir = os.path.join(DOCS, "screenshots", args.app)
    os.makedirs(out_dir, exist_ok=True)
    if args.reset and not args.no_store:
        for f in os.listdir(out_dir):
            if f.endswith(".png"):
                os.remove(os.path.join(out_dir, f))
    name = args.name
    if not name:
        n = 1
        while os.path.exists(os.path.join(out_dir, "%d.png" % n)):
            n += 1
        name = str(n)
    path = os.path.join(out_dir, name + ".png")
    img.save(path, optimize=True)
    print("Capture %dx%d -> %s" % (img.width, img.height, os.path.relpath(path, ROOT)))

    if not args.no_store:
        shots = update_lists(args.app, "screenshots/%s/%s.png" % (args.app, name), args.reset)
        print("Captures de %s dans l'appstore : %s" % (args.app, ", ".join(shots)))


if __name__ == "__main__":
    main()

# Après chaque compilation, publie l'app dans l'appstore web (docs/) :
#   docs/firmware/<id>/bootloader.bin, partitions.bin, boot_app0.bin, firmware.bin
#   docs/firmware/<id>/manifest.json   (format ESP Web Tools)
#   docs/apps.json                     (catalogue lu par docs/index.html)
# Les métadonnées viennent de src/apps/<id>/app.json.
import hashlib
import json
import os
import shutil
from datetime import datetime, timezone

Import("env")

PROJECT_DIR = env.subst("$PROJECT_DIR")
DOCS_DIR = os.path.join(PROJECT_DIR, "docs")


def _offset(value):
    value = env.subst(str(value))
    return int(value, 0)


def export(source, target, env):
    app_id = env.subst("$PIOENV")
    meta_path = os.path.join(PROJECT_DIR, "src", "apps", app_id, "app.json")
    if not os.path.isfile(meta_path):
        print("appstore : %s introuvable, app non exportée" % meta_path)
        return
    with open(meta_path, encoding="utf-8") as f:
        meta = json.load(f)

    build_dir = env.subst("$BUILD_DIR")
    out_dir = os.path.join(DOCS_DIR, "firmware", app_id)
    os.makedirs(out_dir, exist_ok=True)

    # bootloader (0x0 sur S3), table de partitions (0x8000), boot_app0 (0xe000)
    images = [(_offset(off), env.subst(path)) for off, path in env.get("FLASH_EXTRA_IMAGES", [])]
    images.append((_offset(env.subst("$ESP32_APP_OFFSET") or "0x10000"),
                   os.path.join(build_dir, env.subst("${PROGNAME}.bin"))))

    parts = []
    total = 0
    digest = hashlib.sha256()
    for offset, path in sorted(images):
        name = os.path.basename(path)
        shutil.copyfile(path, os.path.join(out_dir, name))
        size = os.path.getsize(path)
        total += size
        with open(path, "rb") as f:
            digest.update(f.read())
        parts.append({"path": name, "offset": offset})

    version = meta.get("version", "0.0.0")
    manifest = {
        "name": meta.get("name", app_id),
        "version": version,
        "new_install_prompt_erase": False,
        "builds": [{"chipFamily": "ESP32-S3", "parts": parts}],
    }
    with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    catalog_path = os.path.join(DOCS_DIR, "apps.json")
    catalog = {"apps": []}
    if os.path.isfile(catalog_path):
        with open(catalog_path, encoding="utf-8") as f:
            catalog = json.load(f)
    entry = dict(meta)
    entry.update({
        "id": app_id,
        "manifest": "firmware/%s/manifest.json" % app_id,
        "size": total,
        "sha256": digest.hexdigest()[:12],
        "updated": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
    })
    catalog["apps"] = [a for a in catalog.get("apps", []) if a.get("id") != app_id] + [entry]
    catalog["apps"].sort(key=lambda a: a.get("name", a["id"]).lower())
    with open(catalog_path, "w", encoding="utf-8") as f:
        json.dump(catalog, f, ensure_ascii=False, indent=2)

    print("appstore : %s %s exportée (%d Ko) -> docs/firmware/%s/" % (
        meta.get("name", app_id), version, total // 1024, app_id))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", export)

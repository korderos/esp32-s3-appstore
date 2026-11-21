# Après l'upload, redémarre la puce par watchdog.
# Le reset RTS d'esptool 4.5.1 (fourni par PlatformIO) laisse l'ESP32-S3
# en mode téléchargement via l'USB natif ; esptool >= 4.6 sait faire un
# reset watchdog, on utilise donc celui installé avec Arduino.
import os
import subprocess

Import("env")

ESPTOOL5 = os.path.join(
    os.environ.get("LOCALAPPDATA", ""),
    "Arduino15", "packages", "esp32", "tools", "esptool_py", "5.1.0", "esptool.exe",
)


def watchdog_reset(source, target, env):
    if not os.path.isfile(ESPTOOL5):
        print("esptool 5.1 introuvable : appuyez sur RESET pour lancer le firmware")
        return
    # Ne sert qu'après un passage manuel en mode téléchargement (BOOT) :
    # si le firmware a déjà démarré, la connexion échoue et on l'ignore.
    port = env.subst("$UPLOAD_PORT")
    result = subprocess.run(
        [ESPTOOL5, "--chip", "esp32s3", "--port", port, "--connect-attempts", "1",
         "--before", "no-reset", "--after", "watchdog-reset", "read-mac"],
        capture_output=True, text=True,
    )
    if result.returncode == 0:
        print("Puce restée en mode téléchargement : reset watchdog envoyé")
    else:
        print("Firmware déjà démarré, pas de reset supplémentaire")


env.AddPostAction("upload", watchdog_reset)

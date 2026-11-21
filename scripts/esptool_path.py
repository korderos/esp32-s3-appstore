# pioarduino appelle la commande `esptool`, normalement fournie par son penv.
# PlatformIO tourne ici depuis le Python système (pas de ~/.platformio/penv) :
# on ajoute au PATH l'esptool.exe installé avec Arduino.
import os

Import("env")

ESPTOOL_DIR = os.path.join(
    os.environ.get("LOCALAPPDATA", ""),
    "Arduino15", "packages", "esp32", "tools", "esptool_py", "5.1.0",
)

if os.path.isfile(os.path.join(ESPTOOL_DIR, "esptool.exe")):
    env.PrependENVPath("PATH", ESPTOOL_DIR)
    os.environ["PATH"] = ESPTOOL_DIR + os.pathsep + os.environ.get("PATH", "")
else:
    print("esptool.exe introuvable dans %s : la compilation risque d'échouer" % ESPTOOL_DIR)

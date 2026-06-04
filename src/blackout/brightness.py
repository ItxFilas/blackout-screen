import subprocess

SERVICE = "org.kde.Solid.PowerManagement"
PATH = "/org/kde/Solid/PowerManagement/Actions/BrightnessControl"
IFACE = "org.kde.Solid.PowerManagement.Actions.BrightnessControl"


def _qdbus(*args):
    result = subprocess.run(
        ["qdbus", SERVICE, PATH, *args],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def get_brightness():
    return int(_qdbus(f"{IFACE}.brightness"))


def set_brightness(value):
    _qdbus(f"{IFACE}.setBrightnessSilent", str(int(value)))

import json
import os


def get_state_path():
    return os.environ.get(
        "BLACKOUT_STATE",
        os.path.expanduser("~/.cache/blackout/state.json"),
    )


def read_state():
    try:
        with open(get_state_path()) as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return None


def write_state(overlay_pid, saved_brightness):
    path = get_state_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(
            {"overlay_pid": overlay_pid, "saved_brightness": saved_brightness}, f
        )


def clear_state():
    try:
        os.remove(get_state_path())
    except FileNotFoundError:
        pass


def is_active():
    return read_state() is not None

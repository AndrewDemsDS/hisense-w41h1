"""What the tooling edge tests share: the check counters, the known-failure marker and the
helpers that patch a module, the environment and the output for the length of a block.

A KNOWN FAILURE is a check that is expected to fail today: `known_failure()` passes while the
defect is there and fails the day it is fixed, so the marker cannot outlive the defect.
"""

import contextlib
import importlib.util
import io
import os
import pathlib
import stat
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "firmware" / "scripts"
sys.path.insert(0, str(SCRIPTS))
import dev  # noqa: E402

ok = True
known = 0


def check(cond, msg):
    global ok
    print(("  ok  " if cond else "  FAIL ") + msg)
    ok = ok and bool(cond)


def known_failure(fixed, msg, defect):
    """`fixed` is the condition that holds once the defect is gone. While it is False this prints
    an xfail line and the suite stays green."""
    global ok, known
    if fixed:
        print(f"  FAIL {msg}: passes now, so the defect is fixed. Turn this into a plain check ({defect})")
        ok = False
    else:
        known += 1
        print(f"  xfail {msg}  [{defect}]")


def finish(name):
    """Print the closing line and return the exit code."""
    print(f"== {name} OK ({known} known failures) ==" if ok else f"== {name} FAILED ==")
    return 0 if ok else 1


def load_script(name, path):
    """Import a script whose file name is not a module name (cpp-lint.py)."""
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def died(fn, *args, **kwargs):
    """The message a tool died with (dev.Die), or None when it did not die."""
    try:
        fn(*args, **kwargs)
    except dev.Die as e:
        return str(e)
    return None


def raised(fn, *args, **kwargs):
    """The exception class name a call ends in, or None."""
    try:
        fn(*args, **kwargs)
    except BaseException as e:  # noqa: BLE001 - the point is to see which one
        return type(e).__name__
    return None


@contextlib.contextmanager
def patched(obj, **attrs):
    """Set attributes for the length of a block. An attribute that did not exist is removed."""
    missing = object()
    old = {k: getattr(obj, k, missing) for k in attrs}
    # Only what the object itself holds is restored by assignment: a name that came from a base
    # (or, for a module, from builtins) is deleted again.
    own = {k: (k in vars(obj)) for k in attrs}
    for k, v in attrs.items():
        setattr(obj, k, v)
    try:
        yield
    finally:
        for k, v in old.items():
            if own[k]:
                setattr(obj, k, v)
            else:
                delattr(obj, k)


@contextlib.contextmanager
def environ(**kv):
    """Set (or with None, remove) environment variables for the length of a block."""
    old = {k: os.environ.get(k) for k in kv}
    for k, v in kv.items():
        if v is None:
            os.environ.pop(k, None)
        else:
            os.environ[k] = v
    try:
        yield
    finally:
        for k, v in old.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


@contextlib.contextmanager
def quiet():
    """Capture what a tool prints. Yields the two buffers."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        yield out, err


def script(path, text):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


# A child process gets none of the caller's git state and none of the variables the tools read,
# so the result does not depend on the shell the suite was started from.
CLEAN_ENV = {k: v for k, v in os.environ.items()
             if not k.startswith("GIT_") and k not in ("ESPHOME", "ENVF", "PIPX_BIN_DIR", "PLATFORMIO_BUILD_CACHE_DIR")}

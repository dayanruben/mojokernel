import subprocess,re,os
from pathlib import Path

def _from_pkg_info():
    p = Path(__file__).resolve().parent.parent/'PKG-INFO'
    if not p.exists(): return
    m = re.search(r'^Version:\s*(\S+)\s*$', p.read_text(encoding='utf-8'), re.MULTILINE)
    if m: return m.group(1)

def _get():
    if v:=_from_pkg_info(): return v
    v = os.environ.get('MOJO_VERSION')
    if v: return v
    try:
        out = subprocess.check_output(['mojo', '--version'], text=True, stderr=subprocess.DEVNULL)
        m = re.search(r'\d+\.(\d+\.\d+\.\d+)', out)
        if m: return m.group(1)
    except FileNotFoundError:
        raise RuntimeError("mojo not found — set MOJO_VERSION env var or install Mojo")
    raise RuntimeError(f"Could not parse version from: mojo --version")

__version__ = _get()

import subprocess,re,os

def _get():
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

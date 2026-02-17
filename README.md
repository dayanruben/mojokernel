# mojokernel

A Jupyter kernel for [Mojo](https://www.modular.com/mojo). Supports full variable persistence, function/struct definitions, and error handling across notebook cells.

## Install

Requires Mojo to be installed. Install the wheel matching your Mojo version:

```bash
pip install mojokernel
mojokernel install --sys-prefix
```

Then select "Mojo" in Jupyter's kernel picker.

The pre-built wheel includes a compiled C++ server that talks directly to Mojo's LLDB -- no text parsing, fast and reliable. If the server binary isn't available for your platform, the kernel falls back to a pexpect-based engine that wraps `mojo repl`.

### Pexpect fallback

The pexpect engine is used automatically if the C++ server binary isn't present (e.g. when installing from the sdist). It spawns `mojo repl` and parses its output. To force pexpect mode:

```bash
MOJO_KERNEL_ENGINE=pexpect jupyter lab
```

### Building from source

To build the C++ server yourself (e.g. for development or an unsupported platform):

```bash
brew install llvm   # macOS; on Linux: apt install llvm-18-dev liblldb-18-dev
tools/build_server.sh
```

### PTY server (backup)

A third engine variant (`server/repl_server_pty.cpp`) uses `SBDebugger::RunREPL()` with PTY I/O redirection. This is a fallback in case the `EvaluateExpression` approach breaks in a future Modular release.

## Releases

mojokernel wheels are built against a specific Mojo version. Install the version matching your Mojo:

```bash
# Check your Mojo version
mojo --version          # e.g. "Mojo 0.26.2.0.dev2026021605 (eb42a2ba)"

# Install matching mojokernel
pip install mojokernel==0.26.2
```

### Publishing a new release

When Mojo publishes a new version:

1. Get the Mojo version: `mojo --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+'`
2. Update `version` in `pyproject.toml` to match (e.g. `0.26.2`)
3. Commit and push
4. Go to Actions > "Build and publish wheels" > Run workflow
   - Set `publish: true` to upload to PyPI
   - Optionally set a `mojo_version` constraint to pin the CI install

Platform wheels are built for macOS (Apple Silicon) and Linux (x86_64).

## Testing

```bash
pip install -e .
pytest -q
```

## Architecture

```
mojokernel/
  kernel.py              -- Jupyter kernel (ipykernel subclass)
  engines/
    base.py              -- ExecutionResult dataclass
    pexpect_engine.py    -- pexpect-based engine (default)
    server_engine.py     -- C++ server engine client
server/
  repl_server.cpp        -- C++ server (EvaluateExpression + REPL mode)
  repl_server_pty.cpp    -- PTY-based backup server
  mojo_repl.cpp          -- thin REPL wrapper (RunREPL)
  json.hpp               -- nlohmann/json
tests/
  test_pexpect_engine.py -- pexpect engine tests
  test_server_execute.py -- server engine tests
  test_kernel.py         -- kernel integration tests
tools/
  build_server.sh        -- compile C++ binaries
  server_exec.py         -- send code to server (debugging tool)
  test.sh                -- run pytest
```

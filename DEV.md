# mojokernel internals

How the Mojo REPL works under the hood, and how each engine exploits it.

## Background: Mojo runs on LLDB

Mojo doesn't have a standalone interpreter. All Mojo code execution -- including the REPL -- happens through LLDB's expression evaluation infrastructure. When you run `mojo repl`, it:

1. Initializes LLDB via `SBDebugger::Initialize()`
2. Loads `libMojoLLDB.dylib` (Modular's LLDB plugin that adds Mojo language support) (or libMojoLLDB.so on linux)
3. Creates a target from `mojo-repl-entry-point` (a small binary with a `mojo_repl_main` breakpoint)
4. Launches the target and stops at the breakpoint
5. Calls `SBDebugger::RunREPL(mojo_lang)` to enter interactive REPL mode

The critical challenge for a Jupyter kernel is **variable persistence**. Each call to LLDB's `expression` command creates a new scope -- `var x = 42` in one call is invisible to the next.

## How Mojo REPL achieves variable persistence

Discovered by running `strings` on `libMojoLLDB.dylib`, the REPL uses a context struct pattern:

```mojo
struct __mojo_repl_context__:
    var `x`: __mojo_repl_UnsafePointer[mut=True, __mojo_repl_UnsafePointer[mut=True, Int]]
    pass
```

Each variable declared in the REPL gets a field in this struct as a double-indirection `UnsafePointer`. Every expression is then wrapped in:

```mojo
def __mojo_repl_expr_impl__(mut __mojo_repl_arg: __mojo_repl_context__,
                             mut `x`: Int) -> None:
    var __mojo_repl_expr_failed = True
    @parameter
    def __mojo_repl_expr_body__() -> None:
        # user code goes here
        pass
    __mojo_repl_expr_body__()
    __mojo_repl_expr_failed = False
```

The context struct accumulates fields as you declare variables. Each expression receives the accumulated context as a parameter, giving it access to all previously declared variables. LLDB's `AddPersistentVariable` stores the compiled results across evaluations.

This mechanism is triggered by using the internal `Target::GetREPL()` + `IOHandlerInputComplete()`.

## The Target::GetREPL discovery

LLDB's public SB API (SBTarget) does not expose `GetREPL()` method

However, the internal `lldb/Target/Target.h` exposes `Target::GetREPL()` which returns a REPL object. Routing code through this object via `IOHandlerInputComplete()` gives full variable persistence (likely because it uses the same REPL instance across calls)

Because `SBTarget` wraps the internal Target with a single member

```cpp
class SBTarget {
private:
    lldb::TargetSP m_opaque_sp;
};
```

Then the C++ server accesses the internal Target via `reinterpret_cast`

```cpp
static lldb::TargetSP get_target_sp(lldb::SBTarget &target) {
    return *reinterpret_cast<lldb::TargetSP *>(&target);
}
```

This works because SBTarget has exactly one data member, so its address is the address of `m_opaque_sp`. We reinterpret it and dereference to get the `TargetSP`, then call `Target::GetREPL()` on it

## C++ server engine (`server/repl_server.cpp`)

The server is a single-process C++ binary:

1. **Startup**: Initialize LLDB, load `libMojoLLDB.dylib`/`.so`, create target from `mojo-repl-entry-point`, set breakpoint on `mojo_repl_main`, launch and stop at breakpoint.

2. **Create the LLDB REPL object**:
   ```cpp
   auto mojo_lang = SBLanguageRuntime::GetLanguageTypeFromString("mojo");
   debugger.SetREPLLanguage(mojo_lang);

   TargetSP target_sp = get_target_sp(target);
   lldb_private::Status repl_err;
   REPLSP repl = target_sp->GetREPL(repl_err, mojo_lang, nullptr, true);
   IOHandlerSP io_handler = repl->GetIOHandler();
   ```

   `SBTarget` does not expose `Target::GetREPL()`, so the server unwraps the internal `TargetSP` from `SBTarget` with the single-member layout assumption described above.

3. **JSON protocol loop**: Read JSON from stdin, route execute requests through the persistent REPL instance, and return JSON on stdout.

   ```cpp
   std::string mutable_code = code;
   repl->IOHandlerInputComplete(*io_handler, mutable_code);
   ```

   This uses LLDB's real Mojo REPL path, so `var`/`let` declarations persist across execute requests.

4. **Output capture**: There are two output channels to collect after each execute request:

   - **Target process stdout/stderr**: output produced by running Mojo code, such as `print(...)`. The server drains this with `SBProcess::GetSTDOUT()` and `SBProcess::GetSTDERR()`.

   - **Debugger stdout/stderr**: output produced by LLDB's REPL machinery, especially compiler diagnostics, parse errors, and other REPL messages. The server redirects the debugger's output and error file handles to temporary files, then reads and truncates those files after each request.

   The JSON response combines both sources so notebook users see normal program output and compile/runtime diagnostics from the same execute request.

### Build requirements

The main server uses LLDB's public SB API plus a few LLDB internal headers for the REPL path:

```cpp
#include <lldb/Target/Target.h>      // Target::GetREPL()
#include <lldb/Expression/REPL.h>    // REPLSP, IOHandlerInputComplete()
#include <lldb/Utility/Status.h>     // lldb_private::Status
```

Because these headers expose LLDB/LLVM private types, `server/repl_server.cpp` must be built against matching LLDB headers and linked against Modular's `liblldb` plus LLVM support libraries:

```
-llldb23.0.0git     (from Modular)
-lLLVMSupport       (from brew LLVM)
-lLLVMDemangle      (from brew LLVM)
```

### JSON protocol

```
→ {"type":"execute","code":"var x = 42","id":1}
← {"id":1,"status":"ok","stdout":"","stderr":"","value":""}

→ {"type":"execute","code":"print(x)","id":2}
← {"id":2,"status":"ok","stdout":"42\r\n","stderr":"","value":""}

→ {"type":"execute","code":"print(bad)","id":3}
← {"id":3,"status":"error","stdout":"","stderr":"","ename":"MojoError",
   "evalue":"use of unknown declaration 'bad'","traceback":["..."]}

→ {"type":"shutdown","id":99}
← {"id":99,"status":"ok"}
```

## Pexpect engine (`mojokernel/engines/pexpect_engine.py`)

The pexpect engine spawns `mojo repl` with noise-suppressing LLDB settings:

```
mojo repl \
  -O 'settings set show-statusline false' \
  -O 'settings set show-progress false' \
  -O 'settings set use-color false' \
  -O 'settings set show-autosuggestion false' \
  -O 'settings set auto-indent false'
```

It sets `TERM=dumb` to minimize terminal escape sequences (though editline still produces some ANSI codes that must be stripped).

### Execution flow

1. **Send code**: Each line sent individually via `sendline()`, followed by a blank line to submit.
2. **Read output**: Read from the PTY until a prompt pattern (`\n\s*\d+>\s`) is detected. After the first prompt match, wait 300ms of silence ("settle time") to ensure all output has arrived.
3. **Parse output**: Strip ANSI codes, filter prompt lines (`\d+[>.]\s`) and echo lines, detect `error:` to split output from error messages.

### Why 300ms settle time?

PTY data arrives in chunks. The prompt pattern might appear in the middle of a chunk, with more output still buffered. The settle time ensures we've received everything before returning.

### Error detection

The parser scans for lines containing `error:` (case-insensitive). Once found, all subsequent lines are treated as error output. This matches how the Mojo compiler reports errors through the REPL.

### Testing note

`tests/test_pexpect_engine.py` is marked `@pytest.mark.slow`.

The pexpect fallback engine is not currently our active execution path, so normal development test runs should skip these tests:

```bash
tools/test.sh
```

Run them only when explicitly working on fallback behavior:

```bash
INCLUDE_SLOW=1 tools/test.sh -m slow
```

### Debug exploration tools

Use these scripts to capture reproducible behavior snapshots for offline debugging:

```bash
tools/explore_lsp.py
tools/explore_kernel_client.py
```

Each writes a timestamped JSON report under `meta/` with raw request/response payloads.

`MojoLSPClient` sets `MODULAR_PROFILE_FILENAME` to a temp path by default, so LSP profiling artifacts don't land in the project directory. Set `MODULAR_PROFILE_FILENAME` explicitly to override this.

For live kernel diagnostics, set `MOJO_KERNEL_LSP_DIAG=1` before starting Jupyter. Completion replies will include `_mojokernel_debug` metadata (per-stage success/failure, elapsed ms, and LSP health snapshot on errors), and kernel logs will include LSP warning details/restarts. If needed, tune LSP request timeout with `MOJO_LSP_REQUEST_TIMEOUT` (seconds).

## PTY server backup (`server/repl_server_pty.cpp`)

This is a C++ version of the pexpect approach. It:

1. Creates a PTY pair with `openpty()`
2. Redirects LLDB's stdin/stdout/stderr to the PTY slave via `SetInputFileHandle()`/`SetOutputFileHandle()`
3. Runs `SBDebugger::RunREPL()` in a detached `std::thread`
4. Communicates with the REPL through the PTY master using the same prompt detection and output parsing as the pexpect engine
5. Exposes the same JSON protocol on stdin/stdout as the main server

This exists as a fallback. If Modular changes the internal `SBTarget` layout or the `Target::GetREPL()` / `REPL::IOHandlerInputComplete()` APIs used by the main server, the PTY server should still work because it drives `SBDebugger::RunREPL()` through public LLDB APIs and terminal I/O.

## Why not HandleCommand?

The original server used `ci.HandleCommand("expression -l mojo -- " + code)`. This works for `fn`/`struct`/`trait` definitions (which compile into the persistent LLDB module) but NOT for `var`/`let` declarations. Each `HandleCommand` creates a new expression scope -- variables are local to that scope and disappear after evaluation.

The `Target::GetREPL()` path fixes this by reusing LLDB's actual Mojo REPL object across requests. That REPL object owns the accumulated context struct, so each call through `REPL::IOHandlerInputComplete()` sees declarations from previous cells, matching the behavior of the interactive Mojo REPL.

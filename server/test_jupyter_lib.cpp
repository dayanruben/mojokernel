#include <cstdlib>
#include <dlfcn.h>
#include <iostream>
#include <string>

#include <lldb/API/SBDebugger.h>
#include <lldb/API/SBTarget.h>
#include <lldb/API/SBProcess.h>
#include <lldb/API/SBBreakpoint.h>
#include <lldb/API/SBExpressionOptions.h>
#include <lldb/API/SBValue.h>
#include <lldb/API/SBCommandInterpreter.h>
#include <lldb/API/SBCommandReturnObject.h>
#include <lldb/API/SBLanguageRuntime.h>
#include <lldb/API/SBError.h>
#include <lldb/Expression/REPL.h>
#include <lldb/Utility/Status.h>

// Internal header for Target::GetREPL.
#include <lldb/Target/Target.h>

#include "platform.h"

using namespace lldb;

static std::string drain(SBProcess &proc, size_t (SBProcess::*fn)(char*, size_t) const) {
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = (proc.*fn)(buf, sizeof(buf))) > 0) out.append(buf, n);
    return out;
}

// SBTarget does not expose Target::GetREPL publicly. This depends on SBTarget's
// current layout storing TargetSP as its first/only data member.
static TargetSP get_target_sp(SBTarget &target) {
    return *reinterpret_cast<TargetSP *>(&target);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: test-jupyter-lib <modular-root>\n";
        return 1;
    }
    std::string root = argv[1];
    auto entry_point = root + "/lib/mojo-repl-entry-point";
    auto plugin_path = mojo_lldb_plugin(root);
    auto jupyter_path = mojo_jupyter_library(root);

    setenv("MODULAR_MAX_PACKAGE_ROOT", root.c_str(), 1);
    setenv("MODULAR_MOJO_MAX_PACKAGE_ROOT", root.c_str(), 1);
    setenv("MODULAR_MOJO_MAX_DRIVER_PATH", (root + "/bin/mojo").c_str(), 1);
    setenv("MODULAR_MOJO_MAX_IMPORT_PATH", (root + "/lib/mojo").c_str(), 1);

    SBDebugger::Initialize();
    auto debugger = SBDebugger::Create(false);
    debugger.SetScriptLanguage(eScriptLanguageNone);
    debugger.SetAsync(false);

    auto ci = debugger.GetCommandInterpreter();
    SBCommandReturnObject ret;

    // Load MojoLLDB plugin
    ci.HandleCommand(("plugin load " + plugin_path).c_str(), ret);
    std::cout << "MojoLLDB loaded: " << (ret.Succeeded() ? "yes" : "no") << "\n";

    auto mojo_lang = SBLanguageRuntime::GetLanguageTypeFromString("mojo");
    debugger.SetREPLLanguage(mojo_lang);

    // Create target, breakpoint, launch
    SBError err;
    auto target = debugger.CreateTarget(entry_point.c_str(), "", "", true, err);
    auto bp = target.BreakpointCreateByName("mojo_repl_main");
    auto process = target.LaunchSimple(nullptr, nullptr, nullptr);
    std::cout << "Process state: " << process.GetState() << "\n";

    drain(process, &SBProcess::GetSTDOUT);
    drain(process, &SBProcess::GetSTDERR);

    // Load libMojoJupyter
    std::cout << "\n--- Loading " << jupyter_path << " ---\n";
    void *handle = dlopen(jupyter_path.c_str(), RTLD_NOW);
    if (!handle) {
        std::cout << "dlopen failed: " << dlerror() << "\n";
        return 1;
    }
    std::cout << "dlopen succeeded!\n";

    // Prepare the real Mojo REPL object before the experiments below. Feeding
    // code through its IOHandler preserves REPL variables.
    lldb_private::Status repl_err;
    TargetSP target_sp = get_target_sp(target);
    REPLSP repl = target_sp->GetREPL(repl_err, mojo_lang, nullptr, true);
    if (!repl) {
        std::cout << "GetREPL failed: " << repl_err.AsCString() << "\n";
        process.Destroy();
        SBDebugger::Destroy(debugger);
        SBDebugger::Terminate();
        dlclose(handle);
        return 1;
    }
    IOHandlerSP io_handler = repl->GetIOHandler();
    std::cout << "GetREPL succeeded; IOHandler ready\n";
    drain(process, &SBProcess::GetSTDOUT);
    drain(process, &SBProcess::GetSTDERR);

    std::cout << "\n--- Test 1: var declaration ---\n";
    std::string code1 = "var _jtest = 42";
    repl->IOHandlerInputComplete(*io_handler, code1);
    auto out1 = drain(process, &SBProcess::GetSTDOUT);
    auto err1 = drain(process, &SBProcess::GetSTDERR);
    std::cout << "Succeeded: " << (err1.empty() ? "yes" : "no") << "\n";
    std::cout << "stdout: [" << out1 << "]"
              << " stderr: [" << err1 << "]\n";

    std::cout << "\n--- Test 2: use var ---\n";
    std::string code2 = "print(_jtest)";
    repl->IOHandlerInputComplete(*io_handler, code2);
    auto out2 = drain(process, &SBProcess::GetSTDOUT);
    auto err2 = drain(process, &SBProcess::GetSTDERR);
    std::cout << "Succeeded: " << (err2.empty() ? "yes" : "no") << "\n";
    std::cout << "stdout: [" << out2 << "]"
              << " stderr: [" << err2 << "]\n";

    std::cout << "\n--- Test 3: var mutation ---\n";
    std::string code3 = "_jtest = 99";
    repl->IOHandlerInputComplete(*io_handler, code3);
    auto out3 = drain(process, &SBProcess::GetSTDOUT);
    auto err3 = drain(process, &SBProcess::GetSTDERR);
    std::cout << "Succeeded: " << (err3.empty() ? "yes" : "no") << "\n";
    std::cout << "stdout: [" << out3 << "]"
              << " stderr: [" << err3 << "]\n";

    std::cout << "\n--- Test 4: use mutated var ---\n";
    std::string code4 = "print(_jtest)";
    repl->IOHandlerInputComplete(*io_handler, code4);
    auto out4 = drain(process, &SBProcess::GetSTDOUT);
    auto err4 = drain(process, &SBProcess::GetSTDERR);
    std::cout << "Succeeded: " << (err4.empty() ? "yes" : "no") << "\n";
    std::cout << "stdout: [" << out4 << "]"
              << " stderr: [" << err4 << "]\n";

    io_handler.reset();
    repl.reset();
    target_sp.reset();

    process.Destroy();
    SBDebugger::Destroy(debugger);
    SBDebugger::Terminate();
    dlclose(handle);
    return 0;
}

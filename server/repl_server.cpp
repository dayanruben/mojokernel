// Mojo REPL server using Modular's LLDB REPL object.
// This gives full var/let persistence without PTY or text parsing.
// JSON protocol on stdin/stdout.

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <lldb/API/SBDebugger.h>
#include <lldb/API/SBTarget.h>
#include <lldb/API/SBProcess.h>
#include <lldb/API/SBBreakpoint.h>
#include <lldb/API/SBLanguageRuntime.h>
#include <lldb/API/SBCommandInterpreter.h>
#include <lldb/API/SBCommandReturnObject.h>
#include <lldb/API/SBError.h>
#include <lldb/Expression/REPL.h>
#include <lldb/Utility/Status.h>

// Internal header for Target::GetREPL.
#include <lldb/Target/Target.h>

#include "json.hpp"
#include "platform.h"

using namespace lldb;
using json = nlohmann::json;

[[noreturn]] static void die(const std::string &msg) {
    std::cerr << msg << "\n";
    std::cout << json{{"status", "error"}, {"message", msg}} << "\n" << std::flush;
    std::exit(1);
}

static std::string drain(SBProcess &proc, size_t (SBProcess::*fn)(char*, size_t) const) {
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = (proc.*fn)(buf, sizeof(buf))) > 0) out.append(buf, n);
    return out;
}

// Drain LLDB debugger output captured in a temp file. The file is truncated
// after each read so later responses only include new REPL output.
static std::string drain_file(FILE *file) {
    if (!file) return "";

    fflush(file);
    clearerr(file);
    fseek(file, 0, SEEK_SET);

    std::string out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), file)) > 0)
        out.append(buf, n);

    ftruncate(fileno(file), 0);
    fseek(file, 0, SEEK_SET);
    clearerr(file);
    return out;
}

// Collect output from both LLDB's REPL stream and the launched Mojo process.
struct OutputCapture {
    FILE *debugger_stdout = nullptr;
    FILE *debugger_stderr = nullptr;

    static OutputCapture Create() {
        OutputCapture capture{std::tmpfile(), std::tmpfile()};
        if (!capture.debugger_stdout || !capture.debugger_stderr)
            die("Failed to create debugger output temp files");
        return capture;
    }

    void AttachTo(SBDebugger &debugger) {
        debugger.SetOutputFileHandle(debugger_stdout, false);
        debugger.SetErrorFileHandle(debugger_stderr, false);
    }

    void Clear(SBProcess &process) {
        drain(process, &SBProcess::GetSTDOUT);
        drain(process, &SBProcess::GetSTDERR);
        drain_file(debugger_stdout);
        drain_file(debugger_stderr);
    }

    std::pair<std::string, std::string> Collect(SBProcess &process) {
        auto out = drain_file(debugger_stdout);
        out += drain(process, &SBProcess::GetSTDOUT);
        auto err = drain_file(debugger_stderr);
        err += drain(process, &SBProcess::GetSTDERR);
        return {out, err};
    }
};

static std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> lines;
    std::istringstream ss(s);
    for (std::string line; std::getline(ss, line);)
        if (!line.empty()) lines.push_back(line);
    return lines;
}

// SBTarget does not expose Target::GetREPL publicly. This depends on SBTarget's
// current layout storing TargetSP as its first/only data member.
static TargetSP get_target_sp(SBTarget &target) {
    return *reinterpret_cast<TargetSP *>(&target);
}

static json handle_execute(const std::string &code,
                            SBProcess &process,
                            IOHandlerSP &io_handler,
                            REPLSP &repl,
                            OutputCapture &capture) {
    if (code.empty())
        return {{"status", "ok"}, {"stdout", ""}, {"stderr", ""}, {"value", ""}};

    capture.Clear(process);

    std::string mutable_code = code;
    repl->IOHandlerInputComplete(*io_handler, mutable_code);

    auto [out, serr] = capture.Collect(process);

    if (!serr.empty()) {
        auto tb = split_lines(serr);
        return {{"status", "error"},
                {"stdout", out},
                {"stderr", serr},
                {"ename", "MojoError"},
                {"evalue", tb.empty() ? serr : tb[0]},
                {"traceback", tb}};
    }

    return {{"status", "ok"},
            {"stdout", out},
            {"stderr", serr},
            {"value", ""}};
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: mojo-repl-server <modular-root>\n";
        return 1;
    }
    std::string root = argv[1];
    auto entry_point = root + "/lib/mojo-repl-entry-point";
    auto plugin_path = mojo_lldb_plugin(root);

    setenv("MODULAR_MAX_PACKAGE_ROOT", root.c_str(), 1);
    setenv("MODULAR_MOJO_MAX_PACKAGE_ROOT", root.c_str(), 1);
    setenv("MODULAR_MOJO_MAX_DRIVER_PATH", (root + "/bin/mojo").c_str(), 1);
    setenv("MODULAR_MOJO_MAX_IMPORT_PATH", (root + "/lib/mojo").c_str(), 1);

    SBDebugger::Initialize();
    auto debugger = SBDebugger::Create(false);
    if (!debugger.IsValid()) die("Failed to create SBDebugger");

    debugger.SetScriptLanguage(eScriptLanguageNone);
    debugger.SetAsync(false);

    auto output_capture = OutputCapture::Create();
    output_capture.AttachTo(debugger);

    auto ci = debugger.GetCommandInterpreter();
    SBCommandReturnObject cmd_result;
    ci.HandleCommand(("plugin load " + plugin_path).c_str(), cmd_result);
    if (!cmd_result.Succeeded()) {
        std::string msg = "Failed to load MojoLLDB plugin";
        if (cmd_result.GetError()) msg += std::string(": ") + cmd_result.GetError();
        die(msg);
    }
    std::cerr << "Loaded MojoLLDB plugin\n";

    auto mojo_lang = SBLanguageRuntime::GetLanguageTypeFromString("mojo");
    if (mojo_lang == eLanguageTypeUnknown)
        die("Mojo language not recognized - is libMojoLLDB loaded correctly?");
    debugger.SetREPLLanguage(mojo_lang);
    std::cerr << "Mojo language type: " << static_cast<int>(mojo_lang) << "\n";

    SBError target_err;
    auto target = debugger.CreateTarget(entry_point.c_str(), "", "", true, target_err);
    if (!target.IsValid()) {
        std::string msg = "Failed to create target: " + entry_point;
        if (target_err.Fail()) msg += std::string(": ") + target_err.GetCString();
        die(msg);
    }

    auto bp = target.BreakpointCreateByName("mojo_repl_main");
    if (!bp.IsValid()) die("Failed to create breakpoint at mojo_repl_main");
    std::cerr << "Breakpoint set, " << bp.GetNumLocations() << " location(s)\n";

    auto process = target.LaunchSimple(nullptr, nullptr, nullptr);
    if (!process.IsValid()) die("Failed to launch target process");
    if (process.GetState() != eStateStopped)
        die("Process not stopped after launch (state=" + std::to_string(process.GetState()) + ")");
    std::cerr << "Process launched and stopped at breakpoint\n";

    drain(process, &SBProcess::GetSTDOUT);
    drain(process, &SBProcess::GetSTDERR);

    lldb_private::Status repl_err;
    TargetSP target_sp = get_target_sp(target);
    REPLSP repl = target_sp->GetREPL(repl_err, mojo_lang, nullptr, true);
    if (!repl) die("Failed to get REPL: " + std::string(repl_err.AsCString()));
    IOHandlerSP io_handler = repl->GetIOHandler();
    std::cerr << "REPL mode enabled\n";
    output_capture.Clear(process);

    std::cout << json{{"status", "ready"}} << "\n" << std::flush;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        json req;
        try { req = json::parse(line); }
        catch (const json::parse_error &e) {
            std::cout << json{{"id", 0}, {"status", "error"},
                {"ename", "ProtocolError"}, {"evalue", e.what()}, {"traceback", json::array()}}
                << "\n" << std::flush;
            continue;
        }

        auto type = req.value("type", "");
        auto id = req.value("id", 0);

        json resp;
        if (type == "execute") {
            resp = handle_execute(req.value("code", ""), process, io_handler, repl,
                                  output_capture);
        } else if (type == "complete") {
            resp = {{"status", "ok"}, {"completions", json::array()}};
        } else if (type == "interrupt") {
            process.SendAsyncInterrupt();
            resp = {{"status", "ok"}};
        } else if (type == "shutdown") {
            std::cout << json{{"id", id}, {"status", "ok"}} << "\n" << std::flush;
            break;
        } else {
            resp = {{"status", "error"}, {"ename", "ProtocolError"},
                    {"evalue", "unknown request type: " + type}, {"traceback", json::array()}};
        }

        resp["id"] = id;
        std::cout << resp << "\n" << std::flush;
    }

    io_handler.reset();
    repl.reset();
    target_sp.reset();

    process.Destroy();
    SBDebugger::Destroy(debugger);
    SBDebugger::Terminate();
    return 0;
}

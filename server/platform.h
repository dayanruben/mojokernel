#pragma once
#include <string>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

inline std::string mojo_lldb_plugin(const std::string &root) {
#ifdef __APPLE__
    return root + "/lib/libMojoLLDB.dylib";
#else
    return root + "/lib/libMojoLLDB.so";
#endif
}

// loxpp-lsp: the Lox++ language server.
//
// v1 speaks LSP over stdio only. `--stdio` is accepted (and is the default)
// so an editor that always passes it works without change.

#include <iostream>
#include <string>
#include <string_view>

#include "lsp/json_rpc.h"
#include "lsp/server.h"
#include "lsp/stdlib_docs.h"

int main(int argc, char** argv) {
    bool stdio = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--stdio") {
            stdio = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: loxpp-lsp [--stdio]\n"
                         "  Lox++ language server. Speaks LSP over stdio.\n";
            return 0;
        } else if (arg == "--version") {
            std::cout << "loxpp-lsp 0.1.0\n";
            return 0;
        } else {
            std::cerr << "loxpp-lsp: unknown argument '" << arg << "'\n";
            return 2;
        }
    }
    if (!stdio) {
        std::cerr << "loxpp-lsp: only --stdio transport is supported\n";
        return 2;
    }

    // A stray write to stdout corrupts the LSP stream. Keep it unbuffered and
    // never let iostreams sync with C stdio behind our back.
    std::ios::sync_with_stdio(false);

    for (const std::string& name : loxpp::lsp::stdlibDocsMissingNames()) {
        std::cerr << "loxpp-lsp: warning: no stdlib doc entry for '" << name
                  << "'\n";
    }

    loxpp::lsp::JsonRpc rpc(std::cin, std::cout);
    loxpp::lsp::Server server(rpc);
    server.registerHandlers();
    return rpc.run();
}

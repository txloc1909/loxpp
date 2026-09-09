#pragma once

// The Lox++ language server: capability advertisement and request handlers.
//
// Diagnostics come from analyze() (src/analyze.h) plus the resolver
// warnings on the DocumentModel. Navigation, hover, and completion come
// from that same DocumentModel. The server holds no language knowledge of its
// own beyond the keyword and stdlib doc tables.

#include <cstdint>
#include <string>

#include "lsp/document_store.h"
#include "lsp/json_rpc.h"
#include "lsp/protocol.h"

namespace loxpp::lsp {

class Server {
  public:
    explicit Server(JsonRpc& rpc);

    // Registers every handler. Call once before rpc.run().
    void registerHandlers();

  private:
    // lifecycle
    json onInitialize(const json& params);
    static void onInitialized(const json& params);
    json onShutdown(const json& params);
    void onExit(const json& params);

    // sync
    void onDidOpen(const json& params);
    void onDidChange(const json& params);
    void onDidClose(const json& params);
    void onDidSave(const json& params);

    // language features
    json onDocumentSymbol(const json& params);
    json onHover(const json& params);
    json onCompletion(const json& params);
    json onDefinition(const json& params);
    json onReferences(const json& params);
    json onDocumentHighlight(const json& params);

    // Throws RpcError when the client calls a feature before `initialize`, or
    // any request after `shutdown`.
    void requireReady() const;

    void publishDiagnostics(const std::string& uri, std::int64_t version,
                            const std::vector<Diagnostic>& diags);

    JsonRpc& m_rpc;
    DocumentStore m_store;
    bool m_gotInitialize = false;
    bool m_shutdownReceived = false;
};

} // namespace loxpp::lsp

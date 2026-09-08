#pragma once

// Open-document registry with debounced re-analysis.
//
// Sync model: full text (LSP TextDocumentSyncKind.Full). didOpen and didSave
// publish diagnostics at once. didChange only schedules a rebuild; a burst of
// changes collapses into one rebuild once the stream is quiet for
// kDebounce. The most recent text always wins -- the last change in a burst
// is never dropped.
//
// One background thread does every rebuild. Request handlers read a model
// through read(), which holds the same lock the worker holds while it
// rebuilds, so a handler never sees a half-built model.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lsp/protocol.h"
#include "tooling/document_model.h"

namespace loxpp::lsp {

inline constexpr std::chrono::milliseconds kDebounce{150};

class DocumentStore {
  public:
    // Called with the diagnostics for one document after a rebuild, an open,
    // or a save. Invoked on the worker thread (rebuild) or the calling thread
    // (open / save); never with any internal lock held.
    using PublishFn =
        std::function<void(const std::string& uri, std::int64_t version,
                           const std::vector<Diagnostic>& diags)>;

    explicit DocumentStore(PublishFn publish);
    ~DocumentStore();

    DocumentStore(const DocumentStore&) = delete;
    DocumentStore& operator=(const DocumentStore&) = delete;

    void didOpen(const std::string& uri, std::int64_t version,
                 const std::string& text);
    void didChange(const std::string& uri, std::int64_t version,
                   const std::string& fullText);
    void didClose(const std::string& uri);
    void didSave(const std::string& uri);

    // Runs `fn` with the current model for `uri`, under the rebuild lock.
    // Returns false when the document is unknown.
    [[nodiscard]] bool
    read(const std::string& uri,
         const std::function<void(const tooling::DocumentModel&)>& fn);

  private:
    struct Entry {
        explicit Entry(std::string text) : model(std::move(text)) {}
        tooling::DocumentModel model;
        std::int64_t version = 0;
        std::string pendingText;
        std::int64_t pendingVersion = 0;
        std::chrono::steady_clock::time_point dueAt;
        bool dirty = false;
    };

    void workerLoop();
    // Builds the LSP diagnostic list for `entry` from analyze() and the
    // resolver warnings. Caller holds m_mutex when `entry` is a live map node.
    [[nodiscard]] static std::vector<Diagnostic>
    diagnosticsFor(const Entry& entry);

    PublishFn m_publish;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<std::string, std::unique_ptr<Entry>> m_docs;
    bool m_shutdown = false;
    std::thread m_worker;
};

// Replaces every CRLF with a single LF. CR is whitespace in Lox++
// (spec/01-lexical.md); normalising here keeps one line index that both the
// server and the client agree on, since an LSP client counts a CRLF as one
// line break and never counts the CR as a character.
[[nodiscard]] std::string normalizeNewlines(std::string text);

} // namespace loxpp::lsp

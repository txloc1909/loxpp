#include "lsp/document_store.h"

#include <utility>

#include "analyze.h"
#include "diagnostic.h"

namespace loxpp::lsp {

std::string normalizeNewlines(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue; // drop the CR, keep the following LF
        }
        out.push_back(text[i]);
    }
    return out;
}

namespace {

Position toLsp(const tooling::Position& p) {
    return {static_cast<std::uint32_t>(p.line),
            static_cast<std::uint32_t>(p.character)};
}

DiagnosticSeverity toLspSeverity(Severity s) {
    switch (s) {
    case Severity::Error:
        return DiagnosticSeverity::Error;
    case Severity::Warning:
        return DiagnosticSeverity::Warning;
    case Severity::Info:
        return DiagnosticSeverity::Information;
    }
    return DiagnosticSeverity::Error;
}

} // namespace

DocumentStore::DocumentStore(PublishFn publish)
    : m_publish(std::move(publish)), m_worker([this] { workerLoop(); }) {}

DocumentStore::~DocumentStore() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

std::vector<Diagnostic> DocumentStore::diagnosticsFor(const Entry& entry) {
    const tooling::DocumentModel& model = entry.model;
    std::vector<Diagnostic> out;

    auto append = [&](const ::Diagnostic& d, const char* source) {
        Diagnostic lsp;
        lsp.range.start = toLsp(model.offsetToPosition(d.offset));
        lsp.range.end = toLsp(model.offsetToPosition(d.offset + d.length));
        lsp.severity = toLspSeverity(d.severity);
        lsp.source = source;
        lsp.message = d.message;
        out.push_back(std::move(lsp));
    };

    for (const ::Diagnostic& d : analyze(model.text())) {
        append(d, "loxpp");
    }
    for (const ::Diagnostic& d : model.warnings()) {
        append(d, "loxpp");
    }
    return out;
}

void DocumentStore::didOpen(const std::string& uri, std::int64_t version,
                            const std::string& text) {
    std::vector<Diagnostic> diags;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto entry = std::make_unique<Entry>(normalizeNewlines(text));
        entry->version = version;
        diags = diagnosticsFor(*entry);
        m_docs[uri] = std::move(entry);
    }
    m_publish(uri, version, diags);
}

void DocumentStore::didChange(const std::string& uri, std::int64_t version,
                              const std::string& fullText) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_docs.find(uri);
    if (it == m_docs.end()) {
        return;
    }
    Entry& e = *it->second;
    e.pendingText = normalizeNewlines(fullText);
    e.pendingVersion = version;
    e.dirty = true;
    e.dueAt = std::chrono::steady_clock::now() + kDebounce;
    m_cv.notify_all();
}

void DocumentStore::didClose(const std::string& uri) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_docs.erase(uri);
}

void DocumentStore::didSave(const std::string& uri) {
    std::vector<Diagnostic> diags;
    std::int64_t version = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_docs.find(uri);
        if (it == m_docs.end()) {
            return;
        }
        version = it->second->version;
        diags = diagnosticsFor(*it->second);
    }
    m_publish(uri, version, diags);
}

bool DocumentStore::read(
    const std::string& uri,
    const std::function<void(const tooling::DocumentModel&)>& fn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_docs.find(uri);
    if (it == m_docs.end()) {
        return false;
    }
    fn(it->second->model);
    return true;
}

void DocumentStore::workerLoop() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
        // Find the dirty entry whose debounce window ends soonest.
        Entry* next = nullptr;
        for (auto& [uri, entryPtr] : m_docs) {
            if (!entryPtr->dirty) {
                continue;
            }
            if (next == nullptr || entryPtr->dueAt < next->dueAt) {
                next = entryPtr.get();
            }
        }

        if (m_shutdown) {
            return;
        }
        if (next == nullptr) {
            m_cv.wait(lock);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < next->dueAt) {
            m_cv.wait_until(lock, next->dueAt);
            continue; // re-scan: a newer change may have pushed the deadline
        }

        // The window has closed with no newer change. Rebuild once.
        next->model.rebuild(std::move(next->pendingText));
        next->pendingText.clear();
        next->version = next->pendingVersion;
        next->dirty = false;

        const std::string uri = [&] {
            for (const auto& [key, value] : m_docs) {
                if (value.get() == next) {
                    return key;
                }
            }
            return std::string{};
        }();
        const std::int64_t version = next->version;
        const std::vector<Diagnostic> diags = diagnosticsFor(*next);

        lock.unlock();
        if (!uri.empty()) {
            m_publish(uri, version, diags);
        }
        lock.lock();
    }
}

} // namespace loxpp::lsp

#pragma once

// JSON-RPC 2.0 over stdio with LSP `Content-Length` framing.
//
// The connection owns the read loop. It parses one message at a time, routes
// requests and notifications through a handler table, and writes responses and
// server-initiated notifications back with a mutex so a background thread may
// publish diagnostics while the main thread answers a request.
//
// stdout carries LSP traffic only. Every log line goes to stderr.

#include <functional>
#include <istream>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace loxpp::lsp {

using json = nlohmann::json;

// JSON-RPC error codes used by this server (a subset of the spec set).
enum class RpcErrorCode : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    // LSP-specific.
    ServerNotInitialized = -32002,
    RequestFailed = -32803,
};

// Thrown by a request handler to turn into a JSON-RPC error response.
struct RpcError {
    RpcErrorCode code;
    std::string message;
};

class JsonRpc {
  public:
    // A request handler returns the `result` value. Throw RpcError for an
    // error response.
    using RequestHandler = std::function<json(const json& params)>;
    using NotificationHandler = std::function<void(const json& params)>;

    JsonRpc(std::istream& in, std::ostream& out);

    void onRequest(const std::string& method, RequestHandler handler);
    void onNotification(const std::string& method, NotificationHandler handler);

    // Server -> client notification (for example
    // textDocument/publishDiagnostics). Safe to call from any thread.
    void notify(const std::string& method, json params);

    // Reads and dispatches messages until end of input or exit(). Returns the
    // process exit code the caller should use.
    int run();

    // Ask the loop to stop after the current message. `code` is the value
    // run() returns.
    void requestStop(int code);

  private:
    // Reads one framed message. Returns nullopt at end of input.
    std::optional<std::string> readFramedMessage();

    void dispatch(const json& message);
    void sendResult(const json& id, json result);
    void sendError(const json& id, RpcErrorCode code,
                   const std::string& message);
    void writeRaw(const json& message);

    std::istream& m_in;
    std::ostream& m_out;
    std::mutex m_writeMutex;

    std::unordered_map<std::string, RequestHandler> m_requestHandlers;
    std::unordered_map<std::string, NotificationHandler> m_notificationHandlers;

    bool m_stop = false;
    int m_exitCode = 0;
};

} // namespace loxpp::lsp

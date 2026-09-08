#include "lsp/json_rpc.h"

#include <cctype>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <utility>

namespace loxpp::lsp {

namespace {

void logLine(std::string_view text) {
    std::cerr << "loxpp-lsp: " << text << '\n';
}

// Trims trailing CR and surrounding spaces from an HTTP-style header line.
std::string trim(std::string s) {
    while (!s.empty() &&
           (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    std::size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
        ++start;
    }
    return s.substr(start);
}

} // namespace

JsonRpc::JsonRpc(std::istream& in, std::ostream& out) : m_in(in), m_out(out) {}

void JsonRpc::onRequest(const std::string& method, RequestHandler handler) {
    m_requestHandlers[method] = std::move(handler);
}

void JsonRpc::onNotification(const std::string& method,
                             NotificationHandler handler) {
    m_notificationHandlers[method] = std::move(handler);
}

void JsonRpc::requestStop(int code) {
    m_stop = true;
    m_exitCode = code;
}

std::optional<std::string> JsonRpc::readFramedMessage() {
    std::size_t contentLength = 0;
    bool haveLength = false;

    // Header section: lines terminated by CRLF, ended by an empty line.
    std::string line;
    while (std::getline(m_in, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty()) {
            break; // end of headers
        }
        const std::size_t colon = trimmed.find(':');
        if (colon == std::string::npos) {
            continue; // ignore a malformed header line
        }
        std::string name = trim(trimmed.substr(0, colon));
        std::string value = trim(trimmed.substr(colon + 1));
        for (char& c : name) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (name == "content-length") {
            try {
                contentLength = static_cast<std::size_t>(std::stoull(value));
                haveLength = true;
            } catch (const std::exception&) {
                haveLength = false;
            }
        }
    }

    if (!haveLength) {
        return std::nullopt; // end of input, or a frame with no length
    }

    std::string body(contentLength, '\0');
    m_in.read(body.data(), static_cast<std::streamsize>(contentLength));
    const auto got = static_cast<std::size_t>(m_in.gcount());
    if (got != contentLength) {
        return std::nullopt; // truncated stream
    }
    return body;
}

int JsonRpc::run() {
    while (!m_stop) {
        std::optional<std::string> body = readFramedMessage();
        if (!body) {
            break; // end of input
        }

        json message;
        try {
            message = json::parse(*body);
        } catch (const json::parse_error& e) {
            logLine(std::string("parse error: ") + e.what());
            sendError(nullptr, RpcErrorCode::ParseError, "invalid JSON");
            continue;
        }

        try {
            dispatch(message);
        } catch (const std::exception& e) {
            logLine(std::string("dispatch error: ") + e.what());
        }
    }
    return m_exitCode;
}

void JsonRpc::dispatch(const json& message) {
    if (!message.is_object() || !message.contains("method") ||
        !message.at("method").is_string()) {
        if (message.is_object() && message.contains("id")) {
            sendError(message.at("id"), RpcErrorCode::InvalidRequest,
                      "not a valid request");
        }
        return;
    }

    const std::string method = message.at("method").get<std::string>();
    const json params =
        message.contains("params") ? message.at("params") : json::object();
    const bool isRequest = message.contains("id");

    if (isRequest) {
        const json& id = message.at("id");
        auto it = m_requestHandlers.find(method);
        if (it == m_requestHandlers.end()) {
            sendError(id, RpcErrorCode::MethodNotFound,
                      "method not found: " + method);
            return;
        }
        try {
            sendResult(id, it->second(params));
        } catch (const RpcError& e) {
            sendError(id, e.code, e.message);
        } catch (const std::exception& e) {
            sendError(id, RpcErrorCode::InternalError, e.what());
        }
        return;
    }

    auto it = m_notificationHandlers.find(method);
    if (it == m_notificationHandlers.end()) {
        return; // an unknown notification is dropped, per the LSP spec
    }
    try {
        it->second(params);
    } catch (const std::exception& e) {
        logLine(std::string("notification handler error: ") + e.what());
    }
}

void JsonRpc::notify(const std::string& method, json params) {
    json message = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", std::move(params)},
    };
    writeRaw(message);
}

void JsonRpc::sendResult(const json& id, json result) {
    json message = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)},
    };
    writeRaw(message);
}

void JsonRpc::sendError(const json& id, RpcErrorCode code,
                        const std::string& message) {
    json msg = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", static_cast<int>(code)}, {"message", message}}},
    };
    writeRaw(msg);
}

void JsonRpc::writeRaw(const json& message) {
    const std::string body = message.dump();
    std::lock_guard<std::mutex> lock(m_writeMutex);
    m_out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    m_out.flush();
}

} // namespace loxpp::lsp

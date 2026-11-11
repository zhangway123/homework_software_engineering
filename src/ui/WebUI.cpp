#include "ui/WebUI.h"

#include "service/AuthService.h"
#include "service/ProductService.h"
#include "service/ReturnService.h"
#include "service/SearchService.h"
#include "service/TradeService.h"
// TODO(REQ-006)：实现消息接口后启用：
// #include "service/MessageService.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#error "WebUI 目前只实现了 Windows (winsock2) 版本，Linux 下请改用 BSD socket"
#endif

namespace {

constexpr int kBacklog = 8;
constexpr size_t kMaxRequestBytes = 1024 * 1024;  // 1 MiB，防止测试时误发超大请求

// ---------- 基础工具 ----------

std::string trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size() &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parseQuery(const std::string& target) {
    std::unordered_map<std::string, std::string> params;
    const size_t mark = target.find('?');
    if (mark == std::string::npos) return params;

    std::string rest = target.substr(mark + 1);
    size_t pos = 0;
    while (pos <= rest.size()) {
        const size_t next = rest.find('&', pos);
        const std::string pair = rest.substr(pos, next == std::string::npos ? std::string::npos
                                                                           : next - pos);
        const size_t eq = pair.find('=');
        const std::string key = urlDecode(eq == std::string::npos ? pair : pair.substr(0, eq));
        const std::string value = eq == std::string::npos ? std::string() : urlDecode(pair.substr(eq + 1));
        if (!key.empty()) params[key] = value;
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return params;
}

bool parseNumber(const std::string& text, double& out) {
    if (text.empty()) return false;
    try {
        size_t used = 0;
        out = std::stod(text, &used);
        return used == text.size();
    } catch (...) {
        return false;
    }
}

// 从 request.target 解析搜索条件；数字字段非法时返回 false
bool parseSearchQuery(const std::string& target, SearchQuery& query) {
    const auto params = parseQuery(target);
    auto get = [&params](const char* key) {
        const auto it = params.find(key);
        return it == params.end() ? std::string() : it->second;
    };

    query.keyword = get("keyword");
    query.category = get("category");

    const std::string minText = get("minPrice");
    const std::string maxText = get("maxPrice");
    if (minText.empty() && maxText.empty()) return true;

    query.hasPriceRange = true;
    query.minPrice = 0;
    query.maxPrice = std::numeric_limits<double>::max();
    if (!minText.empty() && !parseNumber(minText, query.minPrice)) return false;
    if (!maxText.empty() && !parseNumber(maxText, query.maxPrice)) return false;
    return true;
}

// 按 UTF-8 码点计数，保证中文用户名也算 1 个字符
size_t utf8Length(const std::string& s) {
    size_t count = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++count;
    }
    return count;
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // 其它控制字符忽略即可，测试页面用不到
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// 极简 JSON 字符串字段提取：够测试页面用，遇到嵌套/数组等复杂结构请换真正的 JSON 库
bool jsonStringField(const std::string& body, const std::string& key, std::string& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return false;

    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size() || body[pos] != '"') return false;
    ++pos;

    std::string value;
    while (pos < body.size()) {
        char c = body[pos++];
        if (c == '\\' && pos < body.size()) {
            char esc = body[pos++];
            switch (esc) {
                case 'n':  value += '\n'; break;
                case 'r':  value += '\r'; break;
                case 't':  value += '\t'; break;
                case '"':  value += '"';  break;
                case '\\': value += '\\'; break;
                case '/':  value += '/';  break;
                default:   value += esc;  break;  // \uXXXX 简化为原始字符
            }
            continue;
        }
        if (c == '"') {
            out = value;
            return true;
        }
        value += c;
    }
    return false;
}

// 数字字段：只做测试页够用的解析，不支持对象/数组
bool jsonNumberField(const std::string& body, const std::string& key, double& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return false;

    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;

    size_t end = pos;
    while (end < body.size()) {
        char c = body[end];
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' ||
            c == '.' || c == 'e' || c == 'E') {
            ++end;
        } else {
            break;
        }
    }
    if (end == pos) return false;

    try {
        out = std::stod(body.substr(pos, end - pos));
    } catch (...) {
        return false;
    }
    return true;
}

bool jsonLongField(const std::string& body, const std::string& key, long long& out) {
    double value = 0;
    if (!jsonNumberField(body, key, value)) return false;
    out = static_cast<long long>(value);
    return true;
}

bool jsonBoolField(const std::string& body, const std::string& key, bool& out) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return false;

    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;

    if (body.compare(pos, 4, "true") == 0)  { out = true;  return true; }
    if (body.compare(pos, 5, "false") == 0) { out = false; return true; }
    return false;
}

std::string okJson() { return "{\"ok\":true}"; }

std::string errorJson(const std::string& msg, int code) {
    return "{\"ok\":false,\"code\":" + std::to_string(code) +
           ",\"msg\":\"" + jsonEscape(msg) + "\"}";
}

// ---------- HTTP 收发 ----------

std::string httpResponse(int status, const char* reason,
                         const std::string& contentType, const std::string& body,
                         const std::string& extraHeaders = "") {
    std::string response;
    response += "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += extraHeaders;  // 每个头自带 "\r\n"
    response += "Connection: close\r\n";
    response += "\r\n";
    response += body;
    return response;
}

// 32 位十六进制随机会话 id
std::string randomToken() {
    std::random_device rd;
    std::mt19937_64 gen(rd() ^ (static_cast<uint64_t>(rd()) << 32));
    static const char* hex = "0123456789abcdef";
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i)
        token += hex[static_cast<size_t>(gen() & 0xF)];
    return token;
}

std::string cookieValue(const std::string& cookieHeader, const std::string& name) {
    size_t pos = 0;
    while (pos < cookieHeader.size()) {
        size_t end = cookieHeader.find(';', pos);
        if (end == std::string::npos) end = cookieHeader.size();
        std::string part = trim(cookieHeader.substr(pos, end - pos));
        size_t equals = part.find('=');
        if (equals != std::string::npos && trim(part.substr(0, equals)) == name)
            return trim(part.substr(equals + 1));
        pos = end + 1;
    }
    return "";
}

bool sendAll(SOCKET socket, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(socket, data.data() + sent,
                       static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

struct HttpRequest {
    std::string method;
    std::string target;
    std::string body;
    std::string cookie;
    bool valid = false;
};

HttpRequest readRequest(SOCKET client) {
    HttpRequest request;
    std::string data;
    char buffer[4096];

    size_t headerEnd = data.find("\r\n\r\n");
    while (headerEnd == std::string::npos) {
        int n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) return request;
        data.append(buffer, static_cast<size_t>(n));
        if (data.size() > kMaxRequestBytes) return request;
        headerEnd = data.find("\r\n\r\n");
    }

    std::istringstream head(data.substr(0, headerEnd));
    std::string line;
    if (!std::getline(head, line)) return request;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    std::istringstream firstLine(line);
    std::string version;
    if (!(firstLine >> request.method >> request.target >> version)) return request;

    request.body = data.substr(headerEnd + 4);

    size_t contentLength = 0;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = toLower(trim(line.substr(0, colon)));
        if (name == "content-length") {
            try {
                contentLength = static_cast<size_t>(std::stoul(trim(line.substr(colon + 1))));
            } catch (...) {
                contentLength = 0;
            }
        } else if (name == "cookie") {
            request.cookie = trim(line.substr(colon + 1));
        }
    }

    while (request.body.size() < contentLength) {
        int n = ::recv(client, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        request.body.append(buffer, static_cast<size_t>(n));
        if (request.body.size() > kMaxRequestBytes) return HttpRequest{};
    }
    if (request.body.size() > contentLength) request.body.resize(contentLength);

    request.valid = true;
    return request;
}

void serveOne(WebUI& ui, SOCKET client) {
    HttpRequest request = readRequest(client);
    if (!request.valid) return;

    std::string path = request.target;
    size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query);

    // 身份只从会话 Cookie 推导，前端传的 id 一概不信
    const long long accountId = ui.sessionAccount(request.cookie);

    std::string response;
    std::string logExtra;

    const char* jsonType = "application/json; charset=utf-8";

    if (request.method == "GET" && (path == "/" || path == "/index.html")) {
        response = httpResponse(200, "OK", "text/html; charset=utf-8", ui.registrationPage());
    } else if (request.method == "GET" && path == "/products") {
        response = httpResponse(200, "OK", "text/html; charset=utf-8", ui.productPage());
    } else if (request.method == "GET" && path == "/api/me") {
        response = httpResponse(200, "OK", jsonType, ui.meJson(request.cookie));
    } else if (request.method == "GET" && path == "/api/users") {
        response = httpResponse(200, "OK", jsonType, ui.registeredUsersJson());
    } else if (request.method == "GET" && path == "/api/products") {
        response = httpResponse(200, "OK", jsonType, ui.productsJson());
    } else if (request.method == "GET" && path == "/api/reviews") {
        response = httpResponse(200, "OK", jsonType, ui.reviewsJson());
    } else if (request.method == "GET" && path == "/api/reservations") {
        response = httpResponse(200, "OK", jsonType, ui.reservationsJson());
    } else if (request.method == "GET" && path == "/api/orders") {
        response = httpResponse(200, "OK", jsonType, ui.ordersJson());
    } else if (request.method == "GET" && path == "/api/returns") {
        response = httpResponse(200, "OK", jsonType, ui.returnsJson());
    } else if (request.method == "GET" && path == "/api/escrow") {
        response = httpResponse(200, "OK", jsonType, ui.escrowJson());
    } else if (request.method == "GET" && path == "/api/search") {
        SearchQuery query;
        const std::string json = parseSearchQuery(request.target, query)
            ? ui.searchJson(query)
            : errorJson("invalid price range", static_cast<int>(Err::InvalidArg));
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
        // TODO(REQ-006)：消息接口路由放在这一组里，写法与上面保持一致——
        //   GET  /api/conversations
        //        -> ui.conversationsJson(accountId)
        //   GET  /api/messages?conversationId=N
        //        -> ui.messagesJson(conversationId, accountId)
        //   POST /api/conversations
        //        -> ui.handleOpenConversation(request.body, accountId)
        //   POST /api/messages
        //        -> ui.handleSendMessage(request.body, accountId)
        // 坑：readRequest 保留了完整 target，但上面已经把 path 截到 '?' 之前；
        // 取 conversationId 时要自己解析 request.target 的 query，
        // 或直接约定为 /api/conversations/<id>/messages 这类路径。
        // 所有 handler 的 accountId 都来自 sessionAccount(request.cookie)，
        // 绝不能让前端自己传 senderId / accountId。
    } else if (request.method == "POST" && path == "/api/register") {
        WebReply reply = ui.handleRegister(request.body);
        response = httpResponse(200, "OK", jsonType, reply.json, reply.setCookie);
        logExtra = "  " + reply.json;
    } else if (request.method == "POST" && path == "/api/login") {
        WebReply reply = ui.handleLogin(request.body);
        response = httpResponse(200, "OK", jsonType, reply.json, reply.setCookie);
        logExtra = "  " + reply.json;
    } else if (request.method == "POST" && path == "/api/logout") {
        WebReply reply = ui.handleLogout(request.cookie);
        response = httpResponse(200, "OK", jsonType, reply.json, reply.setCookie);
        logExtra = "  " + reply.json;
    } else if (request.method == "POST" && path == "/api/dev/account") {
        std::string json = ui.handleCreateAccount(request.body);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/account/recharge") {
        std::string json = ui.handleRecharge(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/products") {
        std::string json = ui.handleAddProduct(accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/products/draft") {
        std::string json = ui.handleSaveDraft(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/products/submit") {
        std::string json = ui.handleSubmitReview(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/review") {
        std::string json = ui.handleReview(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/products/remove") {
        std::string json = ui.handleRemoveFromShelf(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/reservations") {
        std::string json = ui.handleReserve(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/reservations/cancel") {
        std::string json = ui.handleCancelReservation(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/reservations/confirm") {
        std::string json = ui.handleConfirmReservation(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/reservations/buy") {
        std::string json = ui.handleBuy(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/orders/ship") {
        std::string json = ui.handleShip(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/orders/receipt") {
        std::string json = ui.handleConfirmReceipt(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/returns") {
        std::string json = ui.handleReturnApply(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/returns/review") {
        std::string json = ui.handleReturnReview(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/returns/delivered") {
        std::string json = ui.handleReturnDelivered(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else if (request.method == "POST" && path == "/api/returns/received") {
        std::string json = ui.handleReturnReceived(request.body, accountId);
        response = httpResponse(200, "OK", jsonType, json);
        logExtra = "  " + json;
    } else {
        response = httpResponse(404, "Not Found", jsonType,
                                "{\"ok\":false,\"msg\":\"not found\"}");
    }

    std::cout << "[web] " << request.method << " " << path << logExtra << "\n";
    sendAll(client, response);
}

}  // namespace

// ---------- WebUI ----------

// TODO(REQ-006)：构造函数参数在 searchService 之后加 MessageService& messageService，
// 初始化列表里绑定 messages(messageService)；对应修改 WebUI.h 与 main.cpp。
WebUI::WebUI(AuthService& authService, ProductService& productService,
             TradeService& tradeService, ReturnService& returnService,
             SearchService& searchService, unsigned short listenPort)
    : auth(authService), products(productService), trades(tradeService),
      returns(returnService), search(searchService), port(listenPort) {}

void WebUI::run() {
    std::cout << std::unitbuf;  // 日志立即刷新，方便边测边看
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[web] WSAStartup failed\n";
        return;
    }

    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::cerr << "[web] socket() failed: " << WSAGetLastError() << "\n";
        WSACleanup();
        return;
    }

    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 只监听本机
    address.sin_port = htons(port);

    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        std::cerr << "[web] bind() failed on port " << port
                  << " (port in use? try another one)\n";
        closesocket(listener);
        WSACleanup();
        return;
    }

    if (::listen(listener, kBacklog) == SOCKET_ERROR) {
        std::cerr << "[web] listen() failed: " << WSAGetLastError() << "\n";
        closesocket(listener);
        WSACleanup();
        return;
    }

    std::cout << "[web] registration test page: http://127.0.0.1:" << port << "/\n";
    std::cout << "[web] press Ctrl+C to stop\n";

    for (;;) {
        SOCKET client = ::accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        serveOne(*this, client);
        closesocket(client);
    }
}

std::string WebUI::registrationPage() const {
    return R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>注册测试 · 二手交易平台</title>
<style>
:root{
  --bg:#f3f5f4; --surface:#ffffff; --ink:#17211f; --ink-2:#3d4a47; --muted:#788683;
  --line:#e6ebe9; --line-strong:#d2dbd8; --primary:#11685a; --primary-dark:#0c5247;
  --primary-soft:#e7f2ef; --accent:#e2892f; --price:#c94f3d;
  --radius:14px;
  --shadow-sm:0 1px 2px rgba(16,35,30,.05),0 1px 3px rgba(16,35,30,.06);
  --shadow-md:0 8px 24px rgba(16,35,30,.09);
  font-family:"PingFang SC","Microsoft YaHei","Segoe UI",system-ui,-apple-system,sans-serif;
}
*{box-sizing:border-box}
[hidden]{display:none !important}
html,body{margin:0;padding:0;background:var(--bg);color:var(--ink);font-size:14px;line-height:1.5;-webkit-font-smoothing:antialiased}
h1,h2,h3,p{margin:0}
a{color:inherit;text-decoration:none}
button{font-family:inherit}
svg{display:block}

.btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;height:44px;padding:0 18px;
     border:1px solid transparent;border-radius:10px;background:none;color:var(--ink-2);
     font-size:14px;font-weight:600;cursor:pointer;transition:background .15s,border-color .15s,color .15s,box-shadow .15s}
.btn-primary{background:var(--primary);color:#fff;box-shadow:0 6px 14px rgba(17,104,90,.18)}
.btn-primary:hover{background:var(--primary-dark)}
.btn-outline{background:#fff;border-color:var(--line-strong);color:var(--ink-2)}
.btn-outline:hover{border-color:var(--primary);color:var(--primary)}
.btn-ghost{background:transparent;color:var(--muted)}
.btn-ghost:hover{background:#f0f3f2;color:var(--ink-2)}
.btn-sm{height:36px;padding:0 14px;font-size:13px;border-radius:9px}
.btn-block{width:100%}
.btn:disabled{opacity:.55;cursor:wait}

.login-page{min-height:100vh;display:grid;grid-template-columns:1.06fr .94fr;background:var(--surface)}

.brand-panel{position:relative;display:flex;flex-direction:column;padding:48px 64px 40px;overflow:hidden;color:#fff;
  background:
    radial-gradient(900px 520px at 92% 6%,rgba(88,190,165,.34),transparent 62%),
    radial-gradient(700px 480px at -8% 96%,rgba(228,160,74,.20),transparent 55%),
    linear-gradient(155deg,#0b4c42 0%,#11685a 48%,#1a7f6c 100%)}
.brand-panel::before{content:"";position:absolute;right:-160px;top:52%;width:520px;height:520px;
  border:1px solid rgba(255,255,255,.10);border-radius:50%;pointer-events:none;
  box-shadow:0 0 0 70px rgba(255,255,255,.035),0 0 0 140px rgba(255,255,255,.02)}
.brand-panel::after{content:"";position:absolute;inset:0;pointer-events:none;opacity:.9;
  background-image:linear-gradient(rgba(255,255,255,.028) 1px,transparent 1px),
                   linear-gradient(90deg,rgba(255,255,255,.028) 1px,transparent 1px);
  background-size:44px 44px;
  mask-image:linear-gradient(180deg,transparent,#000 55%,transparent)}
.brand-top{position:relative;z-index:1;display:flex;align-items:center;gap:11px;font-size:19px;font-weight:700;letter-spacing:.6px}
.logo-mark{width:36px;height:36px;border-radius:11px;display:grid;place-items:center;color:#fff;font-size:18px;font-weight:700;
  background:rgba(255,255,255,.14);border:1px solid rgba(255,255,255,.28);backdrop-filter:blur(6px)}
.brand-copy{position:relative;z-index:1;margin-top:84px;max-width:500px}
.eyebrow{display:inline-flex;align-items:center;gap:8px;margin-bottom:20px;padding:6px 13px;
  border:1px solid rgba(255,255,255,.22);border-radius:999px;background:rgba(255,255,255,.07);
  color:rgba(255,255,255,.78);font-size:12px;letter-spacing:1.5px}
.eyebrow .dot{width:6px;height:6px;border-radius:50%;background:#6fd7b9;box-shadow:0 0 0 4px rgba(111,215,185,.16)}
.brand-title{font-size:44px;line-height:1.28;font-weight:800;letter-spacing:1px}
.brand-title em{font-style:normal;color:#8fe0c7}
.brand-sub{max-width:430px;margin-top:20px;font-size:15px;line-height:1.85;color:rgba(255,255,255,.68)}
.brand-features{position:relative;z-index:1;display:grid;gap:14px;margin:auto 0 0;padding-top:46px;list-style:none}
.brand-features li{display:flex;align-items:center;gap:12px;font-size:13.5px;color:rgba(255,255,255,.82)}
.feature-tick{flex:none;width:22px;height:22px;display:grid;place-items:center;border-radius:50%;
  background:rgba(255,255,255,.12);color:#8fe0c7;font-size:12px;font-weight:700}
.brand-foot{position:relative;z-index:1;display:flex;justify-content:space-between;margin-top:38px;
  padding-top:20px;border-top:1px solid rgba(255,255,255,.12);font-size:12px;color:rgba(255,255,255,.45)}

.form-panel{display:grid;place-items:center;padding:48px 56px;
  background:radial-gradient(680px 460px at 100% 0%,#f2f7f5,transparent 60%),#fbfcfc}
.login-card{width:min(430px,100%)}
.login-card h1{font-size:29px;font-weight:800;letter-spacing:.5px}
.login-welcome{margin-top:10px;font-size:14px;color:var(--muted)}

.tabs{display:inline-flex;gap:4px;margin:24px 0 20px;padding:4px;border-radius:11px;background:#e9eeec}
.tab{padding:8px 24px;border:0;border-radius:8px;background:transparent;color:var(--muted);
     font-size:13.5px;font-weight:600;cursor:pointer;transition:color .15s,background .15s}
.tab:hover{color:var(--ink-2)}
.tab.active{background:#fff;color:var(--ink);box-shadow:var(--shadow-sm)}
.panel{display:none}
.panel.active{display:block}

.field{margin-bottom:16px}
.field-label{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:8px;
  font-size:13px;font-weight:600;color:var(--ink-2)}
.hint{font-size:12px;font-weight:400;color:var(--muted)}
.input-shell{display:flex;align-items:center;gap:10px;height:48px;padding:0 14px;
  border:1px solid var(--line-strong);border-radius:11px;background:#fff;
  transition:border-color .15s,box-shadow .15s}
.input-shell:focus-within{border-color:var(--primary);box-shadow:0 0 0 4px rgba(17,104,90,.10)}
.input-shell input{flex:1;min-width:0;border:0;outline:0;background:transparent;color:var(--ink);
  font:inherit;font-size:14px}
.input-shell input::placeholder{color:#a8b2af}
.check{display:flex;align-items:center;gap:9px;margin:2px 0 20px;font-size:12.5px;color:var(--ink-2);cursor:pointer}
.check input{width:15px;height:15px;margin:0;accent-color:var(--primary)}
#submitBtn,#loginBtn{height:48px;font-size:15px;border-radius:11px}

.result{margin:14px 0 0;padding:11px 13px;border-radius:10px;font-size:13px;line-height:1.55;word-break:break-word;
  background:#f4faf8;border:1px solid #dbeae5;color:var(--ink-2)}
.result.good{background:var(--primary-soft);border-color:rgba(17,104,90,.28);color:var(--primary-dark)}
.result.bad{background:#fbeeeb;border-color:rgba(201,79,61,.28);color:#a83a2b}

.session{margin-top:22px;padding-top:18px;border-top:1px dashed var(--line)}
.who{font-size:12.5px;color:var(--muted)}
.who.on{color:var(--primary-dark);font-weight:600}
.who-actions{display:flex;gap:10px;align-items:center;margin-top:10px}

.login-note{display:flex;gap:10px;margin-top:18px;padding:13px 14px;border:1px solid #dbeae5;
  border-radius:11px;background:#f4faf8;color:var(--ink-2);font-size:12.5px;line-height:1.65}
.login-note b{font-weight:700}

.users-block{margin-top:26px;padding-top:20px;border-top:1px solid var(--line)}
.users-head{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px;
  font-size:13px;font-weight:700;color:var(--ink)}
.badge{min-width:24px;padding:2px 8px;display:inline-grid;place-items:center;border-radius:999px;
  background:var(--primary-soft);color:var(--primary-dark);font-size:11.5px;font-weight:700}
.users{list-style:none;margin:0 0 10px;padding:0;display:flex;flex-direction:column;gap:8px;
  max-height:240px;overflow:auto}
.users li{display:flex;justify-content:space-between;gap:12px;padding:9px 12px;border-radius:9px;
  background:#f7f9f8;border:1px solid var(--line);font-size:13px;
  font-family:ui-monospace,Consolas,monospace}
.uid{color:var(--primary);flex:none}
.uname{color:var(--ink-2);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.empty{color:var(--muted);justify-content:center;text-align:center;border-style:dashed !important;background:transparent !important}

@media(max-width:1080px){
  .brand-panel{padding:44px 40px 34px}
  .brand-title{font-size:36px}
  .form-panel{padding:44px 32px}
}
@media(max-width:820px){
  .login-page{grid-template-columns:1fr}
  .brand-panel{min-height:340px}
  .brand-copy{margin-top:40px}
  .brand-features{display:none}
}
</style>
</head>
<body>
<body>
<div class="login-page">
  <section class="brand-panel">
    <div class="brand-top">
      <span class="logo-mark">↻</span>
      <span>流转二手</span>
    </div>
    <div class="brand-copy">
      <span class="eyebrow"><span class="dot"></span>TRUSTED SECOND-HAND MARKET</span>
      <h1 class="brand-title">让每一件闲置，<br>遇见<em>下一位主人</em>。</h1>
      <p class="brand-sub">校园与社区的可信二手交易平台。预约制交易、平台托管资金，
        从预约到收货，每一步都有记录。</p>
    </div>
    <ul class="brand-features">
      <li><span class="feature-tick">✓</span>关键词搜索 + 优先级推荐，快速找到心仪好物</li>
      <li><span class="feature-tick">✓</span>预约 · 确认 · 付款 · 收货全流程留痕，交易更安心</li>
      <li><span class="feature-tick">✓</span>平台托管货款，买家确认收货后再打款给卖家</li>
    </ul>
    <div class="brand-foot">
      <span>© 2026 流转二手</span>
      <span>UML 课程设计 · 界面演示</span>
    </div>
  </section>

  <section class="form-panel">
    <div class="login-card">
      <h1>欢迎回来</h1>
      <p class="login-welcome">登录账号，或创建新账号，继续发现值得流转的好物。</p>

      <nav class="tabs">
        <button type="button" class="tab active" data-panel="registerForm">注册</button>
        <button type="button" class="tab" data-panel="loginForm">登录</button>
      </nav>

      <form id="registerForm" class="panel active" autocomplete="off" novalidate>
        <div class="field">
          <label class="field-label" for="username">
            <span>用户名</span><span class="hint">唯一，4–20 位</span>
          </label>
          <div class="input-shell">
            <input id="username" name="username" type="text" maxlength="20"
                   placeholder="例如 alice_zhang" required>
          </div>
        </div>
        <div class="field">
          <label class="field-label" for="password">
            <span>密码</span><span class="hint">至少 8 位，需含字母和数字</span>
          </label>
          <div class="input-shell">
            <input id="password" name="password" type="password"
                   placeholder="例如 passw0rd" required>
          </div>
        </div>
        <div class="field">
          <label class="field-label" for="confirm"><span>确认密码</span></label>
          <div class="input-shell">
            <input id="confirm" type="password" placeholder="再输入一次" required>
          </div>
        </div>
        <label class="check">
          <input type="checkbox" id="skipValidation">
          <span>跳过格式校验（仅用于测试 Service 行为）</span>
        </label>
        <button class="btn btn-primary btn-block" type="submit" id="submitBtn">创建账号</button>
        <p id="registerResult" class="result" hidden></p>
      </form>

      <form id="loginForm" class="panel" autocomplete="off" novalidate>
        <div class="field">
          <label class="field-label" for="loginUsername">
            <span>用户名</span><span class="hint">注册时使用的用户名</span>
          </label>
          <div class="input-shell">
            <input id="loginUsername" type="text" placeholder="已注册的用户名" required>
          </div>
        </div>
        <div class="field">
          <label class="field-label" for="loginPassword"><span>密码</span></label>
          <div class="input-shell">
            <input id="loginPassword" type="password" placeholder="注册时设置的密码" required>
          </div>
        </div>
        <button class="btn btn-primary btn-block" type="submit" id="loginBtn">登录</button>
        <p id="loginResult" class="result" hidden></p>
      </form>

      <div class="session">
        <p id="who" class="who">当前登录：未登录</p>
        <div class="who-actions">
          <a id="enterProducts" class="btn btn-outline btn-sm" href="/products" hidden>进入商品 / 审核页 →</a>
          <button type="button" id="logoutBtn" class="btn btn-ghost btn-sm" hidden>退出登录</button>
        </div>
      </div>

      <div class="login-note">
        <span><b>账号只保存在内存里</b>，关闭程序即清空；管理员账号由平台预置，不参与买卖。</span>
      </div>

      <div class="users-block">
        <div class="users-head">
          <span>本次会话已注册</span>
          <span id="count" class="badge">0</span>
        </div>
        <ul id="users" class="users"><li class="empty">还没有注册账号</li></ul>
        <p class="hint">注册成功后，切到「登录」页即可用同一账号测试登录。</p>
      </div>
    </div>
  </section>
</div>
<script>
const ERROR_NAMES = {
  0: 'Ok', 1: 'InvalidArg', 2: 'Unauthorized', 3: 'Forbidden',
  4: 'NotFound', 5: 'InvalidState', 6: 'VersionConflict', 7: 'OutOfStock',
  8: 'DuplicateReservation', 9: 'Locked', 10: 'LimitExceeded', 11: 'Expired'
};

const registerForm = document.getElementById('registerForm');
const loginForm = document.getElementById('loginForm');
const usersEl = document.getElementById('users');
const countEl = document.getElementById('count');
const submitBtn = document.getElementById('submitBtn');
const loginBtn = document.getElementById('loginBtn');
const whoEl = document.getElementById('who');
const logoutBtn = document.getElementById('logoutBtn');
const enterProducts = document.getElementById('enterProducts');

function roleName(role) {
  if (role === 4) return '管理员';
  const parts = [];
  if (role & 1) parts.push('买家');
  if (role & 2) parts.push('卖家');
  return parts.length ? parts.join(' + ') : '未知角色';
}

// 登录状态由服务端会话 Cookie 决定，页面只负责显示
async function refreshMe() {
  try {
    const response = await fetch('/api/me', { cache: 'no-store' });
    const me = await response.json();
    if (me.ok) {
      whoEl.textContent = '当前登录：#' + me.id + ' ' + me.username + ' · ' + roleName(me.role);
      whoEl.classList.add('on');
      logoutBtn.hidden = false;
      enterProducts.hidden = false;
    } else {
      whoEl.textContent = '当前登录：未登录';
      whoEl.classList.remove('on');
      logoutBtn.hidden = true;
      enterProducts.hidden = true;
    }
  } catch (error) {
    whoEl.textContent = '当前登录：无法连接服务';
    whoEl.classList.remove('on');
  }
}

logoutBtn.addEventListener('click', async () => {
  try {
    await fetch('/api/logout', { method: 'POST' });
  } catch (error) {
    // 忽略网络错误，下面统一刷新状态
  }
  await refreshMe();
});

// Service 层的 msg 统一映射成中文提示，同时保留原始文案便于调试
const FRIENDLY = {
  'username taken': '该用户名已被注册（username taken）',
  'invalid username or password': '用户名或密码错误（invalid username or password）'
};

function show(el, kind, text) {
  el.hidden = false;
  el.className = 'result ' + kind;
  el.textContent = text;
}

function errorName(code) { return ERROR_NAMES[code] || code; }

function friendly(message) { return FRIENDLY[message] || message; }

function validate(username, password, confirm) {
  const len = Array.from(username).length;
  if (len < 4 || len > 20) return '用户名需为 4–20 位';
  if (Array.from(password).length < 8) return '密码至少 8 位';
  if (!/[A-Za-z]/.test(password) || !/[0-9]/.test(password)) return '密码需同时包含字母和数字';
  if (password !== confirm) return '两次输入的密码不一致';
  return '';
}

async function refreshUsers() {
  try {
    const response = await fetch('/api/users', { cache: 'no-store' });
    const list = await response.json();
    countEl.textContent = list.length;
    usersEl.innerHTML = '';
    if (!list.length) {
      usersEl.innerHTML = '<li class="empty">还没有注册账号</li>';
      return;
    }
    for (const user of list) {
      const li = document.createElement('li');
      const id = document.createElement('span');
      id.className = 'uid';
      id.textContent = '#' + user.id;
      const name = document.createElement('span');
      name.className = 'uname';
      name.textContent = user.username;
      li.append(id, name);
      usersEl.appendChild(li);
    }
  } catch (error) {
    usersEl.innerHTML = '<li class="empty">无法连接服务</li>';
  }
}

// 注册 / 登录 标签页切换
document.querySelectorAll('.tab').forEach((tab) => {
  tab.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t === tab));
    document.querySelectorAll('.panel').forEach((panel) =>
      panel.classList.toggle('active', panel.id === tab.dataset.panel));
  });
});

// 支持直接打开 http://127.0.0.1:8080/#login
if (location.hash === '#login') document.querySelector('.tab[data-panel="loginForm"]').click();

registerForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  const resultEl = document.getElementById('registerResult');
  const username = document.getElementById('username').value.trim();
  const password = document.getElementById('password').value;
  const confirm = document.getElementById('confirm').value;
  const skipValidation = document.getElementById('skipValidation').checked;

  if (!username || !password) { show(resultEl, 'bad', '请填写用户名和密码'); return; }

  if (!skipValidation) {
    const problem = validate(username, password, confirm);
    if (problem) { show(resultEl, 'bad', '✗ ' + problem); return; }
  }

  submitBtn.disabled = true;
  try {
    const response = await fetch('/api/register', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username, password, skipValidation })
    });
    const data = await response.json();
    if (data.ok) {
      show(resultEl, 'good', '✓ 注册成功：' + data.username + '（id=' + data.id + '）');
      document.getElementById('password').value = '';
      document.getElementById('confirm').value = '';
      document.getElementById('loginUsername').value = username;  // 方便接着测登录
      await refreshUsers();
    } else {
      show(resultEl, 'bad', '✗ ' + friendly(data.msg) + '  [Err::' + errorName(data.code) + ']');
    }
  } catch (error) {
    show(resultEl, 'bad', '✗ 请求失败：无法连接本地服务');
  } finally {
    submitBtn.disabled = false;
  }
});

loginForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  const resultEl = document.getElementById('loginResult');
  const username = document.getElementById('loginUsername').value.trim();
  const password = document.getElementById('loginPassword').value;

  if (!username || !password) { show(resultEl, 'bad', '请填写用户名和密码'); return; }

  loginBtn.disabled = true;
  try {
    const response = await fetch('/api/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username, password })
    });
    const data = await response.json();
    if (data.ok) {
      show(resultEl, 'good', '✓ 登录成功：' + data.username + '（id=' + data.id + '），身份已写入会话');
      await refreshMe();
    } else {
      // 登录失败统一提示，不区分用户不存在 / 密码错误（REQ-027）
      show(resultEl, 'bad', '✗ ' + friendly(data.msg) + '  [Err::' + errorName(data.code) + ']');
      await refreshMe();
    }
  } catch (error) {
    show(resultEl, 'bad', '✗ 请求失败：无法连接本地服务');
  } finally {
    loginBtn.disabled = false;
  }
});

refreshUsers();
refreshMe();
</script>
</body>
</html>)HTML";
}

std::string WebUI::productPage() const {
    return R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>商品 / 审核测试 · 二手交易平台</title>
<style>
:root{
  --bg:#f3f5f4; --surface:#ffffff; --ink:#17211f; --ink-2:#3d4a47; --muted:#788683;
  --line:#e6ebe9; --line-strong:#d2dbd8; --primary:#11685a; --primary-dark:#0c5247;
  --primary-soft:#e7f2ef; --accent:#e2892f; --accent-soft:#fdf2e4; --price:#c94f3d;
  --radius:14px;
  --shadow-sm:0 1px 2px rgba(16,35,30,.05),0 1px 3px rgba(16,35,30,.06);
  --shadow-md:0 8px 24px rgba(16,35,30,.09);
  font-family:"PingFang SC","Microsoft YaHei","Segoe UI",system-ui,-apple-system,sans-serif;
}
*{box-sizing:border-box}
[hidden]{display:none !important}
html,body{margin:0;padding:0;background:var(--bg);color:var(--ink);font-size:14px;line-height:1.5;-webkit-font-smoothing:antialiased}
h1,h2,h3,p{margin:0}
a{color:inherit;text-decoration:none}
button{font-family:inherit;cursor:pointer}

.btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;height:40px;padding:0 18px;
     border:1px solid transparent;border-radius:10px;background:none;color:var(--ink-2);
     font-size:14px;font-weight:600;cursor:pointer;transition:background .15s,border-color .15s,color .15s,box-shadow .15s}
.btn-primary{background:var(--primary);color:#fff;box-shadow:0 6px 14px rgba(17,104,90,.18)}
.btn-primary:hover{background:var(--primary-dark)}
.btn-outline{background:#fff;border-color:var(--line-strong);color:var(--ink-2)}
.btn-outline:hover{border-color:var(--primary);color:var(--primary)}
.btn-ghost{background:transparent;color:var(--muted)}
.btn-ghost:hover{background:#f0f3f2;color:var(--ink-2)}
.btn-sm{height:34px;padding:0 14px;font-size:13px;border-radius:9px}
.btn-block{width:100%}
.btn:disabled{opacity:.55;cursor:wait}

.site-header{position:sticky;top:0;z-index:20;background:rgba(255,255,255,.94);
  border-bottom:1px solid var(--line);backdrop-filter:blur(10px)}
.header-inner{max-width:1160px;height:68px;margin:0 auto;padding:0 24px;display:flex;align-items:center;gap:18px}
.logo{display:flex;align-items:center;gap:10px;font-size:19px;font-weight:800;letter-spacing:.5px;white-space:nowrap}
.logo-mark{width:36px;height:36px;border-radius:11px;display:grid;place-items:center;color:#fff;
  background:linear-gradient(140deg,#17806d,#0d574b);box-shadow:0 4px 10px rgba(17,104,90,.22);
  font-size:18px;font-weight:700}
.logo-sub{font-size:12px;font-weight:500;color:var(--muted);letter-spacing:0}
.header-actions{margin-left:auto;display:flex;align-items:center;gap:10px;min-width:0}
.user-menu{display:flex;align-items:center;gap:9px;min-width:0;padding:3px 8px 3px 4px;border-radius:999px}
.user-menu:hover{background:#f1f4f3}
.avatar{flex:none;width:32px;height:32px;border-radius:50%;display:grid;place-items:center;
  background:linear-gradient(145deg,#cfe4de,#a9cec4);color:var(--primary-dark);
  font-size:13px;font-weight:700;border:2px solid #fff;box-shadow:var(--shadow-sm)}
.who{font-size:12.5px;color:var(--muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.who.on{color:var(--ink-2);font-weight:600}

.page{max-width:1160px;margin:0 auto;padding:22px 24px 64px}
.crumb{display:flex;align-items:center;gap:8px;margin-bottom:14px;font-size:12.5px;color:var(--muted)}
.crumb .sep{color:#b6c0bd}
.crumb .current{color:var(--ink-2);font-weight:600}

.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--radius);
  padding:22px;box-shadow:var(--shadow-sm)}
.card h2{font-size:16px;font-weight:800;letter-spacing:.2px;margin:0 0 6px;display:flex;align-items:center;gap:8px}
.card h2 .step{display:inline-grid;place-items:center;width:22px;height:22px;border-radius:7px;
  background:var(--primary-soft);color:var(--primary-dark);font-size:11.5px;font-weight:800}
.card h2 + .role-note,.card h2 + .hint{margin-top:4px}
.hint{color:var(--muted);font-size:12.5px;line-height:1.6}
.role-note{color:var(--muted);font-size:12.5px;line-height:1.6;margin:0 0 12px}
.section-gap{margin-top:22px;padding-top:20px;border-top:1px dashed var(--line)}

label.field{display:block;font-size:12.5px;color:var(--muted);margin-bottom:12px}
label.field input,label.field select{display:block;width:100%;margin-top:6px;height:42px;padding:0 12px;
  border-radius:10px;border:1px solid var(--line-strong);background:#fff;color:var(--ink);
  font:inherit;font-size:13.5px;outline:none;transition:border-color .15s,box-shadow .15s}
label.field input:focus,label.field select:focus{border-color:var(--primary);box-shadow:0 0 0 3px rgba(17,104,90,.10)}
label.field input::placeholder{color:#a8b2af}
.row{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px}
.form-btn{margin-top:4px}
.card form > .btn{margin-top:6px}
input[type=number]{appearance:textfield}

.result{margin:10px 0 0;padding:9px 12px;border-radius:9px;font-size:13px;line-height:1.5;word-break:break-word;
  background:#f4faf8;border:1px solid #dbeae5;color:var(--ink-2)}
.result.good{background:var(--primary-soft);border-color:rgba(17,104,90,.28);color:var(--primary-dark)}
.result.bad{background:#fbeeeb;border-color:rgba(201,79,61,.28);color:#a83a2b}

.tag{display:inline-block;padding:1px 8px;border-radius:999px;font-size:11px;font-weight:600;
  background:rgba(17,104,90,.10);color:var(--primary-dark);border:1px solid rgba(17,104,90,.22)}
.tag.published{background:var(--primary-soft);color:var(--primary-dark);border-color:rgba(17,104,90,.30)}
.tag.rejected{background:#fbeeeb;color:#a83a2b;border-color:rgba(201,79,61,.28)}
.tag.offshelf{background:#eef1f0;color:#6c7976;border-color:var(--line-strong)}
.tag.cancelled{background:#f6eeea;color:#9c6a52;border-color:#e6d5ca}
.tag.soldout{background:var(--accent-soft);color:#a8641a;border-color:rgba(226,137,47,.35)}

.list{list-style:none;margin:10px 0 0;padding:0;display:flex;flex-direction:column;gap:8px;
  max-height:280px;overflow:auto}
.list li{padding:9px 12px;border-radius:10px;background:#f7f9f8;border:1px solid var(--line);
  font-size:12px;font-family:ui-monospace,Consolas,monospace;line-height:1.6;color:var(--ink-2)}
.list .empty{color:var(--muted);text-align:center;border-style:dashed;background:transparent;font-family:inherit}
.list-head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:6px}
.list-head h2{margin:0}

.search-panel{margin-bottom:22px}
.search-head{display:flex;align-items:flex-end;justify-content:space-between;gap:18px;flex-wrap:wrap}
.search-head h2{font-size:22px;letter-spacing:.3px}
.search-meta{margin-top:6px;font-size:12.5px;color:var(--muted)}
.search-meta b{color:var(--primary-dark);font-weight:700}
.search-form{display:flex;flex-wrap:wrap;align-items:flex-end;gap:12px;margin-top:18px;
  padding:14px;border-radius:12px;background:#f6f8f7;border:1px solid var(--line)}
.search-form .field-inline{display:flex;flex-direction:column;gap:6px;font-size:12px;color:var(--muted)}
.search-form .field-inline input{height:42px;padding:0 13px;border-radius:10px;border:1px solid var(--line-strong);
  background:#fff;color:var(--ink);font:inherit;font-size:13.5px;outline:none;transition:border-color .15s,box-shadow .15s}
.search-form .field-inline input:focus{border-color:var(--primary);box-shadow:0 0 0 3px rgba(17,104,90,.10)}
.search-form .grow{flex:1 1 260px}
.search-form .narrow{flex:0 1 130px}
.search-form .btn{height:42px}
.search-result{margin-top:12px}

.product-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:18px;margin-top:18px}
.product-grid .empty{grid-column:1/-1;padding:26px;border:1px dashed var(--line-strong);border-radius:var(--radius);
  color:var(--muted);text-align:center;font-size:13px}
.product-card{overflow:hidden;border:1px solid var(--line);border-radius:var(--radius);background:#fff;
  transition:transform .18s,box-shadow .18s,border-color .18s}
.product-card:hover{transform:translateY(-3px);border-color:#d7e2de;box-shadow:var(--shadow-md)}
.thumb{position:relative;aspect-ratio:4/3;display:grid;place-items:center;font-size:62px;overflow:hidden}
.thumb::after{content:"";position:absolute;right:-24%;bottom:-34%;width:78%;height:90%;border-radius:50%;
  background:rgba(255,255,255,.38);filter:blur(2px)}
.thumb .emoji{position:relative;z-index:1;filter:drop-shadow(0 8px 12px rgba(20,40,34,.16))}
.grad-1{background:linear-gradient(140deg,#e8eef7 0%,#d3dfef 100%)}
.grad-2{background:linear-gradient(140deg,#f5ece1 0%,#ecd9c2 100%)}
.grad-3{background:linear-gradient(140deg,#e6f1ec 0%,#cfe5dc 100%)}
.grad-4{background:linear-gradient(140deg,#f3eaf0 0%,#e5d3df 100%)}
.grad-5{background:linear-gradient(140deg,#eef0e4 0%,#dde3c8 100%)}
.grad-6{background:linear-gradient(140deg,#f1eaea 0%,#e3cfcf 100%)}
.cond-badge{position:absolute;top:10px;left:10px;z-index:2;padding:3px 9px;border-radius:7px;
  background:rgba(255,255,255,.92);color:var(--ink-2);font-size:11px;font-weight:600;backdrop-filter:blur(4px)}
.soldout-badge{position:absolute;top:10px;right:10px;z-index:2;padding:3px 9px;border-radius:7px;
  background:rgba(23,33,31,.78);color:#fff;font-size:11px;font-weight:700;letter-spacing:.4px}
.product-body{padding:14px 16px 15px}
.product-name{display:-webkit-box;height:42px;overflow:hidden;font-size:14px;font-weight:600;line-height:1.5;
  -webkit-line-clamp:2;-webkit-box-orient:vertical}
.product-price-row{display:flex;align-items:baseline;gap:8px;margin-top:9px}
.price{color:var(--price);font-size:19px;font-weight:800;letter-spacing:.2px}
.price .sym{margin-right:1px;font-size:13px;font-weight:700}
.stock{font-size:11.5px;color:var(--muted)}
.product-meta{margin-top:6px;font-size:11.5px;color:var(--muted)}
.product-foot{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:12px;
  padding-top:12px;border-top:1px dashed var(--line)}
.score-pill{display:inline-flex;align-items:center;gap:5px;padding:3px 9px;border-radius:999px;
  background:var(--primary-soft);color:var(--primary-dark);font-size:11.5px;font-weight:700}

.workbench{display:grid;grid-template-columns:minmax(0,1.12fr) minmax(0,.88fr);gap:20px;align-items:start}
.stack{display:flex;flex-direction:column;gap:20px}
.gate-card{max-width:520px;margin:40px auto;text-align:center}
.gate-card .btn{margin-top:16px}
.escrow-line{display:flex;align-items:center;justify-content:space-between;gap:12px;
  padding:12px 14px;border-radius:11px;background:var(--primary-soft);color:var(--primary-dark);
  font-size:13px;font-weight:700}
.escrow-line span{font-size:12px;font-weight:600;opacity:.8}

@media(max-width:1080px){
  .product-grid{grid-template-columns:repeat(2,minmax(0,1fr))}
  .workbench{grid-template-columns:1fr}
}
@media(max-width:820px){
  .product-grid{grid-template-columns:1fr}
  .header-inner{gap:12px;padding:0 16px}
  .logo-sub{display:none}
  .who{display:none}
  .page{padding:18px 16px 48px}
  .row,.row3{grid-template-columns:1fr}
}
</style>
</head>
<body>
<body>
<header class="site-header">
  <div class="header-inner">
    <a class="logo" href="/products">
      <span class="logo-mark">↻</span>
      <span>流转二手</span>
    </a>
    <span class="logo-sub">商品 · 审核 · 交易工作台</span>
    <div class="header-actions">
      <a class="btn btn-ghost btn-sm" href="/">注册 / 登录</a>
      <div class="user-menu">
        <span class="avatar">二</span>
        <span id="who" class="who">未登录</span>
      </div>
      <button type="button" id="logoutBtn" class="btn btn-outline btn-sm" hidden>退出登录</button>
    </div>
  </div>
</header>

<main class="page">
  <nav class="crumb">
    <span>首页</span><span class="sep">/</span>
    <span>商品</span><span class="sep">/</span>
    <span class="current">搜索与交易工作台</span>
  </nav>

  <section id="gate" class="card gate-card" hidden>
    <h2>尚未登录</h2>
    <p class="hint">商品、搜索与交易功能需要登录后使用，身份由服务端会话自动识别。</p>
    <a class="btn btn-primary" href="/#login">去登录 →</a>
  </section>

  <section id="app" hidden>
    <section class="card search-panel" data-role="query" hidden>
      <div class="search-head">
        <div>
          <h2>发现值得流转的好物</h2>
          <p class="search-meta">综合排序权重：<b>匹配度 80%</b> · <b>近 30 天销量 20%</b>
            · 售罄商品仍可被搜索到</p>
        </div>
      </div>
      <form id="searchForm" class="search-form">
        <label class="field-inline grow">关键词
          <input id="searchKeyword" placeholder="标题 / 描述，可空">
        </label>
        <label class="field-inline narrow">分类
          <input id="searchCategory" placeholder="精确匹配">
        </label>
        <label class="field-inline narrow">最低价
          <input id="searchMinPrice" type="number" step="0.01" min="0" placeholder="可空">
        </label>
        <label class="field-inline narrow">最高价
          <input id="searchMaxPrice" type="number" step="0.01" min="0" placeholder="可空">
        </label>
        <button class="btn btn-primary" type="submit">搜索</button>
      </form>
      <p id="searchResult" class="result search-result" hidden></p>
      <div id="searchList" class="product-grid">
        <p class="empty">输入关键词或筛选条件，开始搜索</p>
      </div>
    </section>

    <div class="workbench">
      <div class="stack">
        <section class="card">
          <h2>测试准备</h2>
          <p class="role-note">系统预置管理员账号，不开放自助注册；登录后身份自动取自会话，无需再填 id。</p>
          <form id="accountForm">
            <div class="row3">
              <label class="field">用户名<input id="accName" value="seller01"></label>
              <label class="field">密码<input id="accPass" value="passw0rd"></label>
              <label class="field">角色
                <select id="accRole">
                  <option value="2">卖家</option>
                  <option value="1">买家</option>
                  <option value="3">买家 + 卖家</option>
                  <option value="4">管理员</option>
                </select>
              </label>
            </div>
            <button class="btn btn-primary" type="submit">创建账号</button>
          </form>
          <p id="accResult" class="result" hidden></p>

          <div class="section-gap">
            <form id="balanceForm">
              <label class="field">账户充值（给当前登录账号增加可用余额）
                <input id="balanceAmount" type="number" min="0.01" step="0.01" value="1000">
              </label>
              <button class="btn btn-outline" type="submit">充值</button>
            </form>
            <p id="balanceResult" class="result" hidden></p>
          </div>
        </section>

        <section class="card" data-role="seller">
          <h2><span class="step">1</span>卖家 · 商品与发布</h2>
          <p class="role-note">仅草稿与已下架商品可编辑，已发布商品不能直接修改。</p>
          <form id="createForm">
            <button class="btn btn-primary" type="submit">新建商品草稿</button>
          </form>
          <p id="createResult" class="result" hidden></p>

          <div class="section-gap">
            <form id="draftForm">
              <label class="field">商品 id<input id="draftProductId" type="number" min="1" required></label>
              <label class="field">标题<input id="draftTitle" value="九成新机械键盘"></label>
              <label class="field">描述<input id="draftDesc" value="自用一年，手感良好"></label>
              <label class="field">分类<input id="draftCategory" value="数码"></label>
              <div class="row">
                <label class="field">价格<input id="draftPrice" type="number" step="0.01" value="199"></label>
                <label class="field">余量<input id="draftRemaining" type="number" value="1"></label>
              </div>
              <button class="btn btn-primary" type="submit">保存草稿</button>
            </form>
            <p id="draftResult" class="result" hidden></p>
          </div>

          <div class="section-gap">
            <form id="submitForm">
              <label class="field">商品 id<input id="submitProductId" type="number" min="1" required></label>
              <button class="btn btn-outline" type="submit">提交审核</button>
            </form>
            <p id="submitResult" class="result" hidden></p>
          </div>
        </section>

        <section class="card" data-role="admin">
          <h2><span class="step">2</span>管理员 · 审核</h2>
          <p class="role-note">审核期间商品不可见、不可预约；驳回必须填写理由。</p>
          <form id="reviewForm">
            <label class="field">审核申请 id<input id="reviewId" type="number" min="1" required></label>
            <label class="field">结论
              <select id="approval">
                <option value="true">通过</option>
                <option value="false">驳回</option>
              </select>
            </label>
            <label class="field">驳回理由<input id="reason" value="图片与描述不符"></label>
            <button class="btn btn-primary" type="submit">提交审核结论</button>
          </form>
          <p id="reviewResult" class="result" hidden></p>

          <div class="section-gap">
            <h2>退货裁决</h2>
            <p class="role-note">卖家不同意退货时由管理员裁决，驳回必须填写理由。</p>
            <form id="returnReviewForm">
              <label class="field">退货申请 id<input id="returnReviewId" type="number" min="1" required></label>
              <label class="field">结论
                <select id="returnApproval">
                  <option value="true">通过（可退货）</option>
                  <option value="false">驳回（交易维持）</option>
                </select>
              </label>
              <label class="field">审核意见<input id="returnReviewReason" value="经核实同意退货"></label>
              <button class="btn btn-primary" type="submit">提交退货结论</button>
            </form>
            <p id="returnReviewResult" class="result" hidden></p>
          </div>
        </section>

        <section class="card" data-role="manage">
          <h2>下架商品</h2>
          <p class="role-note">卖家只能下架自己的商品，管理员可下架任意商品；下架后进行中的预约会被终止并归还余量。</p>
          <form id="removeForm">
            <label class="field">商品 id<input id="removeProductId" type="number" min="1" required></label>
            <button class="btn btn-outline" type="submit">下架商品</button>
          </form>
          <p id="removeResult" class="result" hidden></p>
        </section>

        <section class="card" data-role="seller">
          <h2><span class="step">3</span>卖家 · 履约</h2>
          <p class="role-note">确认预约时可调整成交价；买家付款后发货；收到退货后确认退款。</p>
          <form id="confirmForm">
            <div class="row">
              <label class="field">预约 id<input id="confirmReservationId" type="number" min="1" required></label>
              <label class="field">新价格（-1 不修改）<input id="confirmPrice" type="number" step="0.01" value="-1"></label>
            </div>
            <button class="btn btn-primary" type="submit">确认预约</button>
          </form>
          <p id="confirmResult" class="result" hidden></p>

          <div class="section-gap">
            <form id="shipForm">
              <label class="field">订单 id<input id="shipOrderId" type="number" min="1" required></label>
              <button class="btn btn-outline" type="submit">发货</button>
            </form>
            <p id="shipResult" class="result" hidden></p>
          </div>

          <div class="section-gap">
            <form id="returnReceivedForm">
              <label class="field">退货申请 id<input id="returnReceivedId" type="number" min="1" required></label>
              <button class="btn btn-outline" type="submit">确认收到退货</button>
            </form>
            <p id="returnReceivedResult" class="result" hidden></p>
          </div>
        </section>
      </div>

      <div class="stack">
        <section class="card" data-role="buyer">
          <h2><span class="step">4</span>买家 · 预约与收货</h2>
          <p class="role-note">预约扣减余量，取消预约恢复余量；确认后 2 小时内可付款，确认收货后托管货款转给卖家。</p>
          <form id="reserveForm">
            <div class="row">
              <label class="field">商品 id<input id="reserveProductId" type="number" min="1" required></label>
              <label class="field">数量<input id="reserveAmount" type="number" min="1" value="1"></label>
            </div>
            <button class="btn btn-primary" type="submit">预约</button>
          </form>
          <p id="reserveResult" class="result" hidden></p>

          <div class="section-gap">
            <form id="cancelForm">
              <label class="field">预约 id<input id="cancelReservationId" type="number" min="1" required></label>
              <button class="btn btn-outline" type="submit">取消预约</button>
            </form>
            <p id="cancelResult" class="result" hidden></p>
          </div>

          <div class="section-gap">
            <form id="buyForm">
              <label class="field">预约 id<input id="buyReservationId" type="number" min="1" required></label>
              <button class="btn btn-primary" type="submit">付款（平台托管）</button>
            </form>
            <p id="buyResult" class="result" hidden></p>
          </div>

          <div class="section-gap">
            <form id="receiptForm">
              <label class="field">订单 id<input id="receiptOrderId" type="number" min="1" required></label>
              <button class="btn btn-outline" type="submit">确认收货</button>
            </form>
            <p id="receiptResult" class="result" hidden></p>
          </div>

          <div class="section-gap">
            <h2>退货流程</h2>
            <p class="role-note">申请 → 管理员审核 → 7 天内寄回并填单号 → 卖家确认收到 → 退款。</p>
            <form id="returnApplyForm">
              <label class="field">订单 id<input id="returnOrderId" type="number" min="1" required></label>
              <label class="field">退货理由<input id="returnReason" value="商品与描述不符"></label>
              <button class="btn btn-outline" type="submit">申请退货</button>
            </form>
            <p id="returnApplyResult" class="result" hidden></p>

            <form id="returnDeliveredForm" style="margin-top:14px">
              <div class="row">
                <label class="field">退货申请 id<input id="returnDeliveredId" type="number" min="1" required></label>
                <label class="field">快递单号<input id="returnTrackingNo" value="SF1234567890"></label>
              </div>
              <button class="btn btn-outline" type="submit">提交快递单号</button>
            </form>
            <p id="returnDeliveredResult" class="result" hidden></p>
          </div>
        </section>

        <aside class="card">
          <div class="escrow-line">
            <span id="escrowLine">平台托管：¥0.00</span>
            <button type="button" class="btn btn-outline btn-sm" id="refreshBtn">刷新</button>
          </div>

          <div class="list-head"><h2>商品</h2></div>
          <ul id="productList" class="list"><li class="empty">暂无商品</li></ul>

          <div data-role="admin">
            <div class="list-head"><h2>审核申请</h2></div>
            <ul id="reviewList" class="list"><li class="empty">暂无申请</li></ul>
          </div>

          <div class="list-head"><h2>预约</h2></div>
          <ul id="reservationList" class="list"><li class="empty">暂无预约</li></ul>

          <div class="list-head"><h2>订单</h2></div>
          <ul id="orderList" class="list"><li class="empty">暂无订单</li></ul>

          <div class="list-head"><h2>退货申请</h2></div>
          <ul id="returnList" class="list"><li class="empty">暂无退货申请</li></ul>
        </aside>
      </div>
    </div>
  </section>
</main>
<script>
const ERROR_NAMES = {
  0: 'Ok', 1: 'InvalidArg', 2: 'Unauthorized', 3: 'Forbidden',
  4: 'NotFound', 5: 'InvalidState', 6: 'VersionConflict', 7: 'OutOfStock',
  8: 'DuplicateReservation', 9: 'Locked', 10: 'LimitExceeded', 11: 'Expired'
};
const PRODUCT_STATUS = ['Draft', 'PendingReview', 'Published', 'OffShelf', 'Hidden'];
const REVIEW_STATUS = ['Pending', 'Approved', 'Rejected'];
const REVIEW_TYPE = ['Create', 'Modify'];
const RESERVATION_STATUS = ['Pending', 'Confirmed', 'Completed', 'Cancelled'];
const ORDER_STATUS = ['PendingPayment', 'Paid', 'Shipped', 'Completed', 'Closed', 'Returned'];
const RETURN_STATUS = ['Pending', 'Approved', 'Shipped', 'Delivered', 'Received', 'Refunded',
                       'Rejected', 'Closed'];

let me = null;

function roleName(role) {
  if (role === 4) return '管理员';
  const parts = [];
  if (role & 1) parts.push('买家');
  if (role & 2) parts.push('卖家');
  return parts.length ? parts.join(' + ') : '未知角色';
}

function show(el, kind, text) {
  el.hidden = false;
  el.className = 'result ' + kind;
  el.textContent = text;
}

async function post(path, payload) {
  const response = await fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  });
  return response.json();
}

async function loadMe() {
  try {
    const data = await fetch('/api/me', { cache: 'no-store' }).then((r) => r.json());
    return data.ok ? data : null;
  } catch (error) {
    return null;
  }
}

function describe(data) {
  if (data.ok) return '✓ ok';
  return '✗ ' + data.msg + '  [Err::' + (ERROR_NAMES[data.code] || data.code) + ']';
}

function val(id) { return document.getElementById(id).value; }
function num(id) { return Number(document.getElementById(id).value); }

function renderList(el, items, toLine) {
  el.innerHTML = '';
  if (!items.length) {
    const li = document.createElement('li');
    li.className = 'empty';
    li.textContent = '暂无数据';
    el.appendChild(li);
    return;
  }
  for (const item of items) {
    const li = document.createElement('li');
    for (const part of toLine(item)) {
      if (typeof part === 'string') {
        li.appendChild(document.createTextNode(part));
      } else {
        const tag = document.createElement('span');
        tag.className = 'tag ' + part.kind;
        tag.textContent = part.text;
        li.appendChild(tag);
      }
    }
    el.appendChild(li);
  }
}

const GRADS = ['grad-1', 'grad-2', 'grad-3', 'grad-4', 'grad-5', 'grad-6'];

function emojiFor(category) {
  const map = [['数码', '⌨️'], ['键盘', '⌨️'], ['鼠标', '🖱️'], ['图书', '📚'],
               ['教材', '📚'], ['家居', '🛋️'], ['家具', '🪑'], ['运动', '🏀'],
               ['户外', '🏕️'], ['服饰', '👟'], ['鞋', '👟'], ['配件', '🎧']];
  for (const [key, emoji] of map) {
    if (category && category.includes(key)) return emoji;
  }
  return '📦';
}

function renderSearch(el, items) {
  el.innerHTML = '';
  if (!items.length) {
    const empty = document.createElement('p');
    empty.className = 'empty';
    empty.textContent = '没有找到符合条件的商品，换个关键词试试';
    el.appendChild(empty);
    return;
  }
  items.forEach((it, index) => {
    const card = document.createElement('article');
    card.className = 'product-card';

    const thumb = document.createElement('div');
    thumb.className = 'thumb ' + GRADS[index % GRADS.length];
    const badge = document.createElement('span');
    badge.className = 'cond-badge';
    badge.textContent = it.category || '二手好物';
    const emoji = document.createElement('span');
    emoji.className = 'emoji';
    emoji.textContent = emojiFor(it.category);
    thumb.append(badge, emoji);
    if (it.soldOut) {
      const sold = document.createElement('span');
      sold.className = 'soldout-badge';
      sold.textContent = '已售罄';
      thumb.appendChild(sold);
    }

    const body = document.createElement('div');
    body.className = 'product-body';

    const name = document.createElement('h3');
    name.className = 'product-name';
    name.textContent = it.title || '(无标题)';

    const priceRow = document.createElement('div');
    priceRow.className = 'product-price-row';
    const price = document.createElement('span');
    price.className = 'price';
    const sym = document.createElement('span');
    sym.className = 'sym';
    sym.textContent = '¥';
    price.append(sym, document.createTextNode(Number(it.price).toFixed(2)));
    const stock = document.createElement('span');
    stock.className = 'stock';
    stock.textContent = '剩 ' + it.remaining + ' 件';
    priceRow.append(price, stock);

    const meta = document.createElement('div');
    meta.className = 'product-meta';
    meta.textContent = '匹配 ' + Number(it.matchScore).toFixed(0)
      + ' · 近 30 天销量 ' + it.sales30d + ' 件';

    const foot = document.createElement('div');
    foot.className = 'product-foot';
    const score = document.createElement('span');
    score.className = 'score-pill';
    score.textContent = '综合 ' + Number(it.totalScore).toFixed(1);
    const reserve = document.createElement('button');
    reserve.type = 'button';
    reserve.className = 'btn btn-outline btn-sm';
    reserve.textContent = it.soldOut ? '已售罄' : '预约';
    reserve.disabled = it.soldOut;
    reserve.addEventListener('click', () => {
      document.getElementById('reserveProductId').value = it.productId;
      document.getElementById('reserveForm').scrollIntoView({ behavior: 'smooth', block: 'center' });
    });
    foot.append(score, reserve);

    body.append(name, priceRow, meta, foot);
    card.append(thumb, body);
    el.appendChild(card);
  });
}

async function refresh() {
  if (!me) return;
  try {
    const products = await fetch('/api/products', { cache: 'no-store' }).then((r) => r.json());
    renderList(document.getElementById('productList'), products, (p) => [
      '#', String(p.id), ' 卖家 ', String(p.sellerId), ' ', p.title || '(无标题)',
      ' ¥', String(p.price), ' 余量 ', String(p.remaining), ' ',
      { text: PRODUCT_STATUS[p.status] || p.status,
        kind: p.status === 2 ? 'published' : p.status === 3 ? 'offshelf' : '' }
    ]);
    const escrow = await fetch('/api/escrow', { cache: 'no-store' }).then((r) => r.json());
    document.getElementById('escrowLine').textContent =
      '平台托管：¥' + Number(escrow.balance || 0).toFixed(2);
    const reservations = await fetch('/api/reservations', { cache: 'no-store' }).then((r) => r.json());
    renderList(document.getElementById('reservationList'), reservations, (r) => [
      '#', String(r.id), ' 商品 ', String(r.commodityId), ' 买家 ', String(r.purchaserId),
      ' ×', String(r.amount), ' ¥', String(r.commodityPrice), ' ',
      { text: RESERVATION_STATUS[r.status] || r.status, kind: r.status === 3 ? 'cancelled' : '' }
    ]);
    const orders = await fetch('/api/orders', { cache: 'no-store' }).then((r) => r.json());
    renderList(document.getElementById('orderList'), orders, (o) => [
      '#', String(o.id), ' 商品 ', String(o.commodityId), ' 买家 ', String(o.buyerId),
      ' 卖家 ', String(o.sellerId), ' ×', String(o.amount), ' ¥', String(o.totalPrice), ' ',
      { text: ORDER_STATUS[o.status] || o.status,
        kind: o.status === 3 ? 'published' : o.status === 4 || o.status === 5 ? 'cancelled' : '' }
    ]);
    const returnRequests = await fetch('/api/returns', { cache: 'no-store' }).then((r) => r.json());
    renderList(document.getElementById('returnList'), returnRequests, (r) => [
      '#', String(r.id), ' 订单 ', String(r.orderId), ' 买家 ', String(r.buyerId), ' ',
      { text: RETURN_STATUS[r.status] || r.status,
        kind: r.status === 5 ? 'published' : r.status === 6 || r.status === 7 ? 'cancelled' : '' },
      r.reason ? '  理由：' + r.reason : '',
      r.trackingNo ? '  单号：' + r.trackingNo : ''
    ]);
    if (me.role & 4) {
      const reviews = await fetch('/api/reviews', { cache: 'no-store' }).then((r) => r.json());
      renderList(document.getElementById('reviewList'), reviews, (r) => [
        '#', String(r.id), ' 商品 ', String(r.productId), ' ', REVIEW_TYPE[r.type] || '',
        ' ',
        { text: REVIEW_STATUS[r.status] || r.status, kind: r.status === 2 ? 'rejected' : '' },
        r.rejectReason ? '  理由：' + r.rejectReason : ''
      ]);
    }
  } catch (error) {
    document.getElementById('productList').innerHTML = '<li class="empty">无法连接服务</li>';
  }
}

async function boot() {
  me = await loadMe();
  const gate = document.getElementById('gate');
  const app = document.getElementById('app');
  const who = document.getElementById('who');
  const logoutBtn = document.getElementById('logoutBtn');

  if (!me) {
    gate.hidden = false;
    app.hidden = true;
    who.textContent = '未登录';
    who.classList.remove('on');
    logoutBtn.hidden = true;
    return;
  }

  gate.hidden = true;
  app.hidden = false;
  who.textContent = '当前登录：#' + me.id + ' ' + me.username + ' · ' + roleName(me.role) +
                    ' · 余额 ¥' + Number(me.balance || 0).toFixed(2);
  who.classList.add('on');
  logoutBtn.hidden = false;

  // 按角色显示对应区块：buyer / seller / admin / manage（卖家或管理员）
  const ROLE_BITS = { buyer: 1, seller: 2, admin: 4, query: 1 | 4, manage: 2 | 4 };
  document.querySelectorAll('[data-role]').forEach((el) => {
    const need = ROLE_BITS[el.dataset.role] || 0;
    el.hidden = (me.role & need) === 0;
  });
  await refresh();
}

document.getElementById('logoutBtn').addEventListener('click', async () => {
  await post('/api/logout', {});
  await boot();
});

document.getElementById('accountForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('accResult');
  try {
    const data = await post('/api/dev/account', {
      username: val('accName'), password: val('accPass'), role: num('accRole')
    });
    show(el, data.ok ? 'good' : 'bad',
         data.ok ? '✓ 创建成功：' + data.username + '（id=' + data.id + '），请用该账号登录'
                 : describe(data));
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('balanceForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('balanceResult');
  try {
    const data = await post('/api/account/recharge', { amount: num('balanceAmount') });
    show(el, data.ok ? 'good' : 'bad',
         data.ok ? '✓ 充值成功，当前余额 ¥' + Number(data.balance).toFixed(2) : describe(data));
    if (data.ok) await boot();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('createForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('createResult');
  try {
    const data = await post('/api/products', {});
    show(el, data.ok ? 'good' : 'bad',
         data.ok ? '✓ 已创建草稿 #' + data.productId : describe(data));
    if (data.ok) {
      document.getElementById('draftProductId').value = data.productId;
      document.getElementById('submitProductId').value = data.productId;
    }
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('draftForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('draftResult');
  try {
    const data = await post('/api/products/draft', {
      productId: num('draftProductId'),
      title: val('draftTitle'), description: val('draftDesc'), category: val('draftCategory'),
      price: num('draftPrice'), remaining: num('draftRemaining')
    });
    show(el, data.ok ? 'good' : 'bad', data.ok ? '✓ 草稿已保存' : describe(data));
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('submitForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('submitResult');
  try {
    const data = await post('/api/products/submit', { productId: num('submitProductId') });
    show(el, data.ok ? 'good' : 'bad',
         data.ok ? '✓ 已提交，审核申请 #' + data.reviewApplicationId : describe(data));
    if (data.ok) document.getElementById('reviewId').value = data.reviewApplicationId;
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('reviewForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('reviewResult');
  try {
    const data = await post('/api/review', {
      reviewApplicationId: num('reviewId'),
      approval: val('approval') === 'true', reason: val('reason')
    });
    show(el, data.ok ? 'good' : 'bad', data.ok ? '✓ 审核结论已提交' : describe(data));
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('removeForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('removeResult');
  try {
    const data = await post('/api/products/remove', { productId: num('removeProductId') });
    show(el, data.ok ? 'good' : 'bad',
         data.ok ? '✓ 商品已下架，进行中的预约已终止并归还余量' : describe(data));
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('reserveForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('reserveResult');
  try {
    const data = await post('/api/reservations', {
      productId: num('reserveProductId'), amount: num('reserveAmount')
    });
    show(el, data.ok ? 'good' : 'bad',
         data.ok ? '✓ 预约成功 #' + data.reservationId : describe(data));
    if (data.ok) {
      document.getElementById('cancelReservationId').value = data.reservationId;
      document.getElementById('buyReservationId').value = data.reservationId;
    }
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('cancelForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('cancelResult');
  try {
    const data = await post('/api/reservations/cancel', { reservationId: num('cancelReservationId') });
    show(el, data.ok ? 'good' : 'bad', data.ok ? '✓ 预约已取消，余量已恢复' : describe(data));
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('buyForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('buyResult');
  try {
    const data = await post('/api/reservations/buy', { reservationId: num('buyReservationId') });
    show(el, data.ok ? 'good' : 'bad',
         data.ok ? '✓ 付款成功，订单 #' + data.orderId + '（货款由平台托管）' : describe(data));
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('confirmForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('confirmResult');
  try {
    const data = await post('/api/reservations/confirm', {
      reservationId: num('confirmReservationId'), changedPrice: num('confirmPrice')
    });
    show(el, data.ok ? 'good' : 'bad', data.ok ? '✓ 预约已确认' : describe(data));
    if (data.ok) document.getElementById('buyReservationId').value = data.reservationId;
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('shipForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('shipResult');
  try {
    const data = await post('/api/orders/ship', { orderId: num('shipOrderId') });
    show(el, data.ok ? 'good' : 'bad', data.ok ? '✓ 已发货' : describe(data));
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('receiptForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('receiptResult');
  try {
    const data = await post('/api/orders/receipt', { orderId: num('receiptOrderId') });
    show(el, data.ok ? 'good' : 'bad',
         data.ok ? '✓ 已确认收货，托管货款已转给卖家' : describe(data));
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('returnApplyForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('returnApplyResult');
  try {
    const data = await post('/api/returns', {
      orderId: num('returnOrderId'), reason: val('returnReason')
    });
    show(el, data.ok ? 'good' : 'bad',
         data.ok ? '✓ 已提交退货申请 #' + data.returnId + '，等待管理员审核' : describe(data));
    if (data.ok) {
      document.getElementById('returnDeliveredId').value = data.returnId;
      document.getElementById('returnReviewId').value = data.returnId;
      document.getElementById('returnReceivedId').value = data.returnId;
    }
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('returnReviewForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('returnReviewResult');
  try {
    const data = await post('/api/returns/review', {
      returnId: num('returnReviewId'),
      approval: val('returnApproval') === 'true',
      reason: val('returnReviewReason')
    });
    show(el, data.ok ? 'good' : 'bad', data.ok ? '✓ 退货结论已提交' : describe(data));
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('returnDeliveredForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('returnDeliveredResult');
  try {
    const data = await post('/api/returns/delivered', {
      returnId: num('returnDeliveredId'), trackingNo: val('returnTrackingNo')
    });
    show(el, data.ok ? 'good' : 'bad',
         data.ok ? '✓ 已登记快递单号，等待卖家确认收到' : describe(data));
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('returnReceivedForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('returnReceivedResult');
  try {
    const data = await post('/api/returns/received', { returnId: num('returnReceivedId') });
    show(el, data.ok ? 'good' : 'bad',
         data.ok ? '✓ 已确认收到退货，货款已退还买家' : describe(data));
    await refresh();
  } catch (error) { show(el, 'bad', '✗ 请求失败：无法连接本地服务'); }
});

document.getElementById('refreshBtn').addEventListener('click', refresh);
document.getElementById('searchForm').addEventListener('submit', async (event) => {
  event.preventDefault();
  const el = document.getElementById('searchResult');
  const params = new URLSearchParams();
  const keyword = val('searchKeyword').trim();
  const category = val('searchCategory').trim();
  const minPrice = val('searchMinPrice').trim();
  const maxPrice = val('searchMaxPrice').trim();
  if (keyword) params.set('keyword', keyword);
  if (category) params.set('category', category);
  if (minPrice) params.set('minPrice', minPrice);
  if (maxPrice) params.set('maxPrice', maxPrice);
  try {
    const data = await fetch('/api/search?' + params.toString(), { cache: 'no-store' })
      .then((r) => r.json());
    if (!data.ok) {
      show(el, 'bad', describe(data));
      return;
    }
    show(el, 'good', '✓ 命中 ' + data.items.length + ' 件商品，已按综合得分排序');
    renderSearch(document.getElementById('searchList'), data.items);
  } catch (error) {
    show(el, 'bad', '✗ 请求失败：无法连接本地服务');
  }
});

boot();
</script>
</body>
</html>)HTML";
}

long long WebUI::sessionAccount(const std::string& cookieHeader) const {
    const std::string token = cookieValue(cookieHeader, "sid");
    if (token.empty()) return 0;
    auto it = sessions.find(token);
    return it == sessions.end() ? 0 : it->second;
}

std::string WebUI::createSession(long long accountId) {
    const std::string token = randomToken();
    sessions[token] = accountId;
    return token;
}

void WebUI::destroySession(const std::string& cookieHeader) {
    const std::string token = cookieValue(cookieHeader, "sid");
    if (!token.empty()) sessions.erase(token);
}

std::string WebUI::meJson(const std::string& cookieHeader) {
    const long long id = sessionAccount(cookieHeader);
    if (id == 0) return "{\"ok\":false,\"msg\":\"未登录\"}";

    Result<Account> result = auth.getAccount(id);
    if (!result.ok())
        return errorJson(result.msg, static_cast<int>(result.code));

    const Account& account = result.value;
    return "{\"ok\":true,\"id\":" + std::to_string(account.id) +
           ",\"username\":\"" + jsonEscape(account.userName) + "\"" +
           ",\"role\":" + std::to_string(static_cast<unsigned>(account.role)) +
           ",\"balance\":" + std::to_string(account.balance) + "}";
}

WebReply WebUI::handleRegister(const std::string& body) {
    std::string username;
    std::string password;
    if (!jsonStringField(body, "username", username) ||
        !jsonStringField(body, "password", password)) {
        std::cout << "[register] missing username/password\n";
        return {errorJson("请求体缺少 username / password", -1), ""};
    }

    // 前端校验可以被绕过，所以 UI 层再校验一遍（REQ-027）
    const bool skipValidation = body.find("\"skipValidation\":true") != std::string::npos;
    if (!skipValidation) {
        const size_t nameLength = utf8Length(username);
        if (nameLength < 4 || nameLength > 20) {
            std::cout << "[register] '" << username << "' rejected by format check\n";
            return {errorJson("用户名需为 4–20 位", static_cast<int>(Err::InvalidArg)), ""};
        }
        bool hasLetter = false;
        bool hasDigit = false;
        for (unsigned char c : password) {
            if (std::isalpha(c)) hasLetter = true;
            if (std::isdigit(c)) hasDigit = true;
        }
        if (password.size() < 8) {
            std::cout << "[register] '" << username << "' rejected by format check\n";
            return {errorJson("密码至少 8 位", static_cast<int>(Err::InvalidArg)), ""};
        }
        if (!hasLetter || !hasDigit) {
            std::cout << "[register] '" << username << "' rejected by format check\n";
            return {errorJson("密码需同时包含字母和数字", static_cast<int>(Err::InvalidArg)), ""};
        }
    }

    Result<long long> result = auth.registerUser(username, password);
    if (!result.ok()) {
        std::cout << "[register] '" << username << "' failed: " << result.msg
                  << " (code=" << static_cast<int>(result.code) << ")\n";
        return {errorJson(result.msg, static_cast<int>(result.code)), ""};
    }

    registered.push_back({result.value, username});
    std::cout << "[register] '" << username << "' ok, id=" << result.value << "\n";
    return {"{\"ok\":true,\"id\":" + std::to_string(result.value) +
                ",\"username\":\"" + jsonEscape(username) + "\",\"msg\":\"注册成功\"}",
            ""};
}

WebReply WebUI::handleLogin(const std::string& body) {
    std::string username;
    std::string password;
    if (!jsonStringField(body, "username", username) ||
        !jsonStringField(body, "password", password)) {
        std::cout << "[login] missing username/password\n";
        return {errorJson("请求体缺少 username / password", -1), ""};
    }

    Result<long long> result = auth.login(username, password);
    if (!result.ok()) {
        std::cout << "[login] '" << username << "' failed: " << result.msg
                  << " (code=" << static_cast<int>(result.code) << ")\n";
        return {errorJson(result.msg, static_cast<int>(result.code)), ""};
    }

    // 登录成功：建立服务端会话并种 HttpOnly Cookie，页面之后都用会话身份
    const std::string token = createSession(result.value);
    std::cout << "[login] '" << username << "' ok, id=" << result.value
              << ", session=" << token << "\n";
    return {"{\"ok\":true,\"id\":" + std::to_string(result.value) +
                ",\"username\":\"" + jsonEscape(username) + "\",\"msg\":\"登录成功\"}",
            "Set-Cookie: sid=" + token + "; HttpOnly; Path=/; SameSite=Lax\r\n"};
}

WebReply WebUI::handleLogout(const std::string& cookieHeader) {
    destroySession(cookieHeader);
    std::cout << "[logout] session cleared\n";
    return {okJson(), "Set-Cookie: sid=; Max-Age=0; HttpOnly; Path=/; SameSite=Lax\r\n"};
}

std::string WebUI::handleCreateAccount(const std::string& body) {
    std::string username;
    std::string password;
    double roleValue = 0;
    if (!jsonStringField(body, "username", username) ||
        !jsonStringField(body, "password", password) ||
        !jsonNumberField(body, "role", roleValue)) {
        std::cout << "[dev] missing username/password/role\n";
        return errorJson("请求体缺少 username / password / role", -1);
    }

    // 测试页允许 Buyer、Seller、Buyer|Seller（需求允许买卖同一账号）与 Admin
    const unsigned rawRole = static_cast<unsigned>(roleValue);
    const bool validRole =
        rawRole == static_cast<unsigned>(Buyer) ||
        rawRole == static_cast<unsigned>(Seller) ||
        rawRole == static_cast<unsigned>(Buyer | Seller) ||
        rawRole == static_cast<unsigned>(Admin);
    if (!validRole)
        return errorJson("非法角色", static_cast<int>(Err::InvalidArg));

    Role role = static_cast<Role>(rawRole);

    Result<long long> result = auth.createAccount(username, password, role);
    if (!result.ok()) {
        std::cout << "[dev] '" << username << "' failed: " << result.msg
                  << " (code=" << static_cast<int>(result.code) << ")\n";
        return errorJson(result.msg, static_cast<int>(result.code));
    }

    std::cout << "[dev] '" << username << "' ok, id=" << result.value
              << ", role=" << static_cast<unsigned>(role) << "\n";
    return "{\"ok\":true,\"id\":" + std::to_string(result.value) +
           ",\"username\":\"" + jsonEscape(username) + "\"" +
           ",\"role\":" + std::to_string(static_cast<unsigned>(role)) + "}";
}

std::string WebUI::handleRecharge(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    double amount = 0;
    if (!jsonNumberField(body, "amount", amount))
        return errorJson("请求体缺少 amount", -1);

    Status status = auth.recharge(accountId, amount);
    if (!status.ok()) {
        std::cout << "[auth] recharge " << accountId << " failed: " << status.msg
                  << " (code=" << static_cast<int>(status.code) << ")\n";
        return errorJson(status.msg, static_cast<int>(status.code));
    }

    Result<Account> account = auth.getAccount(accountId);
    const double balance = account.ok() ? account.value.balance : 0;
    std::cout << "[auth] recharge " << accountId << " ok, amount=" << amount
              << ", balance=" << balance << "\n";
    return "{\"ok\":true,\"amount\":" + std::to_string(amount) +
           ",\"balance\":" + std::to_string(balance) + "}";
}

std::string WebUI::handleAddProduct(long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    Result<long long> result = products.addProduct(accountId);
    if (!result.ok()) {
        std::cout << "[product] add by " << accountId << " failed: " << result.msg
                  << " (code=" << static_cast<int>(result.code) << ")\n";
        return errorJson(result.msg, static_cast<int>(result.code));
    }

    std::cout << "[product] add by " << accountId << " ok, productId=" << result.value << "\n";
    return "{\"ok\":true,\"productId\":" + std::to_string(result.value) + "}";
}

std::string WebUI::handleSaveDraft(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long productId = 0;
    if (!jsonLongField(body, "productId", productId))
        return errorJson("请求体缺少 productId", -1);

    std::string title;
    std::string description;
    std::string category;
    double price = 0;
    double remaining = 0;
    jsonStringField(body, "title", title);
    jsonStringField(body, "description", description);
    jsonStringField(body, "category", category);
    if (!jsonNumberField(body, "price", price) ||
        !jsonNumberField(body, "remaining", remaining))
        return errorJson("请求体缺少 price / remaining", static_cast<int>(Err::InvalidArg));

    Status status = products.saveDraft(accountId, productId, title, description, category,
                                       price, static_cast<int>(remaining));
    if (!status.ok()) {
        std::cout << "[product] saveDraft " << productId << " by " << accountId
                  << " failed: " << status.msg
                  << " (code=" << static_cast<int>(status.code) << ")\n";
        return errorJson(status.msg, static_cast<int>(status.code));
    }

    std::cout << "[product] saveDraft " << productId << " by " << accountId << " ok\n";
    return okJson();
}

std::string WebUI::handleSubmitReview(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long productId = 0;
    if (!jsonLongField(body, "productId", productId))
        return errorJson("请求体缺少 productId", -1);

    Result<long long> result = products.submitForReview(accountId, productId);
    if (!result.ok()) {
        std::cout << "[product] submit " << productId << " by " << accountId
                  << " failed: " << result.msg
                  << " (code=" << static_cast<int>(result.code) << ")\n";
        return errorJson(result.msg, static_cast<int>(result.code));
    }

    std::cout << "[product] submit " << productId << " by " << accountId
              << " ok, reviewId=" << result.value << "\n";
    return "{\"ok\":true,\"reviewApplicationId\":" + std::to_string(result.value) + "}";
}

std::string WebUI::handleReview(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long reviewApplicationId = 0;
    bool approval = false;
    if (!jsonLongField(body, "reviewApplicationId", reviewApplicationId) ||
        !jsonBoolField(body, "approval", approval))
        return errorJson("请求体缺少 reviewApplicationId / approval", -1);

    std::string reason;
    jsonStringField(body, "reason", reason);

    Status status = products.review(accountId, reviewApplicationId, approval, reason);
    if (!status.ok()) {
        std::cout << "[review] " << reviewApplicationId << " by " << accountId
                  << " failed: " << status.msg
                  << " (code=" << static_cast<int>(status.code) << ")\n";
        return errorJson(status.msg, static_cast<int>(status.code));
    }

    std::cout << "[review] " << reviewApplicationId << " by " << accountId << " ok, "
              << (approval ? "approved" : "rejected") << "\n";
    return okJson();
}

std::string WebUI::handleRemoveFromShelf(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long productId = 0;
    if (!jsonLongField(body, "productId", productId))
        return errorJson("请求体缺少 productId", -1);

    Status status = products.removeFromShelf(accountId, productId);
    if (!status.ok()) {
        std::cout << "[product] remove " << productId << " by " << accountId
                  << " failed: " << status.msg
                  << " (code=" << static_cast<int>(status.code) << ")\n";
        return errorJson(status.msg, static_cast<int>(status.code));
    }

    std::cout << "[product] remove " << productId << " by " << accountId << " ok\n";
    return okJson();
}

std::string WebUI::handleReserve(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long productId = 0;
    double amount = 1;
    if (!jsonLongField(body, "productId", productId))
        return errorJson("请求体缺少 productId", -1);
    jsonNumberField(body, "amount", amount);  // 缺省按 1 件处理

    Result<long long> result = trades.reserve(accountId, productId, static_cast<int>(amount));
    if (!result.ok()) {
        std::cout << "[trade] reserve product " << productId << " by " << accountId
                  << " failed: " << result.msg
                  << " (code=" << static_cast<int>(result.code) << ")\n";
        return errorJson(result.msg, static_cast<int>(result.code));
    }

    std::cout << "[trade] reserve product " << productId << " by " << accountId
              << " ok, reservationId=" << result.value << "\n";
    return "{\"ok\":true,\"reservationId\":" + std::to_string(result.value) + "}";
}

std::string WebUI::handleCancelReservation(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long reservationId = 0;
    if (!jsonLongField(body, "reservationId", reservationId))
        return errorJson("请求体缺少 reservationId", -1);

    Result<long long> result = trades.cancelReservation(accountId, reservationId);
    if (!result.ok()) {
        std::cout << "[trade] cancel reservation " << reservationId << " by " << accountId
                  << " failed: " << result.msg
                  << " (code=" << static_cast<int>(result.code) << ")\n";
        return errorJson(result.msg, static_cast<int>(result.code));
    }

    std::cout << "[trade] cancel reservation " << reservationId << " by " << accountId << " ok\n";
    return "{\"ok\":true,\"reservationId\":" + std::to_string(result.value) + "}";
}

std::string WebUI::handleConfirmReservation(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long reservationId = 0;
    double changedPrice = -1;
    if (!jsonLongField(body, "reservationId", reservationId))
        return errorJson("请求体缺少 reservationId", -1);
    jsonNumberField(body, "changedPrice", changedPrice);

    Result<long long> result = trades.confirmReservation(
        accountId, reservationId, static_cast<int>(changedPrice));
    if (!result.ok()) {
        std::cout << "[trade] confirm reservation " << reservationId << " by " << accountId
                  << " failed: " << result.msg
                  << " (code=" << static_cast<int>(result.code) << ")\n";
        return errorJson(result.msg, static_cast<int>(result.code));
    }

    std::cout << "[trade] confirm reservation " << reservationId << " by " << accountId << " ok\n";
    return "{\"ok\":true,\"reservationId\":" + std::to_string(result.value) + "}";
}

std::string WebUI::handleBuy(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long reservationId = 0;
    if (!jsonLongField(body, "reservationId", reservationId))
        return errorJson("请求体缺少 reservationId", -1);

    Result<long long> result = trades.buy(accountId, reservationId);
    if (!result.ok()) {
        std::cout << "[trade] buy reservation " << reservationId << " by " << accountId
                  << " failed: " << result.msg
                  << " (code=" << static_cast<int>(result.code) << ")\n";
        return errorJson(result.msg, static_cast<int>(result.code));
    }

    std::cout << "[trade] buy reservation " << reservationId << " by " << accountId
              << " ok, orderId=" << result.value << "\n";
    return "{\"ok\":true,\"orderId\":" + std::to_string(result.value) + "}";
}

std::string WebUI::handleShip(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long orderId = 0;
    if (!jsonLongField(body, "orderId", orderId))
        return errorJson("请求体缺少 orderId", -1);

    Status status = trades.ship(accountId, orderId);
    if (!status.ok()) {
        std::cout << "[trade] ship order " << orderId << " by " << accountId
                  << " failed: " << status.msg
                  << " (code=" << static_cast<int>(status.code) << ")\n";
        return errorJson(status.msg, static_cast<int>(status.code));
    }

    std::cout << "[trade] ship order " << orderId << " by " << accountId << " ok\n";
    return okJson();
}

std::string WebUI::handleConfirmReceipt(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long orderId = 0;
    if (!jsonLongField(body, "orderId", orderId))
        return errorJson("请求体缺少 orderId", -1);

    Status status = trades.confirmReceipt(accountId, orderId);
    if (!status.ok()) {
        std::cout << "[trade] confirm receipt " << orderId << " by " << accountId
                  << " failed: " << status.msg
                  << " (code=" << static_cast<int>(status.code) << ")\n";
        return errorJson(status.msg, static_cast<int>(status.code));
    }

    std::cout << "[trade] confirm receipt " << orderId << " by " << accountId
              << " ok, escrow released\n";
    return okJson();
}

std::string WebUI::handleReturnApply(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long orderId = 0;
    if (!jsonLongField(body, "orderId", orderId))
        return errorJson("请求体缺少 orderId", -1);

    std::string reason;
    jsonStringField(body, "reason", reason);

    Result<long long> result = returns.apply(accountId, orderId, reason);
    if (!result.ok()) {
        std::cout << "[return] apply for order " << orderId << " by " << accountId
                  << " failed: " << result.msg
                  << " (code=" << static_cast<int>(result.code) << ")\n";
        return errorJson(result.msg, static_cast<int>(result.code));
    }

    std::cout << "[return] apply for order " << orderId << " by " << accountId
              << " ok, returnId=" << result.value << "\n";
    return "{\"ok\":true,\"returnId\":" + std::to_string(result.value) + "}";
}

std::string WebUI::handleReturnReview(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long returnId = 0;
    bool approval = false;
    if (!jsonLongField(body, "returnId", returnId) ||
        !jsonBoolField(body, "approval", approval))
        return errorJson("请求体缺少 returnId / approval", -1);

    std::string reason;
    jsonStringField(body, "reason", reason);

    Status status = returns.review(accountId, returnId, approval, reason);
    if (!status.ok()) {
        std::cout << "[return] review " << returnId << " by " << accountId
                  << " failed: " << status.msg
                  << " (code=" << static_cast<int>(status.code) << ")\n";
        return errorJson(status.msg, static_cast<int>(status.code));
    }

    std::cout << "[return] review " << returnId << " by " << accountId << " ok, "
              << (approval ? "approved" : "rejected") << "\n";
    return okJson();
}

std::string WebUI::handleReturnDelivered(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long returnId = 0;
    if (!jsonLongField(body, "returnId", returnId))
        return errorJson("请求体缺少 returnId", -1);

    std::string trackingNo;
    jsonStringField(body, "trackingNo", trackingNo);

    Status status = returns.confirmDelivered(accountId, returnId, trackingNo);
    if (!status.ok()) {
        std::cout << "[return] deliver " << returnId << " by " << accountId
                  << " failed: " << status.msg
                  << " (code=" << static_cast<int>(status.code) << ")\n";
        return errorJson(status.msg, static_cast<int>(status.code));
    }

    std::cout << "[return] deliver " << returnId << " by " << accountId << " ok\n";
    return okJson();
}

std::string WebUI::handleReturnReceived(const std::string& body, long long accountId) {
    if (accountId == 0)
        return errorJson("请先登录", static_cast<int>(Err::Unauthorized));

    long long returnId = 0;
    if (!jsonLongField(body, "returnId", returnId))
        return errorJson("请求体缺少 returnId", -1);

    Status status = returns.confirmReceived(accountId, returnId);
    if (!status.ok()) {
        std::cout << "[return] receive " << returnId << " by " << accountId
                  << " failed: " << status.msg
                  << " (code=" << static_cast<int>(status.code) << ")\n";
        return errorJson(status.msg, static_cast<int>(status.code));
    }

    std::cout << "[return] receive " << returnId << " by " << accountId
              << " ok, refunded\n";
    return okJson();
}

// TODO(REQ-006)：仿照下面的 productsJson / returnsJson，新增两个只读接口：
//   std::string WebUI::conversationsJson(long long accountId)
//     -> messages.listConversations(accountId)
//        字段：id / productId / buyerId / sellerId / createdAt（可选 lastMessageAt）
//   std::string WebUI::messagesJson(long long conversationId, long long accountId)
//     -> 调 messages.list(accountId, conversationId)
//        字段：id / conversationId / senderId / kind / content / sentAt / read
// 注意：
//   - content 一定要走 jsonEscape，图片地址也按字符串处理；
//   - 时间建议转成毫秒时间戳（duration_cast<milliseconds>(tp.time_since_epoch()).count()）
//     或 "YYYY-MM-DD HH:MM:SS"，前后端用同一种；
//   - 权限判断不要写在这里，交给 MessageService（非参与方返回 Forbidden）。
std::string WebUI::searchJson(const SearchQuery& query) {
    Result<std::vector<SearchItem>> result = search.search(query);
    if (!result.ok()) {
        return errorJson(result.msg, static_cast<int>(result.code));
    }

    std::string json = "{\"ok\":true,\"items\":[";
    const auto& items = result.value;
    for (size_t i = 0; i < items.size(); ++i) {
        const SearchItem& item = items[i];
        if (i != 0) json += ",";
        json += "{\"productId\":" + std::to_string(item.productId) +
                ",\"title\":\"" + jsonEscape(item.title) + "\"" +
                ",\"category\":\"" + jsonEscape(item.category) + "\"" +
                ",\"price\":" + std::to_string(item.price) +
                ",\"remaining\":" + std::to_string(item.remaining) +
                ",\"soldOut\":" + (item.soldOut ? "true" : "false") +
                ",\"sales30d\":" + std::to_string(item.sales30d) +
                ",\"matchScore\":" + std::to_string(item.matchScore) +
                ",\"salesScore\":" + std::to_string(item.salesScore) +
                ",\"totalScore\":" + std::to_string(item.totalScore) + "}";
    }
    json += "]}";
    return json;
}

std::string WebUI::productsJson() {
    std::vector<Commodity> list = products.listProducts();
    std::string json = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        const Commodity& c = list[i];
        if (i != 0) json += ",";
        json += "{\"id\":" + std::to_string(c.id) +
                ",\"sellerId\":" + std::to_string(c.sellerId) +
                ",\"title\":\"" + jsonEscape(c.title) + "\"" +
                ",\"description\":\"" + jsonEscape(c.description) + "\"" +
                ",\"category\":\"" + jsonEscape(c.category) + "\"" +
                ",\"price\":" + std::to_string(c.price) +
                ",\"remaining\":" + std::to_string(c.remaining) +
                ",\"status\":" + std::to_string(static_cast<int>(c.status)) + "}";
    }
    json += "]";
    return json;
}

std::string WebUI::reviewsJson() {
    std::vector<ReviewApplication> list = products.listReviews();
    std::string json = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        const ReviewApplication& r = list[i];
        if (i != 0) json += ",";
        json += "{\"id\":" + std::to_string(r.id) +
                ",\"productId\":" + std::to_string(r.productId) +
                ",\"sellerId\":" + std::to_string(r.sellerId) +
                ",\"reviewerId\":" + std::to_string(r.reviewerId) +
                ",\"status\":" + std::to_string(static_cast<int>(r.status)) +
                ",\"type\":" + std::to_string(static_cast<int>(r.type)) +
                ",\"rejectReason\":\"" + jsonEscape(r.rejectReason) + "\"}";
    }
    json += "]";
    return json;
}

std::string WebUI::reservationsJson() {
    std::vector<Reservation> list = trades.listReservations();
    std::string json = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        const Reservation& r = list[i];
        if (i != 0) json += ",";
        json += "{\"id\":" + std::to_string(r.id) +
                ",\"sellerId\":" + std::to_string(r.sellerId) +
                ",\"purchaserId\":" + std::to_string(r.purchaserId) +
                ",\"commodityId\":" + std::to_string(r.commodityId) +
                ",\"amount\":" + std::to_string(r.amount) +
                ",\"commodityPrice\":" + std::to_string(r.commodityPrice) +
                ",\"status\":" + std::to_string(static_cast<int>(r.status)) + "}";
    }
    json += "]";
    return json;
}

std::string WebUI::ordersJson() {
    std::vector<Order> list = trades.listOrders();
    std::string json = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        const Order& o = list[i];
        if (i != 0) json += ",";
        json += "{\"id\":" + std::to_string(o.id) +
                ",\"reservationId\":" + std::to_string(o.reservationId) +
                ",\"buyerId\":" + std::to_string(o.buyerId) +
                ",\"sellerId\":" + std::to_string(o.sellerId) +
                ",\"commodityId\":" + std::to_string(o.commodityId) +
                ",\"amount\":" + std::to_string(o.amount) +
                ",\"totalPrice\":" + std::to_string(o.totalPrice) +
                ",\"status\":" + std::to_string(static_cast<int>(o.status)) + "}";
    }
    json += "]";
    return json;
}

std::string WebUI::returnsJson() {
    std::vector<ReturnRequest> list = returns.listReturns();
    std::string json = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        const ReturnRequest& r = list[i];
        if (i != 0) json += ",";
        json += "{\"id\":" + std::to_string(r.id) +
                ",\"orderId\":" + std::to_string(r.orderId) +
                ",\"buyerId\":" + std::to_string(r.buyerId) +
                ",\"sellerId\":" + std::to_string(r.sellerId) +
                ",\"reason\":\"" + jsonEscape(r.reason) + "\"" +
                ",\"reviewReason\":\"" + jsonEscape(r.reviewReason) + "\"" +
                ",\"trackingNo\":\"" + jsonEscape(r.trackingNo) + "\"" +
                ",\"status\":" + std::to_string(static_cast<int>(r.status)) + "}";
    }
    json += "]";
    return json;
}

std::string WebUI::escrowJson() {
    return "{\"ok\":true,\"balance\":" + std::to_string(trades.escrowBalance()) + "}";
}

std::string WebUI::registeredUsersJson() const {
    std::string json = "[";
    for (size_t i = 0; i < registered.size(); ++i) {
        if (i != 0) json += ",";
        json += "{\"id\":" + std::to_string(registered[i].id) +
                ",\"username\":\"" + jsonEscape(registered[i].username) + "\"}";
    }
    json += "]";
    return json;
}

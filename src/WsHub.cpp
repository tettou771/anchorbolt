#include "WsHub.h"
#include "Sha.h"

#include <cstring>

using namespace std;
using namespace tc;

namespace {

const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Header field, case-insensitive name, trimmed value.
string headerValue(const string& head, const string& name) {
    string lower = head;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    string key = name + ":";
    for (char& c : key) c = (char)tolower((unsigned char)c);
    size_t pos = lower.find(key);
    if (pos == string::npos) return "";
    size_t start = pos + key.size();
    size_t end = head.find("\r\n", start);
    if (end == string::npos) end = head.size();
    string v = head.substr(start, end - start);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
    return v;
}

// Server frames are never masked. FIN always set (no fragmentation).
vector<char> encodeFrame(uint8_t opcode, const string& payload) {
    vector<char> f;
    f.push_back((char)(0x80 | opcode));
    size_t n = payload.size();
    if (n < 126) {
        f.push_back((char)n);
    } else if (n <= 0xFFFF) {
        f.push_back(126);
        f.push_back((char)((n >> 8) & 0xFF));
        f.push_back((char)(n & 0xFF));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back((char)((uint64_t)n >> (i * 8) & 0xFF));
    }
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

} // namespace

bool WsHub::start(int port) {
    recvL_ = server_.onReceive.listen([this](TcpServerReceiveEventArgs& e) {
        handleReceive(e.clientId, e.data.data(), e.data.size());
    });
    connL_ = server_.onClientConnect.listen([this](TcpClientConnectEventArgs& e) {
        lock_guard<mutex> lock(mutex_);
        conns_[e.clientId] = Conn{};
    });
    discL_ = server_.onClientDisconnect.listen([this](TcpClientDisconnectEventArgs& e) {
        bool wasOpen = false;
        {
            lock_guard<mutex> lock(mutex_);
            auto it = conns_.find(e.clientId);
            if (it != conns_.end()) {
                wasOpen = it->second.open;
                conns_.erase(it);
            }
        }
        if (wasOpen && onClosed) onClosed(e.clientId);
    });
    return server_.start(port, 64);
}

void WsHub::stop() {
    server_.stop();
    lock_guard<mutex> lock(mutex_);
    conns_.clear();
}

bool WsHub::sendText(int clientId, const string& text) {
    return server_.send(clientId, encodeFrame(0x1, text));
}

void WsHub::closeClient(int clientId) {
    server_.send(clientId, encodeFrame(0x8, ""));
    server_.disconnectClient(clientId);
}

void WsHub::handleReceive(int clientId, const char* data, size_t size) {
    // Extract what to do under the lock, act outside it (onMessage may call
    // back into sendText).
    vector<string> messages;
    bool doClose = false;
    {
        lock_guard<mutex> lock(mutex_);
        auto it = conns_.find(clientId);
        if (it == conns_.end()) return;
        Conn& c = it->second;
        c.buf.insert(c.buf.end(), data, data + size);

        if (!c.open) {
            if (!tryHandshake(clientId, c)) return;  // waiting for more header bytes
        }

        // Decode complete frames.
        while (c.open) {
            if (c.buf.size() < 2) break;
            uint8_t b1 = (uint8_t)c.buf[0], b2 = (uint8_t)c.buf[1];
            uint8_t opcode = b1 & 0x0F;
            bool masked = (b2 & 0x80) != 0;
            uint64_t len = b2 & 0x7F;
            size_t header = 2;
            if (len == 126) {
                if (c.buf.size() < 4) break;
                len = ((uint8_t)c.buf[2] << 8) | (uint8_t)c.buf[3];
                header = 4;
            } else if (len == 127) {
                if (c.buf.size() < 10) break;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | (uint8_t)c.buf[2 + i];
                header = 10;
            }
            uint8_t mask[4] = {0, 0, 0, 0};
            if (masked) {
                if (c.buf.size() < header + 4) break;
                memcpy(mask, &c.buf[header], 4);
                header += 4;
            }
            if (c.buf.size() < header + len) break;

            string payload(&c.buf[header], &c.buf[header] + len);
            if (masked) {
                for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= (char)mask[i % 4];
            }
            c.buf.erase(c.buf.begin(), c.buf.begin() + (long)(header + len));

            if (opcode == 0x1) {          // text
                messages.push_back(std::move(payload));
            } else if (opcode == 0x9) {   // ping -> pong
                server_.send(clientId, encodeFrame(0xA, payload));
            } else if (opcode == 0x8) {   // close
                doClose = true;
                break;
            }
            // binary (0x2) and continuation (0x0) are ignored — out of contract
        }
    }
    for (auto& m : messages) {
        if (onMessage) onMessage(clientId, m);
    }
    if (doClose) closeClient(clientId);
}

// Consume the HTTP Upgrade request once fully buffered and reply 101.
bool WsHub::tryHandshake(int clientId, Conn& c) {
    string head(c.buf.begin(), c.buf.end());
    size_t end = head.find("\r\n\r\n");
    if (end == string::npos) return false;

    string key = headerValue(head.substr(0, end), "Sec-WebSocket-Key");
    if (key.empty()) {
        server_.send(clientId, string("HTTP/1.1 400 Bad Request\r\n\r\n"));
        server_.disconnectClient(clientId);
        return false;
    }
    auto digest = sha::sha1((key + kWsGuid).data(), key.size() + strlen(kWsGuid));
    string accept = toBase64(digest.data(), digest.size());

    string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    server_.send(clientId, resp);

    c.buf.erase(c.buf.begin(), c.buf.begin() + (long)(end + 4));
    c.open = true;
    return true;
}

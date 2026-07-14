#pragma once

#include <TrussC.h>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Minimal WebSocket server (RFC 6455) on top of tc::TcpServer — the serve
// side of the agent command channel. Deliberately narrow: text frames only,
// no fragmentation (our peer is anchorbolt's own client), masked client
// frames, ping answered with pong. Callbacks fire on TcpServer's
// per-connection threads; guard your own state.
class WsHub {
public:
    // message(clientId, text). closed(clientId) fires on disconnect after a
    // completed handshake.
    std::function<void(int, const std::string&)> onMessage;
    std::function<void(int)> onClosed;

    bool start(int port);
    void stop();

    bool sendText(int clientId, const std::string& text);
    void closeClient(int clientId);

private:
    struct Conn {
        bool open = false;              // handshake completed
        std::vector<char> buf;          // bytes not yet consumed
    };

    void handleReceive(int clientId, const char* data, size_t size);
    bool tryHandshake(int clientId, Conn& c);
    void processFrames(int clientId, Conn& c);

    tc::TcpServer server_;
    tc::EventListener recvL_, discL_, connL_;
    std::map<int, Conn> conns_;
    std::mutex mutex_;
};

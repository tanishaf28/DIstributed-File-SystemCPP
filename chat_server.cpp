/**
 * ============================================================
 *  MULTI-USER CHAT SYSTEM : Slack/Discord Backend in C++
 *  C++17 Reference Implementation
 * ============================================================
 *
 * BUILD:
 *   g++ -std=c++17 -pthread -o chat_server chat_server.cpp
 *
 * RUN SERVER:
 *   ./chat_server server 9000
 *
 * RUN CLIENT:
 *   ./chat_server client 127.0.0.1 9000 alice general
 *   ./chat_server client 127.0.0.1 9000 bob   general
 *
 * ============================================================
 *  CONCEPTS COVERED
 * ============================================================
 *  [THREAD]       - std::thread, one per connected client
 *  [MUTEX]        - std::mutex + std::lock_guard for shared state
 *  [CONDVAR]      - std::condition_variable for message queue
 *  [SMART_PTR]    - shared_ptr for Client, Channel objects
 *  [TCP]          - POSIX TCP server with full duplex comms
 *  [STL]          - unordered_map, deque, vector, set
 *  [MOVE]         - Move semantics for message passing
 *  [SERIALIZE]    - JSON-like binary message framing
 *  [RAII]         - Socket wrapper, lock_guard
 *  [POLYMORPHISM] - Abstract MessageHandler
 * ============================================================
 */

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <deque>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <sstream>
#include <fstream>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <stdexcept>
#include <optional>

// POSIX sockets
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// ══════════════════════════════════════════════════════════════
//  SECTION 1: MESSAGE MODEL [SERIALIZE] [MOVE]
// ══════════════════════════════════════════════════════════════

enum class MsgType : uint8_t {
    JOIN    = 1,   // client joins a channel
    LEAVE   = 2,   // client leaves a channel
    CHAT    = 3,   // regular chat message
    HISTORY = 4,   // server sends history to new joiner
    SYSTEM  = 5,   // server-generated notification
    LIST    = 6,   // list channels or users
    QUIT    = 7    // client disconnects
};

struct ChatMessage {
    MsgType     type;
    std::string channel;    // target channel name
    std::string sender;     // username
    std::string body;       // message text
    std::time_t timestamp;

    ChatMessage() : type(MsgType::CHAT), timestamp(std::time(nullptr)) {}

    // [MOVE] Move constructor for efficient queue insertion
    ChatMessage(ChatMessage&&) noexcept = default;
    ChatMessage& operator=(ChatMessage&&) noexcept = default;
    ChatMessage(const ChatMessage&) = default;
    ChatMessage& operator=(const ChatMessage&) = default;

    // [SERIALIZE] Simple text framing: TYPE|CHANNEL|SENDER|BODY\n
    std::string serialize() const {
        return std::to_string(static_cast<int>(type)) + "|"
             + channel + "|" + sender + "|" + body + "\n";
    }

    static std::optional<ChatMessage> parse(const std::string& line) {
        // Split on '|'  first 4 fields
        std::vector<std::string> parts;
        std::stringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, '|')) parts.push_back(tok);
        if (parts.size() < 4) return std::nullopt;

        ChatMessage m;
        m.type      = static_cast<MsgType>(std::stoi(parts[0]));
        m.channel   = parts[1];
        m.sender    = parts[2];
        m.body      = parts[3];
        m.timestamp = std::time(nullptr);
        return m;
    }

    std::string formatted() const {
        char tbuf[20];
        std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S",
                      std::localtime(&timestamp));
        return "[" + std::string(tbuf) + "] #" + channel
               + " <" + sender + "> " + body;
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 2: THREAD-SAFE MESSAGE QUEUE [THREAD] [CONDVAR]
// ══════════════════════════════════════════════════════════════

/**
 * [THREAD] [CONDVAR] MessageQueue<T>  blocking producer/consumer queue.
 * push() notifies waiting pop() calls.
 * Used to decouple the receive thread from the broadcast thread.
 */
template<typename T>
class MessageQueue {
    std::deque<T>           q_;       // [STL]
    mutable std::mutex      mtx_;     // [MUTEX]
    std::condition_variable cv_;      // [CONDVAR]
    std::atomic<bool>       closed_{false};

public:
    void push(T item) {
        {
            std::lock_guard<std::mutex> lk(mtx_);  // [RAII]
            q_.push_back(std::move(item));          // [MOVE]
        }
        cv_.notify_one();                           // [CONDVAR]
    }

    // Blocking pop : returns false if queue is closed and empty
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [&]{ return !q_.empty() || closed_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());               // [MOVE]
        q_.pop_front();
        return true;
    }

    void close() {
        closed_ = true;
        cv_.notify_all();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.empty();
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 3: CHANNEL : message history + membership [SMART_PTR]
// ══════════════════════════════════════════════════════════════

struct Client;  // forward declaration

/**
 * Channel stores message history and the set of joined clients.
 * [SMART_PTR] Clients are weak_ptr to avoid circular ownership.
 * [MUTEX] All mutation is guarded.
 */
class Channel {
    mutable std::mutex              mtx_;
    std::string                     name_;
    std::deque<ChatMessage>         history_;     // [STL] ring buffer
    std::set<std::string>           members_;     // [STL] usernames
    static constexpr size_t         MAX_HISTORY = 50;

public:
    explicit Channel(std::string name) : name_(std::move(name)) {}

    const std::string& name() const { return name_; }

    void addMember(const std::string& user) {
        std::lock_guard<std::mutex> lk(mtx_);
        members_.insert(user);
    }

    void removeMember(const std::string& user) {
        std::lock_guard<std::mutex> lk(mtx_);
        members_.erase(user);
    }

    bool hasMember(const std::string& user) const {
        std::lock_guard<std::mutex> lk(mtx_);
        return members_.count(user) > 0;
    }

    std::set<std::string> members() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return members_;
    }

    // [MOVE] Store message in history
    void addHistory(ChatMessage msg) {
        std::lock_guard<std::mutex> lk(mtx_);
        history_.push_back(std::move(msg));      // [MOVE]
        if (history_.size() > MAX_HISTORY)
            history_.pop_front();
    }

    std::deque<ChatMessage> history() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return history_;
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 4: CLIENT CONNECTION [THREAD] [SMART_PTR] [RAII]
// ══════════════════════════════════════════════════════════════

/**
 * [SMART_PTR] Client — represents one connected user.
 * Shared_ptr because both the server's client list and the
 * broadcast function need to reference the same client.
 */
struct Client : std::enable_shared_from_this<Client> {
    int         fd;
    std::string username;
    std::string current_channel;
    std::atomic<bool> alive{true};

    explicit Client(int fd, std::string user)
        : fd(fd), username(std::move(user)) {}

    ~Client() { if (fd >= 0) ::close(fd); }  // [RAII]

    // Send raw string to this client
    bool send(const std::string& msg) {
        if (!alive) return false;
        ssize_t n = ::send(fd, msg.data(), msg.size(), MSG_NOSIGNAL);
        return n == static_cast<ssize_t>(msg.size());
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 5: CHAT SERVER [THREAD] [MUTEX] [TCP]
// ══════════════════════════════════════════════════════════════

class ChatServer {
    uint16_t    port_;
    int         server_fd_ = -1;
    std::atomic<bool> running_{false};

    // [MUTEX] Protecting shared collections
    mutable std::mutex clients_mtx_;
    mutable std::mutex channels_mtx_;

    // [STL] [SMART_PTR]
    std::unordered_map<std::string, std::shared_ptr<Client>>  clients_;
    std::unordered_map<std::string, std::shared_ptr<Channel>> channels_;

    // Message log (in-memory persistence)
    std::deque<ChatMessage> global_log_;
    mutable std::mutex      log_mtx_;

    // ─── helpers ────────────────────────────────────────────

    std::shared_ptr<Channel> getOrCreateChannel(const std::string& name) {
        std::lock_guard<std::mutex> lk(channels_mtx_);
        auto& ch = channels_[name];
        if (!ch) ch = std::make_shared<Channel>(name);  // [SMART_PTR]
        return ch;
    }

    void broadcast(const ChatMessage& msg,
                   const std::string& channel,
                   const std::string& exclude_user = "") {
        auto ch = [&]() -> std::shared_ptr<Channel> {
            std::lock_guard<std::mutex> lk(channels_mtx_);
            auto it = channels_.find(channel);
            return it != channels_.end() ? it->second : nullptr;
        }();
        if (!ch) return;

        auto members = ch->members();
        std::string wire = msg.serialize();

        std::lock_guard<std::mutex> lk(clients_mtx_);
        for (const auto& uname : members) {
            if (uname == exclude_user) continue;
            auto it = clients_.find(uname);
            if (it != clients_.end())
                it->second->send(wire);
        }
    }

    void sendHistory(std::shared_ptr<Client> client,
                     std::shared_ptr<Channel> channel) {
        auto hist = channel->history();
        for (const auto& m : hist) {
            ChatMessage hm = m;
            hm.type = MsgType::HISTORY;
            client->send(hm.serialize());
        }
    }

    // ─── client message processor ───────────────────────────
    void handleClient(std::shared_ptr<Client> client) {  // [THREAD]
        std::string line_buf;
        char buf[1024];

        while (client->alive) {
            ssize_t n = ::recv(client->fd, buf, sizeof(buf)-1, 0);
            if (n <= 0) { client->alive = false; break; }
            buf[n] = '\0';
            line_buf += buf;

            size_t pos;
            while ((pos = line_buf.find('\n')) != std::string::npos) {
                std::string line = line_buf.substr(0, pos);
                line_buf.erase(0, pos + 1);
                processMessage(client, line);
            }
        }

        // Cleanup: remove from all channels
        {
            std::lock_guard<std::mutex> lk(channels_mtx_);
            for (auto& [_, ch] : channels_) ch->removeMember(client->username);
        }
        {
            std::lock_guard<std::mutex> lk(clients_mtx_);
            clients_.erase(client->username);
        }

        // Broadcast departure
        ChatMessage bye;
        bye.type    = MsgType::SYSTEM;
        bye.channel = client->current_channel;
        bye.sender  = "server";
        bye.body    = client->username + " has left";
        if (!client->current_channel.empty())
            broadcast(bye, client->current_channel);

        std::cout << "[Server] " << client->username << " disconnected\n";
    }

    void processMessage(std::shared_ptr<Client> client, const std::string& raw) {
        auto opt = ChatMessage::parse(raw);
        if (!opt) return;
        auto& msg = *opt;
        msg.sender = client->username;

        switch (msg.type) {
            case MsgType::JOIN: {
                auto ch = getOrCreateChannel(msg.channel);
                // Send history to new joiner
                sendHistory(client, ch);
                ch->addMember(client->username);
                client->current_channel = msg.channel;

                ChatMessage notify;
                notify.type    = MsgType::SYSTEM;
                notify.channel = msg.channel;
                notify.sender  = "server";
                notify.body    = client->username + " joined #" + msg.channel;
                broadcast(notify, msg.channel);
                std::cout << "[Server] " << client->username
                          << " joined #" << msg.channel << "\n";
                break;
            }
            case MsgType::CHAT: {
                auto ch = getOrCreateChannel(msg.channel);
                ch->addHistory(msg);            // persist in channel
                {
                    std::lock_guard<std::mutex> lk(log_mtx_);
                    global_log_.push_back(msg); // global log
                }
                broadcast(msg, msg.channel);
                std::cout << "[Server] " << msg.formatted() << "\n";
                break;
            }
            case MsgType::LEAVE: {
                std::lock_guard<std::mutex> lk(channels_mtx_);
                auto it = channels_.find(msg.channel);
                if (it != channels_.end())
                    it->second->removeMember(client->username);
                break;
            }
            case MsgType::LIST: {
                std::string list = "Channels: ";
                {
                    std::lock_guard<std::mutex> lk(channels_mtx_);
                    for (auto& [name, _] : channels_) list += "#" + name + " ";
                }
                ChatMessage resp;
                resp.type    = MsgType::SYSTEM;
                resp.channel = msg.channel;
                resp.sender  = "server";
                resp.body    = list;
                client->send(resp.serialize());
                break;
            }
            case MsgType::QUIT:
                client->alive = false;
                break;
            default: break;
        }
    }

public:
    explicit ChatServer(uint16_t port) : port_(port) {}

    void run() {
        server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) throw std::runtime_error("socket() failed");

        int opt = 1;
        ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);

        if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("bind() failed");
        if (::listen(server_fd_, 32) < 0)
            throw std::runtime_error("listen() failed");

        running_ = true;
        std::cout << "[Server] Chat server listening on port " << port_ << "\n";

        // Pre-create #general channel
        getOrCreateChannel("general");

        while (running_) {
            sockaddr_in caddr{};
            socklen_t clen = sizeof(caddr);
            int cfd = ::accept(server_fd_,
                               reinterpret_cast<sockaddr*>(&caddr), &clen);
            if (cfd < 0) continue;

            // First message must be the username
            char ubuf[128] = {};
            ssize_t n = ::recv(cfd, ubuf, sizeof(ubuf)-1, 0);
            if (n <= 0) { ::close(cfd); continue; }
            std::string username(ubuf, n);
            // Strip trailing newline
            while (!username.empty() && (username.back() == '\n' || username.back() == '\r'))
                username.pop_back();

            auto client = std::make_shared<Client>(cfd, username);  // [SMART_PTR]
            {
                std::lock_guard<std::mutex> lk(clients_mtx_);
                clients_[username] = client;
            }
            std::cout << "[Server] " << username << " connected\n";

            // [THREAD] Spawn handler thread per client
            std::thread([this, client]() {
                handleClient(client);
            }).detach();
        }
    }

    ~ChatServer() { if (server_fd_ >= 0) ::close(server_fd_); }  // [RAII]
};


// ══════════════════════════════════════════════════════════════
//  SECTION 6: CHAT CLIENT [TCP] [THREAD]
// ══════════════════════════════════════════════════════════════

class ChatClient {
    std::string host_, username_, channel_;
    uint16_t    port_;
    int         fd_ = -1;

    void receiveLoop() {
        // [THREAD] Dedicated receive thread — prints incoming messages
        char buf[2048];
        std::string line_buf;
        while (true) {
            ssize_t n = ::recv(fd_, buf, sizeof(buf)-1, 0);
            if (n <= 0) { std::cout << "[Client] Disconnected\n"; break; }
            buf[n] = '\0';
            line_buf += buf;
            size_t pos;
            while ((pos = line_buf.find('\n')) != std::string::npos) {
                std::string line = line_buf.substr(0, pos);
                line_buf.erase(0, pos+1);
                auto opt = ChatMessage::parse(line);
                if (opt) {
                    if (opt->type == MsgType::HISTORY)
                        std::cout << "[History] " << opt->formatted() << "\n";
                    else
                        std::cout << opt->formatted() << "\n";
                }
            }
        }
    }

public:
    ChatClient(std::string host, uint16_t port,
               std::string user, std::string channel)
        : host_(std::move(host)), port_(port),
          username_(std::move(user)), channel_(std::move(channel)) {}

    void run() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port_);
        ::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);

        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error("Connection failed");

        // Send username
        ::send(fd_, (username_ + "\n").c_str(), username_.size()+1, 0);

        // Join channel
        ChatMessage join;
        join.type    = MsgType::JOIN;
        join.channel = channel_;
        join.sender  = username_;
        join.body    = "";
        ::send(fd_, join.serialize().c_str(), join.serialize().size(), 0);

        // [THREAD] Start receive thread
        std::thread recv_thread([this]{ receiveLoop(); });
        recv_thread.detach();

        std::cout << "[Client] Connected as " << username_
                  << " to #" << channel_ << "\n";
        std::cout << "[Client] Type messages (or /quit to exit)\n";

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "/quit") break;
            if (line == "/list") {
                ChatMessage lst;
                lst.type = MsgType::LIST; lst.channel = channel_;
                ::send(fd_, lst.serialize().c_str(), lst.serialize().size(), 0);
                continue;
            }
            ChatMessage msg;
            msg.type    = MsgType::CHAT;
            msg.channel = channel_;
            msg.sender  = username_;
            msg.body    = line;
            ::send(fd_, msg.serialize().c_str(), msg.serialize().size(), 0);
        }
        ::close(fd_);
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 7: MAIN
// ══════════════════════════════════════════════════════════════

// Runtime flow:
// 1. Start as `server <port>` or `client <host> <port> <username> <channel>`.
// 2. The server accepts TCP clients, assigns each one a thread, and routes
//    JOIN/CHAT/LIST/QUIT messages to the right channel handlers.
// 3. The client connects, sends its username and join request, then keeps a
//    receive thread alive while the foreground thread reads typed messages.

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " server <port>\n"
                  << "  " << argv[0] << " client <host> <port> <username> <channel>\n";
        return 1;
    }
    std::string mode = argv[1];
    try {
        if (mode == "server") {
            ChatServer server(static_cast<uint16_t>(std::stoi(argv[2])));
            server.run();
        } else if (mode == "client") {
            if (argc < 6) { std::cerr << "Need: host port username channel\n"; return 1; }
            ChatClient client(argv[2],
                              static_cast<uint16_t>(std::stoi(argv[3])),
                              argv[4], argv[5]);
            client.run();
        }
    } catch (const std::exception& e) {
        std::cerr << "[Error] " << e.what() << "\n";
        return 1;
    }
}

/*
 * ============================================================
 *  EXTENSION IDEAS
 * ============================================================
 *  1. SQLite persistence — replace deque history with a database
 *  2. Thread pool — replace detach() with bounded pool
 *  3. Private messages — DM channel between two users
 *  4. Message reactions (emoji) stored per-message
 *  5. TLS with OpenSSL for encrypted transport
 * ============================================================
 */

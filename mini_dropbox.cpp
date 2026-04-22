/**
 * ============================================================
 *  MINI DROPBOX — Modular Distributed File Storage System
 *  A complete, single-file reference implementation in C++17
 * ============================================================
 *
 * BUILD:
 *   g++ -std=c++17 -pthread -o mini_dropbox mini_dropbox.cpp
 *
 * RUN SERVER:
 *   ./mini_dropbox server 8080
 *
 * RUN CLIENT (upload):
 *   ./mini_dropbox upload 127.0.0.1 8080 myfile.txt
 *
 * RUN CLIENT (download):
 *   ./mini_dropbox download 127.0.0.1 8080 myfile.txt 1
 *
 * RUN CLIENT (list versions):
 *   ./mini_dropbox list 127.0.0.1 8080 myfile.txt
 *
 * ============================================================
 *  CONCEPTS COVERED (tagged in comments as [CONCEPT])
 * ============================================================
 *  [SMART_PTR]    - unique_ptr, shared_ptr ownership
 *  [MOVE]         - move semantics, std::move, rvalue refs
 *  [INTERFACE]    - pure abstract IStorageEngine
 *  [FACTORY]      - StorageFactory design pattern
 *  [STRATEGY]     - pluggable serialization strategy
 *  [RAII]         - file handles, socket wrappers
 *  [TCP]          - POSIX socket server/client
 *  [THREAD]       - std::thread + std::mutex for concurrency
 *  [STL]          - vector, map, string, algorithm
 *  [SERIALIZE]    - custom binary serialization
 *  [VERSIONING]   - file versioning with metadata
 * ============================================================
 */

// ─── Standard includes ────────────────────────────────────────
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <memory>          // [SMART_PTR]
#include <mutex>           // [THREAD]
#include <thread>          // [THREAD]
#include <functional>
#include <stdexcept>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <algorithm>
#include <optional>

// ─── POSIX networking ──────────────────────────────────────────
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ══════════════════════════════════════════════════════════════
//  SECTION 1: DATA TYPES & SERIALIZATION [SERIALIZE] [STRATEGY]
// ══════════════════════════════════════════════════════════════

/**
 * FileVersion holds one snapshot of a file.
 * [MOVE] We use move semantics when storing large content blobs.
 */
struct FileVersion {
    uint32_t    version_number;
    std::string filename;
    std::string content;      // raw bytes as string
    std::time_t timestamp;
    size_t      size;

    // Default constructor
    FileVersion() = default;

    // [MOVE] Move constructor — transfers ownership of string data
    FileVersion(FileVersion&& other) noexcept
        : version_number(other.version_number),
          filename(std::move(other.filename)),   // [MOVE]
          content(std::move(other.content)),     // [MOVE]
          timestamp(other.timestamp),
          size(other.size)
    {}

    // [MOVE] Move assignment operator
    FileVersion& operator=(FileVersion&& other) noexcept {
        if (this != &other) {
            version_number = other.version_number;
            filename  = std::move(other.filename);   // [MOVE]
            content   = std::move(other.content);    // [MOVE]
            timestamp = other.timestamp;
            size      = other.size;
        }
        return *this;
    }

    // Copy constructor (needed for storage in containers)
    FileVersion(const FileVersion&) = default;
    FileVersion& operator=(const FileVersion&) = default;
};

// ─── Protocol message types ────────────────────────────────────
enum class MessageType : uint8_t {
    UPLOAD   = 1,
    DOWNLOAD = 2,
    LIST     = 3,
    RESPONSE = 4,
    ERROR    = 5
};

/**
 * [STRATEGY] ISerializer — abstract serialization interface.
 * Concrete strategies (BinarySerializer, TextSerializer) can be
 * swapped via StorageFactory without changing the rest of the system.
 */
class ISerializer {                             // [INTERFACE]
public:
    virtual ~ISerializer() = default;

    virtual std::vector<uint8_t> serialize(const FileVersion& fv) const = 0;
    virtual FileVersion          deserialize(const std::vector<uint8_t>& data) const = 0;

    // Serialize a simple string message
    virtual std::vector<uint8_t> serializeMessage(
        MessageType type, const std::string& payload) const = 0;

    virtual std::pair<MessageType, std::string>
        deserializeMessage(const std::vector<uint8_t>& data) const = 0;
};

/**
 * BinarySerializer — compact binary format.
 * Wire format for FileVersion:
 *   [4 bytes: version_number]
 *   [4 bytes: filename_len][filename_len bytes: filename]
 *   [8 bytes: size]
 *   [8 bytes: timestamp]
 *   [size bytes: content]
 *
 * Wire format for messages:
 *   [1 byte: MessageType]
 *   [4 bytes: payload_len]
 *   [payload_len bytes: payload]
 */
class BinarySerializer : public ISerializer {   // [INTERFACE]
public:
    std::vector<uint8_t> serialize(const FileVersion& fv) const override {
        std::vector<uint8_t> buf;

        auto write32 = [&](uint32_t v) {
            buf.push_back((v >> 24) & 0xFF);
            buf.push_back((v >> 16) & 0xFF);
            buf.push_back((v >>  8) & 0xFF);
            buf.push_back((v      ) & 0xFF);
        };
        auto write64 = [&](uint64_t v) {
            for (int i = 56; i >= 0; i -= 8)
                buf.push_back((v >> i) & 0xFF);
        };
        auto writeStr = [&](const std::string& s) {
            write32(static_cast<uint32_t>(s.size()));
            buf.insert(buf.end(), s.begin(), s.end());
        };

        write32(fv.version_number);
        writeStr(fv.filename);
        write64(static_cast<uint64_t>(fv.size));
        write64(static_cast<uint64_t>(fv.timestamp));
        writeStr(fv.content);
        return buf;
    }

    FileVersion deserialize(const std::vector<uint8_t>& data) const override {
        size_t pos = 0;

        auto read32 = [&]() -> uint32_t {
            uint32_t v = (static_cast<uint32_t>(data[pos])   << 24) |
                         (static_cast<uint32_t>(data[pos+1]) << 16) |
                         (static_cast<uint32_t>(data[pos+2]) <<  8) |
                         (static_cast<uint32_t>(data[pos+3]));
            pos += 4;
            return v;
        };
        auto read64 = [&]() -> uint64_t {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v = (v << 8) | data[pos++];
            return v;
        };
        auto readStr = [&]() -> std::string {
            uint32_t len = read32();
            std::string s(data.begin() + pos, data.begin() + pos + len);
            pos += len;
            return s;
        };

        FileVersion fv;
        fv.version_number = read32();
        fv.filename  = readStr();
        fv.size      = static_cast<size_t>(read64());
        fv.timestamp = static_cast<std::time_t>(read64());
        fv.content   = readStr();
        return fv;
    }

    std::vector<uint8_t> serializeMessage(
        MessageType type, const std::string& payload) const override
    {
        std::vector<uint8_t> buf;
        buf.push_back(static_cast<uint8_t>(type));
        uint32_t len = static_cast<uint32_t>(payload.size());
        buf.push_back((len >> 24) & 0xFF);
        buf.push_back((len >> 16) & 0xFF);
        buf.push_back((len >>  8) & 0xFF);
        buf.push_back((len      ) & 0xFF);
        buf.insert(buf.end(), payload.begin(), payload.end());
        return buf;
    }

    std::pair<MessageType, std::string>
        deserializeMessage(const std::vector<uint8_t>& data) const override
    {
        if (data.size() < 5) throw std::runtime_error("Message too short");
        MessageType type = static_cast<MessageType>(data[0]);
        uint32_t len = (static_cast<uint32_t>(data[1]) << 24) |
                       (static_cast<uint32_t>(data[2]) << 16) |
                       (static_cast<uint32_t>(data[3]) <<  8) |
                       (static_cast<uint32_t>(data[4]));
        std::string payload(data.begin() + 5, data.begin() + 5 + len);
        return {type, payload};
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 2: STORAGE ENGINE INTERFACE [INTERFACE]
// ══════════════════════════════════════════════════════════════

/**
 * [INTERFACE] IStorageEngine — pure abstract base class.
 * Concrete engines (LocalDiskEngine, InMemoryEngine) are
 * created by StorageFactory and injected into the server.
 * This decouples business logic from storage details.
 */
class IStorageEngine {
public:
    virtual ~IStorageEngine() = default;

    // Store a new version of a file. Returns assigned version number.
    virtual uint32_t store(FileVersion fv) = 0;

    // Retrieve a specific version. Returns nullopt if not found.
    virtual std::optional<FileVersion> retrieve(
        const std::string& filename, uint32_t version) const = 0;

    // Get latest version number for a file. Returns 0 if not found.
    virtual uint32_t latestVersion(const std::string& filename) const = 0;

    // List all versions for a file as (version, timestamp, size) tuples.
    virtual std::vector<std::tuple<uint32_t, std::time_t, size_t>>
        listVersions(const std::string& filename) const = 0;
};


// ══════════════════════════════════════════════════════════════
//  SECTION 3: CONCRETE STORAGE ENGINES
// ══════════════════════════════════════════════════════════════

/**
 * [THREAD] LocalDiskEngine — persists files to disk.
 * Uses a mutex to guard concurrent writes from multiple client threads.
 * Files are stored as: storage_root/<filename>.v<version>
 * Metadata (version list) in: storage_root/<filename>.meta
 */
class LocalDiskEngine : public IStorageEngine {   // [INTERFACE]
    std::string root_;
    mutable std::mutex mtx_;  // [THREAD] guard for concurrent access
    std::unique_ptr<ISerializer> ser_;  // [SMART_PTR]

    std::string versionPath(const std::string& fn, uint32_t v) const {
        return root_ + "/" + fn + ".v" + std::to_string(v);
    }
    std::string metaPath(const std::string& fn) const {
        return root_ + "/" + fn + ".meta";
    }

    // Read version count from meta file
    uint32_t readMeta(const std::string& fn) const {
        std::ifstream f(metaPath(fn));
        if (!f) return 0;
        uint32_t v = 0;
        f >> v;
        return v;
    }

    void writeMeta(const std::string& fn, uint32_t v) const {
        std::ofstream f(metaPath(fn));
        f << v;
    }

public:
    // [SMART_PTR] Takes ownership of serializer via unique_ptr
    explicit LocalDiskEngine(std::string root,
                             std::unique_ptr<ISerializer> ser)
        : root_(std::move(root)),       // [MOVE]
          ser_(std::move(ser))          // [MOVE] transfer ownership
    {
        fs::create_directories(root_);
    }

    uint32_t store(FileVersion fv) override {
        std::lock_guard<std::mutex> lock(mtx_);  // [THREAD] RAII lock

        uint32_t latest = readMeta(fv.filename);
        fv.version_number = latest + 1;
        fv.timestamp = std::time(nullptr);
        fv.size = fv.content.size();

        // Serialize and write to disk
        auto bytes = ser_->serialize(fv);
        std::ofstream out(versionPath(fv.filename, fv.version_number),
                          std::ios::binary);
        if (!out) throw std::runtime_error("Cannot open file for writing");
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());

        writeMeta(fv.filename, fv.version_number);
        std::cout << "[Storage] Stored " << fv.filename
                  << " v" << fv.version_number << " (" << fv.size << " bytes)\n";
        return fv.version_number;
    }

    std::optional<FileVersion> retrieve(
        const std::string& filename, uint32_t version) const override
    {
        std::lock_guard<std::mutex> lock(mtx_);  // [THREAD]
        std::string path = versionPath(filename, version);
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;

        std::vector<uint8_t> bytes(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        return ser_->deserialize(bytes);
    }

    uint32_t latestVersion(const std::string& filename) const override {
        std::lock_guard<std::mutex> lock(mtx_);
        return readMeta(filename);
    }

    std::vector<std::tuple<uint32_t, std::time_t, size_t>>
        listVersions(const std::string& filename) const override
    {
        std::lock_guard<std::mutex> lock(mtx_);
        uint32_t latest = readMeta(filename);
        std::vector<std::tuple<uint32_t, std::time_t, size_t>> result;

        for (uint32_t v = 1; v <= latest; ++v) {
            std::ifstream in(versionPath(filename, v), std::ios::binary);
            if (!in) continue;
            std::vector<uint8_t> bytes(
                (std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());
            auto fv = ser_->deserialize(bytes);
            result.emplace_back(fv.version_number, fv.timestamp, fv.size);
        }
        return result;
    }
};

/**
 * InMemoryEngine — stores files in RAM (good for tests/demos).
 * Key: filename → vector of FileVersion ordered by version number.
 * [STL] Uses std::map and std::vector throughout.
 */
class InMemoryEngine : public IStorageEngine {
    mutable std::mutex mtx_;
    // [STL] map: filename → list of versions
    std::map<std::string, std::vector<FileVersion>> store_;

public:
    uint32_t store(FileVersion fv) override {
        std::lock_guard<std::mutex> lock(mtx_);
        auto& versions = store_[fv.filename];
        fv.version_number = static_cast<uint32_t>(versions.size()) + 1;
        fv.timestamp = std::time(nullptr);
        fv.size = fv.content.size();
        versions.push_back(std::move(fv));   // [MOVE] avoid copying content
        return versions.back().version_number;
    }

    std::optional<FileVersion> retrieve(
        const std::string& filename, uint32_t version) const override
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = store_.find(filename);
        if (it == store_.end()) return std::nullopt;
        for (const auto& fv : it->second)
            if (fv.version_number == version) return fv;
        return std::nullopt;
    }

    uint32_t latestVersion(const std::string& filename) const override {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = store_.find(filename);
        if (it == store_.end() || it->second.empty()) return 0;
        return it->second.back().version_number;
    }

    std::vector<std::tuple<uint32_t, std::time_t, size_t>>
        listVersions(const std::string& filename) const override
    {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<std::tuple<uint32_t, std::time_t, size_t>> result;
        auto it = store_.find(filename);
        if (it == store_.end()) return result;
        for (const auto& fv : it->second)
            result.emplace_back(fv.version_number, fv.timestamp, fv.size);
        return result;
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 4: FACTORY PATTERN [FACTORY]
// ══════════════════════════════════════════════════════════════

/**
 * [FACTORY] StorageFactory — creates IStorageEngine instances.
 * Centralises construction decisions, decouples server code
 * from knowing which concrete type it gets.
 *
 * Usage:
 *   auto engine = StorageFactory::create("disk", "./storage");
 *   auto engine = StorageFactory::create("memory", "");
 */
class StorageFactory {
public:
    // [SMART_PTR] Returns unique_ptr — caller owns the engine
    static std::unique_ptr<IStorageEngine> create(
        const std::string& type,
        const std::string& config = "")
    {
        if (type == "disk") {
            auto serializer = std::make_unique<BinarySerializer>();  // [SMART_PTR]
            return std::make_unique<LocalDiskEngine>(
                config.empty() ? "./storage" : config,
                std::move(serializer));   // [MOVE]
        }
        if (type == "memory") {
            return std::make_unique<InMemoryEngine>();   // [SMART_PTR]
        }
        throw std::invalid_argument("Unknown storage type: " + type);
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 5: SOCKET RAII WRAPPER [RAII] [TCP]
// ══════════════════════════════════════════════════════════════

/**
 * [RAII] SocketHandle — wraps a raw fd so it closes automatically.
 * Demonstrates RAII: resource acquired in constructor, released in destructor.
 */
class SocketHandle {
    int fd_;
public:
    explicit SocketHandle(int fd) : fd_(fd) {}

    // [MOVE] Non-copyable, movable
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SocketHandle(SocketHandle&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    SocketHandle& operator=(SocketHandle&& o) noexcept {
        if (this != &o) { close(); fd_ = o.fd_; o.fd_ = -1; }
        return *this;
    }

    ~SocketHandle() { close(); }   // [RAII] automatic cleanup

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    void close() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
};

// ─── Low-level send/receive helpers ────────────────────────────

// Send exactly n bytes (handles partial writes)
bool sendAll(int fd, const void* buf, size_t n) {
    const char* ptr = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t sent = ::send(fd, ptr, n, 0);
        if (sent <= 0) return false;
        ptr += sent; n -= sent;
    }
    return true;
}

// Receive exactly n bytes
bool recvAll(int fd, void* buf, size_t n) {
    char* ptr = static_cast<char*>(buf);
    while (n > 0) {
        ssize_t got = ::recv(fd, ptr, n, 0);
        if (got <= 0) return false;
        ptr += got; n -= got;
    }
    return true;
}

/**
 * Send a length-prefixed message:
 *   [4 bytes: length][length bytes: data]
 */
bool sendMessage(int fd, const std::vector<uint8_t>& data) {
    uint32_t len = static_cast<uint32_t>(data.size());
    uint32_t net_len = htonl(len);
    if (!sendAll(fd, &net_len, 4)) return false;
    return sendAll(fd, data.data(), data.size());
}

/**
 * Receive a length-prefixed message.
 */
std::vector<uint8_t> recvMessage(int fd) {
    uint32_t net_len = 0;
    if (!recvAll(fd, &net_len, 4)) return {};
    uint32_t len = ntohl(net_len);
    if (len == 0 || len > 100 * 1024 * 1024) return {};  // sanity cap: 100MB
    std::vector<uint8_t> buf(len);
    if (!recvAll(fd, buf.data(), len)) return {};
    return buf;
}


// ══════════════════════════════════════════════════════════════
//  SECTION 6: REQUEST HANDLER [THREAD] [SMART_PTR]
// ══════════════════════════════════════════════════════════════

/**
 * [THREAD] RequestHandler — handles one client connection in its own thread.
 * Holds a shared_ptr to the engine so multiple threads share one store safely.
 */
class RequestHandler {
    int client_fd_;
    std::shared_ptr<IStorageEngine> engine_;  // [SMART_PTR] shared ownership
    BinarySerializer ser_;

    void sendError(const std::string& msg) {
        auto bytes = ser_.serializeMessage(MessageType::ERROR, msg);
        sendMessage(client_fd_, bytes);
    }

    void handleUpload(const std::string& payload) {
        // Payload: "<filename>\n<content>"
        size_t sep = payload.find('\n');
        if (sep == std::string::npos) { sendError("Bad upload payload"); return; }

        FileVersion fv;
        fv.filename = payload.substr(0, sep);
        fv.content  = payload.substr(sep + 1);   // [MOVE] could use std::move here

        uint32_t version = engine_->store(std::move(fv));  // [MOVE]
        std::string resp = "Stored as version " + std::to_string(version);
        sendMessage(client_fd_, ser_.serializeMessage(MessageType::RESPONSE, resp));
    }

    void handleDownload(const std::string& payload) {
        // Payload: "<filename> <version>"
        std::istringstream ss(payload);
        std::string filename; uint32_t version;
        ss >> filename >> version;

        auto fv = engine_->retrieve(filename, version);
        if (!fv) { sendError("File not found: " + filename + " v" + std::to_string(version)); return; }

        // Send the serialized FileVersion as response payload
        auto data = ser_.serialize(*fv);
        std::string as_str(data.begin(), data.end());
        sendMessage(client_fd_, ser_.serializeMessage(MessageType::RESPONSE, as_str));
    }

    void handleList(const std::string& filename) {
        auto versions = engine_->listVersions(filename);
        std::ostringstream out;
        out << "Versions of " << filename << ":\n";
        for (auto& [ver, ts, sz] : versions) {
            char tbuf[64];
            std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S",
                          std::localtime(&ts));
            out << "  v" << ver << "  " << tbuf << "  " << sz << " bytes\n";
        }
        sendMessage(client_fd_, ser_.serializeMessage(MessageType::RESPONSE, out.str()));
    }

public:
    RequestHandler(int fd, std::shared_ptr<IStorageEngine> eng)
        : client_fd_(fd), engine_(std::move(eng)) {}  // [MOVE] shared_ptr

    void run() {
        std::cout << "[Server] Client connected (fd=" << client_fd_ << ")\n";
        auto raw = recvMessage(client_fd_);
        if (raw.empty()) { ::close(client_fd_); return; }

        try {
            auto [type, payload] = ser_.deserializeMessage(raw);
            switch (type) {
                case MessageType::UPLOAD:   handleUpload(payload); break;
                case MessageType::DOWNLOAD: handleDownload(payload); break;
                case MessageType::LIST:     handleList(payload); break;
                default: sendError("Unknown message type");
            }
        } catch (const std::exception& e) {
            sendError(e.what());
        }

        ::close(client_fd_);
        std::cout << "[Server] Client disconnected (fd=" << client_fd_ << ")\n";
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 7: SERVER [TCP] [THREAD]
// ══════════════════════════════════════════════════════════════

/**
 * [TCP] FileServer — listens for incoming connections.
 * Spawns a new std::thread [THREAD] per client — simple but sufficient.
 * Production systems would use a thread pool (left as exercise).
 */
class FileServer {
    uint16_t port_;
    std::shared_ptr<IStorageEngine> engine_;  // [SMART_PTR] shared across threads

public:
    FileServer(uint16_t port, std::unique_ptr<IStorageEngine> engine)
        : port_(port),
          engine_(std::move(engine))   // [MOVE] unique→shared via move
    {}

    void run() {
        // [TCP] Create TCP socket
        SocketHandle server(::socket(AF_INET, SOCK_STREAM, 0));  // [RAII]
        if (!server.valid()) throw std::runtime_error("socket() failed");

        int opt = 1;
        ::setsockopt(server.get(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);

        if (::bind(server.get(), reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) < 0)
            throw std::runtime_error("bind() failed");

        if (::listen(server.get(), 16) < 0)
            throw std::runtime_error("listen() failed");

        std::cout << "[Server] Listening on port " << port_ << "\n";
        std::cout << "[Server] Storage engine ready\n";

        while (true) {
            sockaddr_in client_addr{};
            socklen_t   client_len = sizeof(client_addr);
            int client_fd = ::accept(server.get(),
                                     reinterpret_cast<sockaddr*>(&client_addr),
                                     &client_len);
            if (client_fd < 0) continue;

            // [THREAD] Spawn a thread per client
            // Capture engine_ by value (shared_ptr — safe, ref-counted)
            std::thread([client_fd, eng = engine_]() mutable {
                RequestHandler handler(client_fd, std::move(eng));
                handler.run();
            }).detach();
        }
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 8: CLIENT [TCP]
// ══════════════════════════════════════════════════════════════

/**
 * [TCP] FileClient — connects to server and sends requests.
 */
class FileClient {
    std::string host_;
    uint16_t    port_;
    BinarySerializer ser_;

    SocketHandle connect() {
        SocketHandle sock(::socket(AF_INET, SOCK_STREAM, 0));  // [RAII]
        if (!sock.valid()) throw std::runtime_error("socket() failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port_);
        if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0)
            throw std::runtime_error("Invalid address: " + host_);

        if (::connect(sock.get(), reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) < 0)
            throw std::runtime_error("connect() failed — is the server running?");

        return sock;   // [MOVE] RVO / NRVO
    }

    std::string transact(MessageType type, const std::string& payload) {
        auto sock = connect();
        auto msg  = ser_.serializeMessage(type, payload);
        sendMessage(sock.get(), msg);

        auto raw = recvMessage(sock.get());
        if (raw.empty()) throw std::runtime_error("Empty response from server");

        auto [rtype, rpayload] = ser_.deserializeMessage(raw);
        if (rtype == MessageType::ERROR)
            throw std::runtime_error("Server error: " + rpayload);

        return rpayload;
    }

public:
    FileClient(std::string host, uint16_t port)
        : host_(std::move(host)), port_(port) {}  // [MOVE]

    // Upload a local file. Returns the version number assigned.
    uint32_t upload(const std::string& local_path) {
        std::ifstream f(local_path, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot open: " + local_path);
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        std::string filename = fs::path(local_path).filename().string();
        std::string payload  = filename + "\n" + content;

        std::string resp = transact(MessageType::UPLOAD, payload);
        std::cout << "[Client] " << resp << "\n";
        return 0;  // simplified; parse version from resp in production
    }

    // Download a specific version and save to disk.
    void download(const std::string& filename, uint32_t version,
                  const std::string& out_dir = ".") {
        std::string payload = filename + " " + std::to_string(version);
        std::string raw = transact(MessageType::DOWNLOAD, payload);

        std::vector<uint8_t> bytes(raw.begin(), raw.end());
        FileVersion fv = ser_.deserialize(bytes);

        std::string out_path = out_dir + "/" + fv.filename +
                               ".v" + std::to_string(fv.version_number);
        std::ofstream out(out_path, std::ios::binary);
        out.write(fv.content.data(), fv.content.size());
        std::cout << "[Client] Downloaded " << fv.filename
                  << " v" << fv.version_number << " → " << out_path << "\n";
    }

    // List all versions of a file.
    void listVersions(const std::string& filename) {
        std::string resp = transact(MessageType::LIST, filename);
        std::cout << resp;
    }
};


// ══════════════════════════════════════════════════════════════
//  SECTION 9: MAIN ENTRY POINT
// ══════════════════════════════════════════════════════════════

// Runtime flow for the commands below:
//   g++ -std=c++17 -pthread -o mini_dropbox mini_dropbox.cpp
//   ./mini_dropbox server 8080 disk ./storage
//   ./mini_dropbox upload 127.0.0.1 8080 notes.txt
//   ./mini_dropbox list 127.0.0.1 8080 notes.txt
//   ./mini_dropbox download 127.0.0.1 8080 notes.txt 2
//
// 1. `main()` reads the first argument and chooses server, upload, download,
//    or list mode.
// 2. Server mode creates the requested storage engine. With `disk`, versions
//    are written to `./storage` as numbered files plus a small metadata file.
// 3. Upload, list, and download modes each open one TCP connection, send one
//    request, receive one response, and exit.
// 4. Versions are created in order, so `download ... notes.txt 2` retrieves the
//    second stored snapshot of `notes.txt`.

void printUsage(const char* prog) {
    std::cerr << "Usage:\n"
              << "  " << prog << " server <port> [disk|memory] [storage_dir]\n"
              << "  " << prog << " upload  <host> <port> <local_file>\n"
              << "  " << prog << " download <host> <port> <filename> <version>\n"
              << "  " << prog << " list     <host> <port> <filename>\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) { printUsage(argv[0]); return 1; }

    std::string mode = argv[1];

    try {
        // ── SERVER MODE ──────────────────────────────────────
        if (mode == "server") {
            if (argc < 3) { printUsage(argv[0]); return 1; }
            uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));

            std::string engine_type = (argc >= 4) ? argv[3] : "disk";
            std::string storage_dir = (argc >= 5) ? argv[4] : "./storage";

            // [FACTORY] Engine created here — server doesn't know which type
            auto engine = StorageFactory::create(engine_type, storage_dir);
            FileServer server(port, std::move(engine));  // [MOVE]
            server.run();
        }

        // ── UPLOAD MODE ──────────────────────────────────────
        else if (mode == "upload") {
            if (argc < 5) { printUsage(argv[0]); return 1; }
            FileClient client(argv[2], static_cast<uint16_t>(std::stoi(argv[3])));
            client.upload(argv[4]);
        }

        // ── DOWNLOAD MODE ────────────────────────────────────
        else if (mode == "download") {
            if (argc < 6) { printUsage(argv[0]); return 1; }
            FileClient client(argv[2], static_cast<uint16_t>(std::stoi(argv[3])));
            uint32_t version = static_cast<uint32_t>(std::stoi(argv[5]));
            client.download(argv[4], version);
        }

        // ── LIST MODE ────────────────────────────────────────
        else if (mode == "list") {
            if (argc < 5) { printUsage(argv[0]); return 1; }
            FileClient client(argv[2], static_cast<uint16_t>(std::stoi(argv[3])));
            client.listVersions(argv[4]);
        }

        else { printUsage(argv[0]); return 1; }

    } catch (const std::exception& e) {
        std::cerr << "[Error] " << e.what() << "\n";
        return 1;
    }
    return 0;
}

/*
 * ============================================================
 *  EXTENSION IDEAS (for lab assignments or extra credit)
 * ============================================================
 *
 *  1. Thread pool  — replace detach() with a fixed-size thread pool
 *     to limit resource consumption. Uses condition_variable + queue.
 *
 *  2. Delta versioning — instead of storing full content per version,
 *     store diffs. Reduces disk usage dramatically for text files.
 *
 *  3. Compression — add a CompressingSerializer that wraps the binary
 *     serializer and applies zlib. Demonstrates the Decorator pattern.
 *
 *  4. Authentication — add a username/password handshake phase before
 *     processing any request. Uses HMAC or simple token comparison.
 *
 *  5. Async I/O — replace blocking recv/send with epoll (Linux) or
 *     kqueue (macOS) to handle thousands of concurrent connections
 *     without one thread per client.
 *
 *  6. SQLite metadata — replace .meta files with a SQLite database
 *     for atomic multi-file queries and proper ACID semantics.
 * ============================================================
 */

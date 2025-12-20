/*
 * Pulse Multiplayer - Common Network Definitions
 * Cross-platform UDP networking with host-client architecture
 */
#ifndef NET_COMMON_HPP
#define NET_COMMON_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <queue>
#include <mutex>
#include <atomic>

// Cross-platform socket includes
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET socket_t;
    #define SOCKET_INVALID INVALID_SOCKET
    #define SOCKET_ERROR_CODE WSAGetLastError()
    #define CLOSE_SOCKET closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    typedef int socket_t;
    #define SOCKET_INVALID (-1)
    #define SOCKET_ERROR_CODE errno
    #define CLOSE_SOCKET close
#endif

namespace pulse {
namespace net {

// ============================================================================
// Constants & Configuration
// ============================================================================

constexpr uint16_t DEFAULT_PORT = 7777;
constexpr size_t MAX_PACKET_SIZE = 1400;  // Safe MTU
constexpr size_t MAX_PLAYERS = 16;
constexpr uint32_t TICK_RATE = 60;        // Server ticks per second
constexpr float TICK_INTERVAL = 1.0f / TICK_RATE;
constexpr uint32_t SNAPSHOT_RATE = 20;    // Snapshots per second
constexpr float SNAPSHOT_INTERVAL = 1.0f / SNAPSHOT_RATE;
constexpr float CONNECTION_TIMEOUT = 10.0f;
constexpr float HEARTBEAT_INTERVAL = 1.0f;
constexpr uint32_t INPUT_BUFFER_SIZE = 64;
constexpr uint32_t STATE_BUFFER_SIZE = 128;
constexpr float INTERPOLATION_DELAY = 0.1f; // 100ms interpolation buffer

// ============================================================================
// Packet Types
// ============================================================================

enum class PacketType : uint8_t {
    // Connection
    CONNECT_REQUEST = 0x01,
    CONNECT_ACCEPT = 0x02,
    CONNECT_REJECT = 0x03,
    DISCONNECT = 0x04,
    HEARTBEAT = 0x05,
    
    // Game State
    INPUT = 0x10,
    STATE_UPDATE = 0x11,
    WORLD_SNAPSHOT = 0x12,
    
    // Events
    ENTITY_CREATE = 0x20,
    ENTITY_DESTROY = 0x21,
    EVENT_BROADCAST = 0x22,
    
    // Reliability
    ACK = 0x30,
    RELIABLE_DATA = 0x31,
};

// ============================================================================
// Entity & Player State
// ============================================================================

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    
    Vec3 operator+(const Vec3& o) const { return Vec3(x+o.x, y+o.y, z+o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x-o.x, y-o.y, z-o.z); }
    Vec3 operator*(float s) const { return Vec3(x*s, y*s, z*s); }
    
    static Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
        return a + (b - a) * t;
    }
};

struct PlayerInput {
    uint32_t sequence;      // Input sequence number
    uint32_t tick;          // Server tick this input is for
    uint8_t keys;           // Bitfield: W=1, S=2, A=4, D=8, SPACE=16, SHIFT=32
    float yaw, pitch;       // Look direction
    float deltaTime;        // Client delta time
};

struct PlayerState {
    uint32_t playerId;
    uint32_t tick;          // Server tick of this state
    Vec3 position;
    float yaw, pitch;
    uint32_t lastProcessedInput;  // Last input sequence processed
};

struct EntityState {
    uint32_t entityId;
    uint8_t entityType;     // 0=player, 1=cube, etc.
    Vec3 position;
    Vec3 velocity;
    float yaw, pitch;
};

// ============================================================================
// Packet Header
// ============================================================================

struct PacketHeader {
    uint8_t magic[4];       // "PULS"
    PacketType type;
    uint32_t sequence;      // Packet sequence number
    uint32_t ack;           // Last received sequence
    uint32_t ackBits;       // Bitfield of received packets
    uint32_t tick;          // Current server tick
    uint16_t payloadSize;
    
    PacketHeader() : type(PacketType::HEARTBEAT), sequence(0), ack(0), 
                     ackBits(0), tick(0), payloadSize(0) {
        magic[0] = 'P'; magic[1] = 'U'; magic[2] = 'L'; magic[3] = 'S';
    }
    
    bool isValid() const {
        return magic[0] == 'P' && magic[1] == 'U' && 
               magic[2] == 'L' && magic[3] == 'S';
    }
};

// ============================================================================
// Serialization Buffer
// ============================================================================

class PacketBuffer {
public:
    uint8_t data[MAX_PACKET_SIZE];
    size_t writePos = 0;
    size_t readPos = 0;
    
    void reset() { writePos = 0; readPos = 0; }
    size_t size() const { return writePos; }
    
    // Write operations
    void writeU8(uint8_t v) { if (writePos < MAX_PACKET_SIZE) data[writePos++] = v; }
    void writeU16(uint16_t v) { writeU8(v & 0xFF); writeU8((v >> 8) & 0xFF); }
    void writeU32(uint32_t v) { writeU16(v & 0xFFFF); writeU16((v >> 16) & 0xFFFF); }
    void writeFloat(float v) { uint32_t u; memcpy(&u, &v, 4); writeU32(u); }
    void writeVec3(const Vec3& v) { writeFloat(v.x); writeFloat(v.y); writeFloat(v.z); }
    void writeBytes(const void* src, size_t len) {
        if (writePos + len <= MAX_PACKET_SIZE) {
            memcpy(data + writePos, src, len);
            writePos += len;
        }
    }
    
    // Read operations
    uint8_t readU8() { return readPos < writePos ? data[readPos++] : 0; }
    uint16_t readU16() { uint16_t v = readU8(); return v | (readU8() << 8); }
    uint32_t readU32() { uint32_t v = readU16(); return v | (readU16() << 16); }
    float readFloat() { uint32_t u = readU32(); float f; memcpy(&f, &u, 4); return f; }
    Vec3 readVec3() { 
        float x = readFloat(); 
        float y = readFloat(); 
        float z = readFloat(); 
        return Vec3(x, y, z); 
    }
    void readBytes(void* dst, size_t len) {
        if (readPos + len <= writePos) {
            memcpy(dst, data + readPos, len);
            readPos += len;
        }
    }
    
    // Header operations
    void writeHeader(const PacketHeader& h) {
        writeBytes(h.magic, 4);
        writeU8(static_cast<uint8_t>(h.type));
        writeU32(h.sequence);
        writeU32(h.ack);
        writeU32(h.ackBits);
        writeU32(h.tick);
        writeU16(h.payloadSize);
    }
    
    PacketHeader readHeader() {
        PacketHeader h;
        readBytes(h.magic, 4);
        h.type = static_cast<PacketType>(readU8());
        h.sequence = readU32();
        h.ack = readU32();
        h.ackBits = readU32();
        h.tick = readU32();
        h.payloadSize = readU16();
        return h;
    }
    
    // Payload serialization
    void writePlayerInput(const PlayerInput& input) {
        writeU32(input.sequence);
        writeU32(input.tick);
        writeU8(input.keys);
        writeFloat(input.yaw);
        writeFloat(input.pitch);
        writeFloat(input.deltaTime);
    }
    
    PlayerInput readPlayerInput() {
        PlayerInput input;
        input.sequence = readU32();
        input.tick = readU32();
        input.keys = readU8();
        input.yaw = readFloat();
        input.pitch = readFloat();
        input.deltaTime = readFloat();
        return input;
    }
    
    void writePlayerState(const PlayerState& state) {
        writeU32(state.playerId);
        writeU32(state.tick);
        writeVec3(state.position);
        writeFloat(state.yaw);
        writeFloat(state.pitch);
        writeU32(state.lastProcessedInput);
    }
    
    PlayerState readPlayerState() {
        PlayerState state;
        state.playerId = readU32();
        state.tick = readU32();
        state.position = readVec3();
        state.yaw = readFloat();
        state.pitch = readFloat();
        state.lastProcessedInput = readU32();
        return state;
    }
    
    void writeEntityState(const EntityState& e) {
        writeU32(e.entityId);
        writeU8(e.entityType);
        writeVec3(e.position);
        writeVec3(e.velocity);
        writeFloat(e.yaw);
        writeFloat(e.pitch);
    }
    
    EntityState readEntityState() {
        EntityState e;
        e.entityId = readU32();
        e.entityType = readU8();
        e.position = readVec3();
        e.velocity = readVec3();
        e.yaw = readFloat();
        e.pitch = readFloat();
        return e;
    }
};

// ============================================================================
// Connection State
// ============================================================================

enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    DISCONNECTING
};

struct Connection {
    uint32_t playerId;
    sockaddr_in address;
    ConnectionState state;
    
    uint32_t localSequence;
    uint32_t remoteSequence;
    uint32_t ackBits;
    
    float lastReceiveTime;
    float lastSendTime;
    float rtt;              // Round trip time estimate
    
    // Input buffer for server-side processing
    std::queue<PlayerInput> pendingInputs;
    uint32_t lastProcessedInput;
    
    Connection() : playerId(0), state(ConnectionState::DISCONNECTED),
                   localSequence(0), remoteSequence(0), ackBits(0),
                   lastReceiveTime(0), lastSendTime(0), rtt(0.1f),
                   lastProcessedInput(0) {
        memset(&address, 0, sizeof(address));
    }
};

// ============================================================================
// Interpolation State
// ============================================================================

struct InterpolationState {
    PlayerState states[STATE_BUFFER_SIZE];
    uint32_t count = 0;
    
    void addState(const PlayerState& state) {
        // Circular buffer
        states[count % STATE_BUFFER_SIZE] = state;
        count++;
    }
    
    bool interpolate(uint32_t targetTick, PlayerState& out) const {
        if (count < 2) return false;
        
        // Find two states to interpolate between
        const PlayerState* before = nullptr;
        const PlayerState* after = nullptr;
        
        for (uint32_t i = 0; i < std::min(count, (uint32_t)STATE_BUFFER_SIZE); i++) {
            const PlayerState& s = states[(count - 1 - i) % STATE_BUFFER_SIZE];
            if (s.tick <= targetTick) {
                before = &s;
                if (i > 0) {
                    after = &states[(count - i) % STATE_BUFFER_SIZE];
                }
                break;
            }
        }
        
        if (!before) return false;
        if (!after) {
            out = *before;
            return true;
        }
        
        float t = (float)(targetTick - before->tick) / (float)(after->tick - before->tick);
        t = std::max(0.0f, std::min(1.0f, t));
        
        out.playerId = before->playerId;
        out.tick = targetTick;
        out.position = Vec3::lerp(before->position, after->position, t);
        out.yaw = before->yaw + (after->yaw - before->yaw) * t;
        out.pitch = before->pitch + (after->pitch - before->pitch) * t;
        out.lastProcessedInput = after->lastProcessedInput;
        
        return true;
    }
};

// ============================================================================
// Input History (for client-side prediction & rollback)
// ============================================================================

struct InputHistory {
    PlayerInput inputs[INPUT_BUFFER_SIZE];
    PlayerState predictedStates[INPUT_BUFFER_SIZE];
    uint32_t head = 0;
    uint32_t count = 0;
    
    void addInput(const PlayerInput& input, const PlayerState& predicted) {
        uint32_t idx = (head + count) % INPUT_BUFFER_SIZE;
        inputs[idx] = input;
        predictedStates[idx] = predicted;
        if (count < INPUT_BUFFER_SIZE) count++;
        else head = (head + 1) % INPUT_BUFFER_SIZE;
    }
    
    // Remove inputs that have been acknowledged
    void acknowledgeUpTo(uint32_t sequence) {
        while (count > 0 && inputs[head].sequence <= sequence) {
            head = (head + 1) % INPUT_BUFFER_SIZE;
            count--;
        }
    }
    
    // Get unacknowledged inputs for resending
    std::vector<PlayerInput> getUnacknowledged() const {
        std::vector<PlayerInput> result;
        for (uint32_t i = 0; i < count; i++) {
            result.push_back(inputs[(head + i) % INPUT_BUFFER_SIZE]);
        }
        return result;
    }
};

// ============================================================================
// Socket Utilities
// ============================================================================

inline bool initSockets() {
#ifdef _WIN32
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
    return true;
#endif
}

inline void shutdownSockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

inline socket_t createUdpSocket() {
    socket_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == SOCKET_INVALID) return SOCKET_INVALID;
    
    // Set non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
    
    // Allow address reuse
    int optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval));
    
    return sock;
}

inline bool bindSocket(socket_t sock, uint16_t port) {
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    return bind(sock, (sockaddr*)&addr, sizeof(addr)) == 0;
}

inline bool addressEqual(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

inline std::string addressToString(const sockaddr_in& addr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s:%d", 
             inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    return buf;
}

} // namespace net
} // namespace pulse

#endif // NET_COMMON_HPP

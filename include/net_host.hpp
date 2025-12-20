/*
 * Pulse Multiplayer - Host (Server) Implementation
 * Authoritative game server with tick-based simulation
 */
#ifndef NET_HOST_HPP
#define NET_HOST_HPP

#include "net_common.hpp"
#include <functional>
#include <cmath>

namespace pulse {
namespace net {

// Event callback types
using OnPlayerConnected = std::function<void(uint32_t playerId)>;
using OnPlayerDisconnected = std::function<void(uint32_t playerId)>;

class Host {
public:
    Host() : socket_(SOCKET_INVALID), running_(false), currentTick_(0),
             nextPlayerId_(1), tickAccumulator_(0), snapshotAccumulator_(0) {}
    
    ~Host() { stop(); }
    
    bool start(uint16_t port = DEFAULT_PORT) {
        if (!initSockets()) {
            printf("[Host] Failed to init sockets\n");
            return false;
        }
        
        socket_ = createUdpSocket();
        if (socket_ == SOCKET_INVALID) {
            printf("[Host] Failed to create socket\n");
            return false;
        }
        
        if (!bindSocket(socket_, port)) {
            printf("[Host] Failed to bind to port %d\n", port);
            CLOSE_SOCKET(socket_);
            socket_ = SOCKET_INVALID;
            return false;
        }
        
        running_ = true;
        currentTick_ = 0;
        startTime_ = std::chrono::steady_clock::now();
        printf("[Host] Started on port %d\n", port);
        return true;
    }
    
    void stop() {
        if (socket_ != SOCKET_INVALID) {
            // Send disconnect to all clients
            for (auto& [id, conn] : connections_) {
                sendDisconnect(conn);
            }
            CLOSE_SOCKET(socket_);
            socket_ = SOCKET_INVALID;
        }
        connections_.clear();
        players_.clear();
        running_ = false;
        shutdownSockets();
        printf("[Host] Stopped\n");
    }
    
    void update(float deltaTime) {
        if (!running_) return;
        
        float now = getTime();
        
        // Receive packets
        receivePackets(now);
        
        // Check for timeouts
        checkTimeouts(now);
        
        // Fixed timestep tick update
        tickAccumulator_ += deltaTime;
        while (tickAccumulator_ >= TICK_INTERVAL) {
            processTick();
            tickAccumulator_ -= TICK_INTERVAL;
        }
        
        // Send snapshots at lower rate
        snapshotAccumulator_ += deltaTime;
        if (snapshotAccumulator_ >= SNAPSHOT_INTERVAL) {
            sendStateUpdates();
            snapshotAccumulator_ -= SNAPSHOT_INTERVAL;
        }
        
        // Send heartbeats
        for (auto& [id, conn] : connections_) {
            if (conn.state == ConnectionState::CONNECTED &&
                now - conn.lastSendTime >= HEARTBEAT_INTERVAL) {
                sendHeartbeat(conn);
            }
        }
    }
    
    // Get player states for rendering
    const std::unordered_map<uint32_t, PlayerState>& getPlayers() const {
        return players_;
    }
    
    // Get local player state (host is player 0)
    PlayerState& getLocalPlayer() {
        if (players_.find(0) == players_.end()) {
            players_[0] = PlayerState();
            players_[0].playerId = 0;
            players_[0].position = Vec3(0, 1.7f, 5);
            players_[0].yaw = -90.0f;
            players_[0].pitch = 0.0f;
        }
        return players_[0];
    }
    
    // Process local host input
    void processLocalInput(const PlayerInput& input) {
        applyInput(0, input);
    }
    
    uint32_t getCurrentTick() const { return currentTick_; }
    bool isRunning() const { return running_; }
    size_t getPlayerCount() const { return players_.size(); }
    
    // Callbacks
    OnPlayerConnected onPlayerConnected;
    OnPlayerDisconnected onPlayerDisconnected;

private:
    socket_t socket_;
    std::atomic<bool> running_;
    uint32_t currentTick_;
    uint32_t nextPlayerId_;
    float tickAccumulator_;
    float snapshotAccumulator_;
    std::chrono::steady_clock::time_point startTime_;
    
    std::unordered_map<uint32_t, Connection> connections_;
    std::unordered_map<uint32_t, PlayerState> players_;
    
    float getTime() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(now - startTime_).count();
    }
    
    void receivePackets(float now) {
        PacketBuffer buffer;
        sockaddr_in fromAddr;
        socklen_t fromLen = sizeof(fromAddr);
        
        while (true) {
            ssize_t received = recvfrom(socket_, (char*)buffer.data, MAX_PACKET_SIZE,
                                        0, (sockaddr*)&fromAddr, &fromLen);
            if (received <= 0) break;
            
            buffer.writePos = received;
            buffer.readPos = 0;
            
            PacketHeader header = buffer.readHeader();
            if (!header.isValid()) continue;
            
            handlePacket(header, buffer, fromAddr, now);
        }
    }
    
    void handlePacket(const PacketHeader& header, PacketBuffer& buffer,
                      const sockaddr_in& fromAddr, float now) {
        // Find or create connection
        Connection* conn = findConnection(fromAddr);
        
        switch (header.type) {
            case PacketType::CONNECT_REQUEST:
                handleConnectRequest(fromAddr, now);
                break;
                
            case PacketType::DISCONNECT:
                if (conn) handleDisconnect(*conn);
                break;
                
            case PacketType::HEARTBEAT:
                if (conn) {
                    conn->lastReceiveTime = now;
                    updateAcks(*conn, header);
                }
                break;
                
            case PacketType::INPUT:
                if (conn && conn->state == ConnectionState::CONNECTED) {
                    conn->lastReceiveTime = now;
                    updateAcks(*conn, header);
                    handleInput(*conn, buffer, header.payloadSize);
                }
                break;
                
            case PacketType::ACK:
                if (conn) {
                    conn->lastReceiveTime = now;
                    updateAcks(*conn, header);
                }
                break;
                
            default:
                break;
        }
    }
    
    Connection* findConnection(const sockaddr_in& addr) {
        for (auto& [id, conn] : connections_) {
            if (addressEqual(conn.address, addr)) {
                return &conn;
            }
        }
        return nullptr;
    }
    
    void handleConnectRequest(const sockaddr_in& fromAddr, float now) {
        // Check if already connected
        Connection* existing = findConnection(fromAddr);
        if (existing && existing->state == ConnectionState::CONNECTED) {
            sendConnectAccept(*existing);
            return;
        }
        
        // Create new player
        uint32_t playerId = nextPlayerId_++;
        
        Connection& conn = connections_[playerId];
        conn.playerId = playerId;
        conn.address = fromAddr;
        conn.state = ConnectionState::CONNECTED;
        conn.lastReceiveTime = now;
        conn.localSequence = 0;
        conn.remoteSequence = 0;
        
        // Create player state
        players_[playerId] = PlayerState();
        players_[playerId].playerId = playerId;
        players_[playerId].position = Vec3(0, 1.7f, 5);
        players_[playerId].yaw = -90.0f;
        players_[playerId].pitch = 0.0f;
        
        printf("[Host] Player %d connected from %s\n", 
               playerId, addressToString(fromAddr).c_str());
        
        sendConnectAccept(conn);
        sendWorldSnapshot(conn);
        
        // Broadcast new player to others
        broadcastEntityCreate(playerId);
        
        if (onPlayerConnected) onPlayerConnected(playerId);
    }
    
    void handleDisconnect(Connection& conn) {
        printf("[Host] Player %d disconnected\n", conn.playerId);
        
        uint32_t playerId = conn.playerId;
        
        // Broadcast entity destroy
        broadcastEntityDestroy(playerId);
        
        players_.erase(playerId);
        connections_.erase(playerId);
        
        if (onPlayerDisconnected) onPlayerDisconnected(playerId);
    }
    
    void handleInput(Connection& conn, PacketBuffer& buffer, uint16_t payloadSize) {
        // Read multiple inputs (redundant sending for packet loss)
        size_t inputCount = payloadSize / 21; // Size of serialized PlayerInput
        
        for (size_t i = 0; i < inputCount; i++) {
            PlayerInput input = buffer.readPlayerInput();
            
            // Only process inputs newer than last processed
            if (input.sequence > conn.lastProcessedInput) {
                conn.pendingInputs.push(input);
            }
        }
    }
    
    void processTick() {
        currentTick_++;
        
        // Process pending inputs from all clients
        for (auto& [id, conn] : connections_) {
            while (!conn.pendingInputs.empty()) {
                PlayerInput input = conn.pendingInputs.front();
                conn.pendingInputs.pop();
                
                if (input.sequence > conn.lastProcessedInput) {
                    applyInput(id, input);
                    conn.lastProcessedInput = input.sequence;
                    
                    if (players_.find(id) != players_.end()) {
                        players_[id].lastProcessedInput = input.sequence;
                    }
                }
            }
        }
    }
    
    void applyInput(uint32_t playerId, const PlayerInput& input) {
        auto it = players_.find(playerId);
        if (it == players_.end()) return;
        
        PlayerState& state = it->second;
        
        float velocity = 5.0f * input.deltaTime;
        float yawRad = input.yaw * M_PI / 180.0f;
        
        // Forward/backward
        if (input.keys & 0x01) { // W
            state.position.x += cosf(yawRad) * velocity;
            state.position.z += sinf(yawRad) * velocity;
        }
        if (input.keys & 0x02) { // S
            state.position.x -= cosf(yawRad) * velocity;
            state.position.z -= sinf(yawRad) * velocity;
        }
        
        // Left/right (fixed direction)
        if (input.keys & 0x04) { // A - left
            state.position.x += sinf(yawRad) * velocity;
            state.position.z -= cosf(yawRad) * velocity;
        }
        if (input.keys & 0x08) { // D - right
            state.position.x -= sinf(yawRad) * velocity;
            state.position.z += cosf(yawRad) * velocity;
        }
        
        // Up/down
        if (input.keys & 0x10) state.position.y += velocity; // Space
        if (input.keys & 0x20) state.position.y -= velocity; // Shift
        
        state.yaw = input.yaw;
        state.pitch = input.pitch;
        state.tick = currentTick_;
    }
    
    void sendStateUpdates() {
        for (auto& [connId, conn] : connections_) {
            if (conn.state != ConnectionState::CONNECTED) continue;
            
            PacketBuffer buffer;
            PacketHeader header;
            header.type = PacketType::STATE_UPDATE;
            header.sequence = ++conn.localSequence;
            header.ack = conn.remoteSequence;
            header.ackBits = conn.ackBits;
            header.tick = currentTick_;
            
            // Reserve space for header
            size_t headerPos = buffer.writePos;
            buffer.writePos += 22; // Header size
            
            // Write all player states
            uint8_t playerCount = static_cast<uint8_t>(players_.size());
            buffer.writeU8(playerCount);
            
            for (const auto& [playerId, state] : players_) {
                buffer.writePlayerState(state);
            }
            
            // Update header with payload size
            header.payloadSize = buffer.writePos - headerPos - 22;
            
            // Write header at start
            size_t savedPos = buffer.writePos;
            buffer.writePos = headerPos;
            buffer.writeHeader(header);
            buffer.writePos = savedPos;
            
            sendTo(conn, buffer);
        }
    }
    
    void sendConnectAccept(Connection& conn) {
        PacketBuffer buffer;
        PacketHeader header;
        header.type = PacketType::CONNECT_ACCEPT;
        header.sequence = ++conn.localSequence;
        header.tick = currentTick_;
        
        buffer.writeHeader(header);
        buffer.writeU32(conn.playerId);
        buffer.writeU32(currentTick_);
        
        sendTo(conn, buffer);
        conn.lastSendTime = getTime();
    }
    
    void sendDisconnect(Connection& conn) {
        PacketBuffer buffer;
        PacketHeader header;
        header.type = PacketType::DISCONNECT;
        header.sequence = ++conn.localSequence;
        
        buffer.writeHeader(header);
        sendTo(conn, buffer);
    }
    
    void sendHeartbeat(Connection& conn) {
        PacketBuffer buffer;
        PacketHeader header;
        header.type = PacketType::HEARTBEAT;
        header.sequence = ++conn.localSequence;
        header.ack = conn.remoteSequence;
        header.ackBits = conn.ackBits;
        header.tick = currentTick_;
        
        buffer.writeHeader(header);
        sendTo(conn, buffer);
        conn.lastSendTime = getTime();
    }
    
    void sendWorldSnapshot(Connection& conn) {
        PacketBuffer buffer;
        PacketHeader header;
        header.type = PacketType::WORLD_SNAPSHOT;
        header.sequence = ++conn.localSequence;
        header.tick = currentTick_;
        
        // Reserve space for header
        size_t headerPos = buffer.writePos;
        buffer.writePos += 22;
        
        // Write all players
        buffer.writeU8(static_cast<uint8_t>(players_.size()));
        for (const auto& [id, state] : players_) {
            buffer.writePlayerState(state);
        }
        
        // Write world entities (static cubes)
        buffer.writeU8(3); // 3 static cubes
        EntityState cube1 = {1, 1, Vec3(0, 1, 0), Vec3(), 0, 0};
        EntityState cube2 = {2, 1, Vec3(5, 1, 3), Vec3(), 0, 0};
        EntityState cube3 = {3, 1, Vec3(-3, 0.5f, -5), Vec3(), 0, 0};
        buffer.writeEntityState(cube1);
        buffer.writeEntityState(cube2);
        buffer.writeEntityState(cube3);
        
        header.payloadSize = buffer.writePos - headerPos - 22;
        size_t savedPos = buffer.writePos;
        buffer.writePos = headerPos;
        buffer.writeHeader(header);
        buffer.writePos = savedPos;
        
        sendTo(conn, buffer);
    }
    
    void broadcastEntityCreate(uint32_t entityId) {
        for (auto& [connId, conn] : connections_) {
            if (conn.state != ConnectionState::CONNECTED) continue;
            if (connId == entityId) continue; // Don't send to self
            
            PacketBuffer buffer;
            PacketHeader header;
            header.type = PacketType::ENTITY_CREATE;
            header.sequence = ++conn.localSequence;
            header.tick = currentTick_;
            
            buffer.writeHeader(header);
            buffer.writeU32(entityId);
            buffer.writeU8(0); // Player type
            buffer.writeVec3(players_[entityId].position);
            
            sendTo(conn, buffer);
        }
    }
    
    void broadcastEntityDestroy(uint32_t entityId) {
        for (auto& [connId, conn] : connections_) {
            if (conn.state != ConnectionState::CONNECTED) continue;
            
            PacketBuffer buffer;
            PacketHeader header;
            header.type = PacketType::ENTITY_DESTROY;
            header.sequence = ++conn.localSequence;
            header.tick = currentTick_;
            
            buffer.writeHeader(header);
            buffer.writeU32(entityId);
            
            sendTo(conn, buffer);
        }
    }
    
    void updateAcks(Connection& conn, const PacketHeader& header) {
        if (header.sequence > conn.remoteSequence) {
            // Update ack bits
            uint32_t shift = header.sequence - conn.remoteSequence;
            if (shift < 32) {
                conn.ackBits = (conn.ackBits << shift) | 1;
            } else {
                conn.ackBits = 1;
            }
            conn.remoteSequence = header.sequence;
        } else if (header.sequence < conn.remoteSequence) {
            uint32_t diff = conn.remoteSequence - header.sequence;
            if (diff < 32) {
                conn.ackBits |= (1 << diff);
            }
        }
    }
    
    void checkTimeouts(float now) {
        std::vector<uint32_t> toRemove;
        
        for (auto& [id, conn] : connections_) {
            if (now - conn.lastReceiveTime > CONNECTION_TIMEOUT) {
                printf("[Host] Player %d timed out\n", id);
                toRemove.push_back(id);
            }
        }
        
        for (uint32_t id : toRemove) {
            broadcastEntityDestroy(id);
            players_.erase(id);
            connections_.erase(id);
            if (onPlayerDisconnected) onPlayerDisconnected(id);
        }
    }
    
    void sendTo(const Connection& conn, const PacketBuffer& buffer) {
        sendto(socket_, (const char*)buffer.data, buffer.writePos, 0,
               (const sockaddr*)&conn.address, sizeof(conn.address));
    }
};

} // namespace net
} // namespace pulse

#endif // NET_HOST_HPP

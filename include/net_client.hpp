/*
 * Pulse Multiplayer - Client Implementation
 * Client-side prediction, interpolation, and rollback
 */
#ifndef NET_CLIENT_HPP
#define NET_CLIENT_HPP

#include "net_common.hpp"
#include <functional>
#include <cmath>

namespace pulse {
namespace net {

// Event callback types
using OnConnected = std::function<void(uint32_t playerId)>;
using OnDisconnected = std::function<void()>;
using OnEntityCreated = std::function<void(uint32_t entityId, uint8_t type, Vec3 pos)>;
using OnEntityDestroyed = std::function<void(uint32_t entityId)>;

class Client {
public:
    Client() : socket_(SOCKET_INVALID), state_(ConnectionState::DISCONNECTED),
               playerId_(0), serverTick_(0), localSequence_(0), remoteSequence_(0),
               inputSequence_(0), lastSendTime_(0), lastReceiveTime_(0) {}
    
    ~Client() { disconnect(); }
    
    bool connect(const std::string& host, uint16_t port = DEFAULT_PORT) {
        if (!initSockets()) {
            printf("[Client] Failed to init sockets\n");
            return false;
        }
        
        socket_ = createUdpSocket();
        if (socket_ == SOCKET_INVALID) {
            printf("[Client] Failed to create socket\n");
            return false;
        }
        
        // Bind to any port
        if (!bindSocket(socket_, 0)) {
            printf("[Client] Failed to bind socket\n");
            CLOSE_SOCKET(socket_);
            socket_ = SOCKET_INVALID;
            return false;
        }
        
        // Set server address
        memset(&serverAddr_, 0, sizeof(serverAddr_));
        serverAddr_.sin_family = AF_INET;
        serverAddr_.sin_port = htons(port);
        
        if (inet_pton(AF_INET, host.c_str(), &serverAddr_.sin_addr) <= 0) {
            // Try localhost
            serverAddr_.sin_addr.s_addr = inet_addr("127.0.0.1");
        }
        
        state_ = ConnectionState::CONNECTING;
        startTime_ = std::chrono::steady_clock::now();
        connectStartTime_ = getTime();
        
        printf("[Client] Connecting to %s:%d...\n", host.c_str(), port);
        
        // Send connect request
        sendConnectRequest();
        
        return true;
    }
    
    void disconnect() {
        if (state_ != ConnectionState::DISCONNECTED) {
            sendDisconnect();
            state_ = ConnectionState::DISCONNECTED;
            
            if (onDisconnected) onDisconnected();
        }
        
        if (socket_ != SOCKET_INVALID) {
            CLOSE_SOCKET(socket_);
            socket_ = SOCKET_INVALID;
        }
        
        shutdownSockets();
        remotePlayers_.clear();
        interpolationStates_.clear();
        inputHistory_.count = 0;
        inputHistory_.head = 0;
        
        printf("[Client] Disconnected\n");
    }
    
    void update(float deltaTime) {
        if (state_ == ConnectionState::DISCONNECTED) return;
        
        float now = getTime();
        
        // Receive packets
        receivePackets(now);
        
        // Handle connection timeout
        if (state_ == ConnectionState::CONNECTING) {
            if (now - connectStartTime_ > CONNECTION_TIMEOUT) {
                printf("[Client] Connection timeout\n");
                disconnect();
                return;
            }
            // Retry connect request
            if (now - lastSendTime_ > 1.0f) {
                sendConnectRequest();
            }
        } else if (state_ == ConnectionState::CONNECTED) {
            // Check for timeout
            if (now - lastReceiveTime_ > CONNECTION_TIMEOUT) {
                printf("[Client] Server timeout\n");
                disconnect();
                return;
            }
            
            // Update interpolation for remote players
            updateInterpolation(deltaTime);
            
            // Send heartbeat
            if (now - lastSendTime_ > HEARTBEAT_INTERVAL) {
                sendHeartbeat();
            }
        }
    }
    
    // Send input to server and apply prediction locally
    void sendInput(const PlayerInput& input) {
        if (state_ != ConnectionState::CONNECTED) return;
        
        // Create input with sequence
        PlayerInput seqInput = input;
        seqInput.sequence = ++inputSequence_;
        seqInput.tick = serverTick_;
        
        // Apply prediction locally
        PlayerState predicted = localState_;
        applyInputToState(predicted, seqInput);
        predicted.tick = serverTick_;
        
        // Store in history for rollback
        inputHistory_.addInput(seqInput, predicted);
        
        // Update local state with prediction
        localState_ = predicted;
        
        // Send input packet with redundancy (last N inputs)
        PacketBuffer buffer;
        PacketHeader header;
        header.type = PacketType::INPUT;
        header.sequence = ++localSequence_;
        header.ack = remoteSequence_;
        header.ackBits = ackBits_;
        header.tick = serverTick_;
        
        // Reserve space for header
        size_t headerPos = buffer.writePos;
        buffer.writePos += 22;
        
        // Write redundant inputs for packet loss handling
        auto unacked = inputHistory_.getUnacknowledged();
        size_t count = std::min(unacked.size(), (size_t)5); // Send up to 5 recent inputs
        for (size_t i = unacked.size() - count; i < unacked.size(); i++) {
            buffer.writePlayerInput(unacked[i]);
        }
        
        header.payloadSize = buffer.writePos - headerPos - 22;
        size_t savedPos = buffer.writePos;
        buffer.writePos = headerPos;
        buffer.writeHeader(header);
        buffer.writePos = savedPos;
        
        sendToServer(buffer);
    }
    
    // Get local player state (predicted)
    const PlayerState& getLocalState() const { return localState_; }
    PlayerState& getLocalState() { return localState_; }
    
    // Get interpolated remote player states
    std::unordered_map<uint32_t, PlayerState> getInterpolatedPlayers() const {
        std::unordered_map<uint32_t, PlayerState> result;
        
        // Calculate render tick with interpolation delay
        uint32_t renderTick = serverTick_ > (uint32_t)(INTERPOLATION_DELAY / TICK_INTERVAL) 
                            ? serverTick_ - (uint32_t)(INTERPOLATION_DELAY / TICK_INTERVAL) 
                            : 0;
        
        for (const auto& [id, interpState] : interpolationStates_) {
            if (id == playerId_) continue; // Skip local player
            
            PlayerState interpolated;
            if (interpState.interpolate(renderTick, interpolated)) {
                result[id] = interpolated;
            } else if (remotePlayers_.find(id) != remotePlayers_.end()) {
                result[id] = remotePlayers_.at(id);
            }
        }
        
        return result;
    }
    
    bool isConnected() const { return state_ == ConnectionState::CONNECTED; }
    bool isConnecting() const { return state_ == ConnectionState::CONNECTING; }
    uint32_t getPlayerId() const { return playerId_; }
    uint32_t getServerTick() const { return serverTick_; }
    size_t getPlayerCount() const { return remotePlayers_.size() + 1; }
    float getRtt() const { return rtt_; }
    
    // Callbacks
    OnConnected onConnected;
    OnDisconnected onDisconnected;
    OnEntityCreated onEntityCreated;
    OnEntityDestroyed onEntityDestroyed;

private:
    socket_t socket_;
    sockaddr_in serverAddr_;
    ConnectionState state_;
    uint32_t playerId_;
    uint32_t serverTick_;
    uint32_t localSequence_;
    uint32_t remoteSequence_;
    uint32_t ackBits_;
    uint32_t inputSequence_;
    float lastSendTime_;
    float lastReceiveTime_;
    float connectStartTime_;
    float rtt_ = 0.1f;
    
    std::chrono::steady_clock::time_point startTime_;
    
    PlayerState localState_;
    PlayerState lastServerState_; // For reconciliation
    std::unordered_map<uint32_t, PlayerState> remotePlayers_;
    std::unordered_map<uint32_t, InterpolationState> interpolationStates_;
    InputHistory inputHistory_;
    
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
            
            handlePacket(header, buffer, now);
        }
    }
    
    void handlePacket(const PacketHeader& header, PacketBuffer& buffer, float now) {
        lastReceiveTime_ = now;
        updateAcks(header);
        
        switch (header.type) {
            case PacketType::CONNECT_ACCEPT:
                handleConnectAccept(buffer, header);
                break;
                
            case PacketType::CONNECT_REJECT:
                printf("[Client] Connection rejected\n");
                disconnect();
                break;
                
            case PacketType::DISCONNECT:
                printf("[Client] Server disconnected\n");
                disconnect();
                break;
                
            case PacketType::HEARTBEAT:
                // Just update timing
                break;
                
            case PacketType::STATE_UPDATE:
                handleStateUpdate(buffer, header);
                break;
                
            case PacketType::WORLD_SNAPSHOT:
                handleWorldSnapshot(buffer);
                break;
                
            case PacketType::ENTITY_CREATE:
                handleEntityCreate(buffer);
                break;
                
            case PacketType::ENTITY_DESTROY:
                handleEntityDestroy(buffer);
                break;
                
            default:
                break;
        }
    }
    
    void handleConnectAccept(PacketBuffer& buffer, const PacketHeader& header) {
        playerId_ = buffer.readU32();
        serverTick_ = buffer.readU32();
        
        state_ = ConnectionState::CONNECTED;
        
        // Initialize local state
        localState_.playerId = playerId_;
        localState_.position = Vec3(0, 1.7f, 5);
        localState_.yaw = -90.0f;
        localState_.pitch = 0.0f;
        localState_.tick = serverTick_;
        
        printf("[Client] Connected as player %d, tick %d\n", playerId_, serverTick_);
        
        if (onConnected) onConnected(playerId_);
    }
    
    void handleStateUpdate(PacketBuffer& buffer, const PacketHeader& header) {
        serverTick_ = header.tick;
        
        uint8_t playerCount = buffer.readU8();
        
        for (uint8_t i = 0; i < playerCount; i++) {
            PlayerState state = buffer.readPlayerState();
            
            if (state.playerId == playerId_) {
                // This is our state from server - reconcile
                reconcileState(state);
            } else {
                // Remote player - add to interpolation buffer
                remotePlayers_[state.playerId] = state;
                interpolationStates_[state.playerId].addState(state);
            }
        }
    }
    
    void handleWorldSnapshot(PacketBuffer& buffer) {
        // Read players
        uint8_t playerCount = buffer.readU8();
        for (uint8_t i = 0; i < playerCount; i++) {
            PlayerState state = buffer.readPlayerState();
            
            if (state.playerId == playerId_) {
                localState_ = state;
                lastServerState_ = state;
            } else {
                remotePlayers_[state.playerId] = state;
                interpolationStates_[state.playerId].addState(state);
            }
        }
        
        // Read entities
        uint8_t entityCount = buffer.readU8();
        for (uint8_t i = 0; i < entityCount; i++) {
            EntityState entity = buffer.readEntityState();
            if (onEntityCreated) {
                onEntityCreated(entity.entityId, entity.entityType, entity.position);
            }
        }
        
        printf("[Client] Received world snapshot: %d players, %d entities\n", 
               playerCount, entityCount);
    }
    
    void handleEntityCreate(PacketBuffer& buffer) {
        uint32_t entityId = buffer.readU32();
        uint8_t type = buffer.readU8();
        Vec3 pos = buffer.readVec3();
        
        if (type == 0) { // Player
            remotePlayers_[entityId] = PlayerState();
            remotePlayers_[entityId].playerId = entityId;
            remotePlayers_[entityId].position = pos;
        }
        
        if (onEntityCreated) onEntityCreated(entityId, type, pos);
        
        printf("[Client] Entity %d created (type %d)\n", entityId, type);
    }
    
    void handleEntityDestroy(PacketBuffer& buffer) {
        uint32_t entityId = buffer.readU32();
        
        remotePlayers_.erase(entityId);
        interpolationStates_.erase(entityId);
        
        if (onEntityDestroyed) onEntityDestroyed(entityId);
        
        printf("[Client] Entity %d destroyed\n", entityId);
    }
    
    void reconcileState(const PlayerState& serverState) {
        lastServerState_ = serverState;
        
        // Check if server acknowledged our inputs
        inputHistory_.acknowledgeUpTo(serverState.lastProcessedInput);
        
        // Calculate position error
        Vec3 error = serverState.position - localState_.position;
        float errorMag = sqrtf(error.x*error.x + error.y*error.y + error.z*error.z);
        
        // If error is significant, perform rollback
        if (errorMag > 0.01f) {
            // Start from server state
            PlayerState corrected = serverState;
            
            // Re-apply unacknowledged inputs
            auto unacked = inputHistory_.getUnacknowledged();
            for (const auto& input : unacked) {
                applyInputToState(corrected, input);
            }
            
            // Smooth correction to avoid visual pop
            if (errorMag < 1.0f) {
                // Blend towards corrected position
                float blendFactor = 0.1f;
                localState_.position.x += (corrected.position.x - localState_.position.x) * blendFactor;
                localState_.position.y += (corrected.position.y - localState_.position.y) * blendFactor;
                localState_.position.z += (corrected.position.z - localState_.position.z) * blendFactor;
            } else {
                // Large error - snap to corrected
                localState_.position = corrected.position;
            }
        }
    }
    
    void applyInputToState(PlayerState& state, const PlayerInput& input) {
        float velocity = 5.0f * input.deltaTime;
        float yawRad = input.yaw * M_PI / 180.0f;
        
        if (input.keys & 0x01) { // W
            state.position.x += cosf(yawRad) * velocity;
            state.position.z += sinf(yawRad) * velocity;
        }
        if (input.keys & 0x02) { // S
            state.position.x -= cosf(yawRad) * velocity;
            state.position.z -= sinf(yawRad) * velocity;
        }
        if (input.keys & 0x04) { // A - left
            state.position.x += sinf(yawRad) * velocity;
            state.position.z -= cosf(yawRad) * velocity;
        }
        if (input.keys & 0x08) { // D - right
            state.position.x -= sinf(yawRad) * velocity;
            state.position.z += cosf(yawRad) * velocity;
        }
        if (input.keys & 0x10) state.position.y += velocity;
        if (input.keys & 0x20) state.position.y -= velocity;
        
        state.yaw = input.yaw;
        state.pitch = input.pitch;
    }
    
    void updateInterpolation(float deltaTime) {
        // Interpolation is handled in getInterpolatedPlayers()
        // Nothing additional needed here
        (void)deltaTime;
    }
    
    void updateAcks(const PacketHeader& header) {
        if (header.sequence > remoteSequence_) {
            uint32_t shift = header.sequence - remoteSequence_;
            if (shift < 32) {
                ackBits_ = (ackBits_ << shift) | 1;
            } else {
                ackBits_ = 1;
            }
            remoteSequence_ = header.sequence;
        } else if (header.sequence < remoteSequence_) {
            uint32_t diff = remoteSequence_ - header.sequence;
            if (diff < 32) {
                ackBits_ |= (1 << diff);
            }
        }
    }
    
    void sendConnectRequest() {
        PacketBuffer buffer;
        PacketHeader header;
        header.type = PacketType::CONNECT_REQUEST;
        header.sequence = ++localSequence_;
        
        buffer.writeHeader(header);
        sendToServer(buffer);
        lastSendTime_ = getTime();
    }
    
    void sendDisconnect() {
        PacketBuffer buffer;
        PacketHeader header;
        header.type = PacketType::DISCONNECT;
        header.sequence = ++localSequence_;
        
        buffer.writeHeader(header);
        sendToServer(buffer);
    }
    
    void sendHeartbeat() {
        PacketBuffer buffer;
        PacketHeader header;
        header.type = PacketType::HEARTBEAT;
        header.sequence = ++localSequence_;
        header.ack = remoteSequence_;
        header.ackBits = ackBits_;
        
        buffer.writeHeader(header);
        sendToServer(buffer);
        lastSendTime_ = getTime();
    }
    
    void sendToServer(const PacketBuffer& buffer) {
        sendto(socket_, (const char*)buffer.data, buffer.writePos, 0,
               (const sockaddr*)&serverAddr_, sizeof(serverAddr_));
    }
};

} // namespace net
} // namespace pulse

#endif // NET_CLIENT_HPP

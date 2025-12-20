/*
 * Pulse Multiplayer - Main Game
 * Host-client multiplayer 3D sandbox with UDP networking
 * 
 * Usage:
 *   ./multiplayer host [port]     - Start as host
 *   ./multiplayer client [ip] [port] - Connect as client
 */

#define _USE_MATH_DEFINES
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glu.h>

#include "net_common.hpp"
#include "net_host.hpp"
#include "net_client.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace pulse::net;

// ============================================================================
// Game State
// ============================================================================

enum class GameMode {
    HOST,
    CLIENT
};

struct GameState {
    GameMode mode;
    Host* host = nullptr;
    Client* client = nullptr;
    
    float yaw = -90.0f;
    float pitch = 0.0f;
    float lastX = 640, lastY = 360;
    bool firstMouse = true;
    float deltaTime = 0.0f;
    float lastFrame = 0.0f;
    
    // Cursor capture: true = captured (mouse controls camera, cursor hidden)
    bool cursorCaptured = true;
    bool prevTabState = false; // for edge detection of toggle key
    
    // Statistics
    float fps = 0.0f;
    int playerCount = 0;
};

static GameState game;

// ============================================================================
// Input Handling
// ============================================================================

void mouseCallback(GLFWwindow* window, double xpos, double ypos) {
    (void)window;
    // Ignore mouse movement when cursor is not captured
    if (!game.cursorCaptured) return;
    
    if (game.firstMouse) {
        game.lastX = (float)xpos;
        game.lastY = (float)ypos;
        game.firstMouse = false;
    }
    
    float xoffset = (float)xpos - game.lastX;
    float yoffset = game.lastY - (float)ypos;
    game.lastX = (float)xpos;
    game.lastY = (float)ypos;
    
    float sensitivity = 0.1f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;
    
    game.yaw += xoffset;
    game.pitch += yoffset;
    
    if (game.pitch > 89.0f) game.pitch = 89.0f;
    if (game.pitch < -89.0f) game.pitch = -89.0f;
}

PlayerInput getInput(GLFWwindow* window, float deltaTime) {
    PlayerInput input;
    input.sequence = 0;
    input.tick = 0;
    input.keys = 0;
    input.yaw = game.yaw;
    input.pitch = game.pitch;
    input.deltaTime = deltaTime;
    
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) input.keys |= 0x01;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) input.keys |= 0x02;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) input.keys |= 0x04;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) input.keys |= 0x08;
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) input.keys |= 0x10;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) input.keys |= 0x20;
    
    return input;
}

// ============================================================================
// Rendering
// ============================================================================

void drawGrid() {
    glColor3f(0.3f, 0.3f, 0.3f);
    glBegin(GL_LINES);
    for (int i = -20; i <= 20; i++) {
        glVertex3f((float)i, 0.0f, -20.0f);
        glVertex3f((float)i, 0.0f, 20.0f);
        glVertex3f(-20.0f, 0.0f, (float)i);
        glVertex3f(20.0f, 0.0f, (float)i);
    }
    glEnd();
}

void drawCube(float x, float y, float z, float size, float r, float g, float b) {
    float h = size / 2.0f;
    glBegin(GL_QUADS);
    
    glColor3f(r, g * 0.8f, b * 0.8f); // Front
    glVertex3f(x-h, y-h, z+h); glVertex3f(x+h, y-h, z+h);
    glVertex3f(x+h, y+h, z+h); glVertex3f(x-h, y+h, z+h);
    
    glColor3f(r * 0.8f, g, b * 0.8f); // Back
    glVertex3f(x-h, y-h, z-h); glVertex3f(x-h, y+h, z-h);
    glVertex3f(x+h, y+h, z-h); glVertex3f(x+h, y-h, z-h);
    
    glColor3f(r * 0.8f, g * 0.8f, b); // Top
    glVertex3f(x-h, y+h, z-h); glVertex3f(x-h, y+h, z+h);
    glVertex3f(x+h, y+h, z+h); glVertex3f(x+h, y+h, z-h);
    
    glColor3f(r * 0.6f, g * 0.6f, b * 0.6f); // Bottom
    glVertex3f(x-h, y-h, z-h); glVertex3f(x+h, y-h, z-h);
    glVertex3f(x+h, y-h, z+h); glVertex3f(x-h, y-h, z+h);
    
    glColor3f(r * 0.7f, g * 0.7f, b); // Right
    glVertex3f(x+h, y-h, z-h); glVertex3f(x+h, y+h, z-h);
    glVertex3f(x+h, y+h, z+h); glVertex3f(x+h, y-h, z+h);
    
    glColor3f(r, g * 0.7f, b * 0.7f); // Left
    glVertex3f(x-h, y-h, z-h); glVertex3f(x-h, y-h, z+h);
    glVertex3f(x-h, y+h, z+h); glVertex3f(x-h, y+h, z-h);
    
    glEnd();
}

void drawPlayer(const PlayerState& state, bool isLocal) {
    // Draw player as a colored cube
    float r = isLocal ? 0.2f : 0.8f;
    float g = isLocal ? 0.8f : 0.2f;
    float b = 0.2f;
    
    // Body
    drawCube(state.position.x, state.position.y - 0.5f, state.position.z, 0.6f, r, g, b);
    
    // Head
    drawCube(state.position.x, state.position.y + 0.1f, state.position.z, 0.4f, r*1.2f, g*1.2f, b*1.2f);
    
    // Direction indicator
    float yawRad = state.yaw * (float)M_PI / 180.0f;
    float dirX = state.position.x + cosf(yawRad) * 0.5f;
    float dirZ = state.position.z + sinf(yawRad) * 0.5f;
    
    glColor3f(1.0f, 1.0f, 0.0f);
    glBegin(GL_LINES);
    glVertex3f(state.position.x, state.position.y, state.position.z);
    glVertex3f(dirX, state.position.y, dirZ);
    glEnd();
}

void render(const Vec3& cameraPos, float yaw, float pitch) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    
    float yawRad = yaw * (float)M_PI / 180.0f;
    float pitchRad = pitch * (float)M_PI / 180.0f;
    float frontX = cosf(yawRad) * cosf(pitchRad);
    float frontY = sinf(pitchRad);
    float frontZ = sinf(yawRad) * cosf(pitchRad);
    
    gluLookAt(cameraPos.x, cameraPos.y, cameraPos.z,
              cameraPos.x + frontX, cameraPos.y + frontY, cameraPos.z + frontZ,
              0.0f, 1.0f, 0.0f);
    
    // Draw grid
    drawGrid();
    
    // Draw static cubes
    drawCube(0, 1, 0, 2, 1.0f, 0.0f, 0.0f);
    drawCube(5, 1, 3, 1.5f, 0.0f, 1.0f, 0.0f);
    drawCube(-3, 0.5f, -5, 1, 0.0f, 0.0f, 1.0f);
    
    // Draw players
    if (game.mode == GameMode::HOST && game.host) {
        // Draw all players except local
        for (const auto& [id, state] : game.host->getPlayers()) {
            if (id != 0) { // Don't draw local host player (we're the camera)
                drawPlayer(state, false);
            }
        }
    } else if (game.mode == GameMode::CLIENT && game.client) {
        // Draw interpolated remote players
        auto remotePlayers = game.client->getInterpolatedPlayers();
        for (const auto& [id, state] : remotePlayers) {
            drawPlayer(state, false);
        }
    }
}

// ============================================================================
// Main
// ============================================================================

void printUsage(const char* programName) {
    printf("Pulse Multiplayer\n");
    printf("Usage:\n");
    printf("  %s host [port]           - Start as host (default port: 7777)\n", programName);
    printf("  %s client [ip] [port]    - Connect as client\n", programName);
    printf("\nControls:\n");
    printf("  WASD - Move\n");
    printf("  Space/Shift - Up/Down\n");
    printf("  Mouse - Look around\n");
    printf("  ESC - Exit\n");
}

int main(int argc, char* argv[]) {
    // Parse arguments
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string modeStr = argv[1];
    uint16_t port = DEFAULT_PORT;
    std::string hostIp = "127.0.0.1";
    
    if (modeStr == "host") {
        game.mode = GameMode::HOST;
        if (argc >= 3) port = (uint16_t)atoi(argv[2]);
    } else if (modeStr == "client") {
        game.mode = GameMode::CLIENT;
        if (argc >= 3) hostIp = argv[2];
        if (argc >= 4) port = (uint16_t)atoi(argv[3]);
    } else {
        printUsage(argv[0]);
        return 1;
    }
    
    // Initialize GLFW
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }
    
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Pulse Multiplayer", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return -1;
    }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0); // Disable VSync
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    game.cursorCaptured = true;
    glfwSetCursorPosCallback(window, mouseCallback);
    
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, 1280.0/720.0, 0.1, 100.0);
    
    // Initialize networking
    if (game.mode == GameMode::HOST) {
        game.host = new Host();
        if (!game.host->start(port)) {
            fprintf(stderr, "Failed to start host\n");
            delete game.host;
            glfwTerminate();
            return -1;
        }
        
        game.host->onPlayerConnected = [](uint32_t id) {
            printf("Player %d joined the game\n", id);
        };
        game.host->onPlayerDisconnected = [](uint32_t id) {
            printf("Player %d left the game\n", id);
        };
        
        // Initialize host player
        game.host->getLocalPlayer();
        
    } else {
        game.client = new Client();
        if (!game.client->connect(hostIp, port)) {
            fprintf(stderr, "Failed to connect\n");
            delete game.client;
            glfwTerminate();
            return -1;
        }
        
        game.client->onConnected = [](uint32_t id) {
            printf("Connected as player %d\n", id);
        };
        game.client->onDisconnected = []() {
            printf("Disconnected from server\n");
        };
        game.client->onEntityCreated = [](uint32_t id, uint8_t type, Vec3 pos) {
            printf("Entity %d (type %d) created at (%.1f, %.1f, %.1f)\n", 
                   id, type, pos.x, pos.y, pos.z);
        };
    }
    
    printf("\n=== Pulse Multiplayer ===\n");
    printf("Mode: %s\n", game.mode == GameMode::HOST ? "HOST" : "CLIENT");
    if (game.mode == GameMode::HOST) {
        printf("Port: %d\n", port);
    } else {
        printf("Server: %s:%d\n", hostIp.c_str(), port);
    }
    printf("Controls: WASD + Mouse, ESC to exit\n\n");
    
    // Main loop
    while (!glfwWindowShouldClose(window)) {
        float currentFrame = (float)glfwGetTime();
        game.deltaTime = currentFrame - game.lastFrame;
        game.lastFrame = currentFrame;
        game.fps = game.deltaTime > 0 ? 1.0f / game.deltaTime : 0;
        
        // Check for ESC
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, 1);
        }
        
        // Toggle cursor capture with TAB (edge-detected)
        bool tabPressed = (glfwGetKey(window, GLFW_KEY_TAB) == GLFW_PRESS);
        if (tabPressed && !game.prevTabState) {
            // Toggle
            if (game.cursorCaptured) {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                game.cursorCaptured = false;
                printf("[Input] Cursor released (OS control)\n");
            } else {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                game.cursorCaptured = true;
                // Reset firstMouse so we don't get a large jump
                game.firstMouse = true;
                double cx, cy; glfwGetCursorPos(window, &cx, &cy);
                game.lastX = (float)cx; game.lastY = (float)cy;
                printf("[Input] Cursor captured (mouse controls camera)\n");
            }
        }
        game.prevTabState = tabPressed;
        
        // Get input
        PlayerInput input = getInput(window, game.deltaTime);
        
        Vec3 cameraPos;
        float cameraYaw = game.yaw;
        float cameraPitch = game.pitch;
        
        if (game.mode == GameMode::HOST) {
            // Update host
            game.host->update(game.deltaTime);
            game.host->processLocalInput(input);
            
            PlayerState& localPlayer = game.host->getLocalPlayer();
            cameraPos = localPlayer.position;
            localPlayer.yaw = game.yaw;
            localPlayer.pitch = game.pitch;
            
            game.playerCount = (int)game.host->getPlayerCount();
            
        } else {
            // Update client
            game.client->update(game.deltaTime);
            
            if (game.client->isConnected()) {
                game.client->sendInput(input);
                
                const PlayerState& localState = game.client->getLocalState();
                cameraPos = localState.position;
                
                // Update local state with current look direction
                game.client->getLocalState().yaw = game.yaw;
                game.client->getLocalState().pitch = game.pitch;
                
                game.playerCount = (int)game.client->getPlayerCount();
            } else if (game.client->isConnecting()) {
                cameraPos = Vec3(0, 1.7f, 5);
            } else {
                // Disconnected
                glfwSetWindowShouldClose(window, 1);
            }
        }
        
        // Render
        render(cameraPos, cameraYaw, cameraPitch);
        
        // Update window title
        char title[256];
        const char* modeLabel = game.mode == GameMode::HOST ? "HOST" : "CLIENT";
        const char* status = "";
        if (game.mode == GameMode::CLIENT) {
            if (game.client->isConnecting()) status = " (Connecting...)";
            else if (!game.client->isConnected()) status = " (Disconnected)";
        }
        snprintf(title, sizeof(title), "Pulse Multiplayer [%s] - Players: %d - FPS: %.0f%s",
                 modeLabel, game.playerCount, game.fps, status);
        glfwSetWindowTitle(window, title);
        
        glfwSwapBuffers(window);
        glfwPollEvents();
    }
    
    // Cleanup
    if (game.host) {
        game.host->stop();
        delete game.host;
    }
    if (game.client) {
        game.client->disconnect();
        delete game.client;
    }
    
    glfwDestroyWindow(window);
    glfwTerminate();
    
    printf("Goodbye!\n");
    return 0;
}

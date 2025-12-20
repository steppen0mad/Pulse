#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glu.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Camera state
typedef struct {
    float pos[3];
    float yaw, pitch;
    float speed;
} Camera;

Camera camera = {{0.0f, 1.7f, 5.0f}, -90.0f, 0.0f, 5.0f};
float lastX = 400, lastY = 300;
int firstMouse = 1;
float deltaTime = 0.0f;
float lastFrame = 0.0f;

void processInput(GLFWwindow* window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, 1);
    
    float velocity = camera.speed * deltaTime;
    float yawRad = camera.yaw * M_PI / 180.0f;
    
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
        camera.pos[0] += cosf(yawRad) * velocity;
        camera.pos[2] += sinf(yawRad) * velocity;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
        camera.pos[0] -= cosf(yawRad) * velocity;
        camera.pos[2] -= sinf(yawRad) * velocity;
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
        camera.pos[0] += sinf(yawRad) * velocity;
        camera.pos[2] -= cosf(yawRad) * velocity;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
        camera.pos[0] -= sinf(yawRad) * velocity;
        camera.pos[2] += cosf(yawRad) * velocity;
    }
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
        camera.pos[1] += velocity;
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        camera.pos[1] -= velocity;
}

void mouseCallback(GLFWwindow* window, double xpos, double ypos) {
    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = 0;
    }
    
    float xoffset = xpos - lastX;
    float yoffset = lastY - ypos;
    lastX = xpos;
    lastY = ypos;
    
    float sensitivity = 0.1f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;
    
    camera.yaw += xoffset;
    camera.pitch += yoffset;
    
    if (camera.pitch > 89.0f) camera.pitch = 89.0f;
    if (camera.pitch < -89.0f) camera.pitch = -89.0f;
}

void drawGrid() {
    glColor3f(0.3f, 0.3f, 0.3f);
    glBegin(GL_LINES);
    for (int i = -20; i <= 20; i++) {
        glVertex3f(i, 0, -20);
        glVertex3f(i, 0, 20);
        glVertex3f(-20, 0, i);
        glVertex3f(20, 0, i);
    }
    glEnd();
}

void drawCube(float x, float y, float z, float size) {
    float h = size / 2.0f;
    glBegin(GL_QUADS);
    
    glColor3f(1.0f, 0.0f, 0.0f); // Front
    glVertex3f(x-h, y-h, z+h); glVertex3f(x+h, y-h, z+h);
    glVertex3f(x+h, y+h, z+h); glVertex3f(x-h, y+h, z+h);
    
    glColor3f(0.0f, 1.0f, 0.0f); // Back
    glVertex3f(x-h, y-h, z-h); glVertex3f(x-h, y+h, z-h);
    glVertex3f(x+h, y+h, z-h); glVertex3f(x+h, y-h, z-h);
    
    glColor3f(0.0f, 0.0f, 1.0f); // Top
    glVertex3f(x-h, y+h, z-h); glVertex3f(x-h, y+h, z+h);
    glVertex3f(x+h, y+h, z+h); glVertex3f(x+h, y+h, z-h);
    
    glColor3f(1.0f, 1.0f, 0.0f); // Bottom
    glVertex3f(x-h, y-h, z-h); glVertex3f(x+h, y-h, z-h);
    glVertex3f(x+h, y-h, z+h); glVertex3f(x-h, y-h, z+h);
    
    glColor3f(1.0f, 0.0f, 1.0f); // Right
    glVertex3f(x+h, y-h, z-h); glVertex3f(x+h, y+h, z-h);
    glVertex3f(x+h, y+h, z+h); glVertex3f(x+h, y-h, z+h);
    
    glColor3f(0.0f, 1.0f, 1.0f); // Left
    glVertex3f(x-h, y-h, z-h); glVertex3f(x-h, y-h, z+h);
    glVertex3f(x-h, y+h, z+h); glVertex3f(x-h, y+h, z-h);
    
    glEnd();
}

void render() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    
    float yawRad = camera.yaw * M_PI / 180.0f;
    float pitchRad = camera.pitch * M_PI / 180.0f;
    float frontX = cosf(yawRad) * cosf(pitchRad);
    float frontY = sinf(pitchRad);
    float frontZ = sinf(yawRad) * cosf(pitchRad);
    
    gluLookAt(camera.pos[0], camera.pos[1], camera.pos[2],
              camera.pos[0] + frontX, camera.pos[1] + frontY, camera.pos[2] + frontZ,
              0.0f, 1.0f, 0.0f);
    
    drawGrid();
    drawCube(0, 1, 0, 2);
    drawCube(5, 1, 3, 1.5);
    drawCube(-3, 0.5, -5, 1);
}

int main() {
    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return -1;
    }
    
    GLFWwindow* window = glfwCreateWindow(1280, 720, "3D Sandbox", NULL, NULL);
    if (!window) {
        fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return -1;
    }
    
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0); /* disable VSync for uncapped framerate */
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetCursorPosCallback(window, mouseCallback);
    
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, 1280.0/720.0, 0.1, 100.0);
    
    printf("Controls:\n");
    printf("  WASD - Move\n");
    printf("  Space/Shift - Up/Down\n");
    printf("  Mouse - Look around\n");
    printf("  ESC - Exit\n");
    
    while (!glfwWindowShouldClose(window)) {
        float currentFrame = glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;
        
        processInput(window);
        render();
        
        // Update FPS in window title (simple overlay via title bar)
        float fps = deltaTime > 0.0f ? 1.0f / deltaTime : 0.0f;
        char title[128];
        snprintf(title, sizeof(title), "3D Sandbox - FPS: %.1f", fps);
        glfwSetWindowTitle(window, title);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }
    
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

#include "camera.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void camera_init(Camera *c) {
    c->yaw         = -90.0f;   /* face down -Z to start */
    c->pitch       = 0.0f;
    c->sensitivity = 0.1f;
    c->lastX       = 0.0;
    c->lastY       = 0.0;
    c->firstMouse  = 1;
}

void camera_on_mouse(Camera *c, double xpos, double ypos) {
    if (c->firstMouse) {
        c->lastX      = xpos;
        c->lastY      = ypos;
        c->firstMouse = 0;
    }

    float xoffset = (float)(xpos - c->lastX);
    float yoffset = (float)(c->lastY - ypos);   /* screen y grows downward */
    c->lastX = xpos;
    c->lastY = ypos;

    c->yaw   += xoffset * c->sensitivity;
    c->pitch += yoffset * c->sensitivity;

    if (c->pitch >  89.0f) c->pitch =  89.0f;
    if (c->pitch < -89.0f) c->pitch = -89.0f;
}

void camera_front(const Camera *c, float out[3]) {
    float yawRad   = c->yaw   * (float)M_PI / 180.0f;
    float pitchRad = c->pitch * (float)M_PI / 180.0f;
    out[0] = cosf(yawRad) * cosf(pitchRad);
    out[1] = sinf(pitchRad);
    out[2] = sinf(yawRad) * cosf(pitchRad);
}

#ifndef PULSE_CAMERA_H
#define PULSE_CAMERA_H

typedef struct {
    float  yaw;
    float  pitch;
    float  sensitivity;
    double lastX, lastY;
    int    firstMouse;
} Camera;

void camera_init(Camera *c);

void camera_on_mouse(Camera *c, double xpos, double ypos);

void camera_front(const Camera *c, float out[3]);

#endif

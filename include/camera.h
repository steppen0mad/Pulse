#ifndef PULSE_CAMERA_H
#define PULSE_CAMERA_H

/*
 * First-person mouse-look camera. Holds only orientation -- the player's
 * position lives in the predicted PlayerState, since position is owned by the
 * authoritative simulation. (Extracted from the original single-player main.c.)
 */
typedef struct {
    float  yaw;          /* degrees */
    float  pitch;        /* degrees, clamped to +/-89 */
    float  sensitivity;
    double lastX, lastY;
    int    firstMouse;
} Camera;

void camera_init(Camera *c);

/* Update yaw/pitch from a new absolute mouse position. Pitch is clamped to
 * +/-89 degrees so the view can never flip over the poles. */
void camera_on_mouse(Camera *c, double xpos, double ypos);

/* Write the unit forward vector implied by yaw/pitch into out[3]. */
void camera_front(const Camera *c, float out[3]);

#endif /* PULSE_CAMERA_H */

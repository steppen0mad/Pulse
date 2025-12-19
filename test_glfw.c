#include <stdio.h>
#include <GLFW/glfw3.h>
#include <GL/gl.h>
int main(){
    if(!glfwInit()){fprintf(stderr,"glfwInit failed\n"); return 1;}
    GLFWwindow* w = glfwCreateWindow(640,480,"test",NULL,NULL);
    if(!w){fprintf(stderr,"glfwCreateWindow failed\n"); glfwTerminate(); return 1;}
    glfwMakeContextCurrent(w);
    const unsigned char* renderer = glGetString(GL_RENDERER);
    const unsigned char* version = glGetString(GL_VERSION);
    printf("GL_RENDERER: %s\nGL_VERSION: %s\n", renderer?renderer:"(null)", version?version:"(null)");
    glfwSwapBuffers(w);
    glfwPollEvents();
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}

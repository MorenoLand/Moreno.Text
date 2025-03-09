#include "Renderer/GLRenderer.h"
#include <cstdio>

static GLuint s_shaderProgram = 0;
static GLuint s_vao = 0, s_vbo = 0;
static int s_width = 0, s_height = 0;

static const char* vertexSrc = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
out vec2 vUV;
out vec4 vColor;
uniform mat4 uProjection;
void main() {
    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);
    vUV = aUV;
    vColor = aColor;
}
)";

static const char* fragmentSrc = R"(
#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uAtlas;
void main() {
    FragColor = vec4(vColor.rgb, texture(uAtlas, vUV).r * vColor.a);
}
)";

static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, 512, nullptr, log);
        fprintf(stderr, "Shader compile error: %s\n", log);
        return 0;
    }
    return s;
}

static void ortho(float* m, float l, float r, float b, float t) {
    m[0]=2.f/(r-l); m[4]=0;         m[8]=0;          m[12]=-(r+l)/(r-l);
    m[1]=0;         m[5]=2.f/(t-b); m[9]=0;          m[13]=-(t+b)/(t-b);
    m[2]=0;         m[6]=0;         m[10]=-1.f;       m[14]=-1.f;
    m[3]=0;         m[7]=0;         m[11]=0;          m[15]=1.f;
}

bool GLRenderer::init(int width, int height) {
    s_width = width; s_height = height;
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSrc);
    if (!vs || !fs) return false;
    s_shaderProgram = glCreateProgram();
    glAttachShader(s_shaderProgram, vs);
    glAttachShader(s_shaderProgram, fs);
    glLinkProgram(s_shaderProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(s_shaderProgram, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(s_shaderProgram, 512, nullptr, log);
        fprintf(stderr, "Shader link error: %s\n", log);
        return false;
    }
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    // pos(2) + uv(2) + color(4) = 8 floats per vert
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(2*sizeof(float)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8*sizeof(float), (void*)(4*sizeof(float)));
    glBindVertexArray(0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    return true;
}

void GLRenderer::destroy() {
    if (s_vao) { glDeleteVertexArrays(1, &s_vao); s_vao = 0; }
    if (s_vbo) { glDeleteBuffers(1, &s_vbo); s_vbo = 0; }
    if (s_shaderProgram) { glDeleteProgram(s_shaderProgram); s_shaderProgram = 0; }
}

void GLRenderer::resize(int w, int h) { s_width = w; s_height = h; }

void GLRenderer::beginFrame() {
    glViewport(0, 0, s_width, s_height);
    glClearColor(0.12f, 0.12f, 0.14f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(s_shaderProgram);
    float proj[16];
    ortho(proj, 0, (float)s_width, (float)s_height, 0);
    glUniformMatrix4fv(glGetUniformLocation(s_shaderProgram, "uProjection"), 1, GL_FALSE, proj);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(s_shaderProgram, "uAtlas"), 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void GLRenderer::endFrame() {
    glUseProgram(0);
}

GLuint gl_shaderProgram() { return s_shaderProgram; }
GLuint gl_vao() { return s_vao; }
GLuint gl_vbo() { return s_vbo; }

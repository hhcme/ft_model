#version 300 es
precision highp float;

layout(location = 0) in vec2 a_position;

uniform mat4 u_view_proj;

void main() {
    gl_Position = u_view_proj * vec4(a_position, 0.0, 1.0);
}

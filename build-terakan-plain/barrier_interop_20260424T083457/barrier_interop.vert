#version 450
void main() {
    if (gl_VertexIndex == 0) {
        gl_Position = vec4(-1.0, -1.0, 0.0, 1.0);
    } else if (gl_VertexIndex == 1) {
        gl_Position = vec4( 1.0, -1.0, 0.0, 1.0);
    } else if (gl_VertexIndex == 2) {
        gl_Position = vec4(-1.0,  1.0, 0.0, 1.0);
    } else {
        gl_Position = vec4( 1.0,  1.0, 0.0, 1.0);
    }
}

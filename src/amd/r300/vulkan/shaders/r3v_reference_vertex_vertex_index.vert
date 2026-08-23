// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
const vec4 step = vec4(0.0, 0.0625, 0.0, 0.0);
void main() {
   gl_Position = fma(vec4(float(gl_VertexIndex)), step, position);
}

// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
layout(location = 0) flat out vec4 tint;
void main() {
   float a = dot(position, vec4(0.53333333, 0.0, 0.0, 0.55));
   vec4 rgb = fma(position, vec4(2.0, 2.0, 0.0, 0.0), vec4(0.0));
   tint = fma(vec4(a, a, a, a), vec4(0.0, 0.0, 0.0, 1.0), rgb);
   gl_Position = position;
}

// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
layout(location = 0) flat out vec4 tint;
void main() {
   tint = fma(position, vec4(2.0), vec4(0.0));
   gl_Position = position;
}

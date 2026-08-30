// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
layout(location = 1) in vec4 color;
layout(location = 0) out float tint;
void main() {
   tint = color.x;
   gl_Position = position;
}

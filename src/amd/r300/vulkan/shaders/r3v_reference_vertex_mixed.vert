// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
layout(location = 0) flat out vec4 tint;
layout(location = 1) out vec4 tone;
void main() {
   tint = position;
   tone = position;
   gl_Position = position;
}

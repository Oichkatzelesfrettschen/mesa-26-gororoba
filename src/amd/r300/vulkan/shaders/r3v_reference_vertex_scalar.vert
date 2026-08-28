// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
layout(location = 0) out float lum;
void main() {
   lum = position.x;
   gl_Position = position;
}

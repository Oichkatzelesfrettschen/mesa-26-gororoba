// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in float lum;
layout(location = 0) out vec4 color;
void main() {
   color = vec4(lum);
}

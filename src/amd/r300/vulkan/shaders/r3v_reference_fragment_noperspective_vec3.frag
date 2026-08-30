// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) noperspective in vec3 tint;
layout(location = 0) out vec4 color;
void main() {
   color = vec4(tint, 1.0);
}

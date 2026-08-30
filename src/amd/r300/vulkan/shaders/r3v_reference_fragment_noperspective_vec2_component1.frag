// SPDX-License-Identifier: MIT
#version 450
layout(location = 0, component = 1) noperspective in vec2 tint;
layout(location = 0) out vec4 color;
void main() {
   color = vec4(tint, 0.0, 1.0);
}

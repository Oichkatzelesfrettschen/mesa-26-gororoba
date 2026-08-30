// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) flat in vec4 tint;
layout(location = 1) noperspective in vec4 tone;
layout(location = 0) out vec4 color;
void main() {
   color = vec4(tint.xy, tone.xy);
}

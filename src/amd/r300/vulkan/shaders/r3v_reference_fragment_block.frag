// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in Varyings {
   flat vec4 tint;
   vec4 tone;
} v;
layout(location = 0) out vec4 color;
void main() {
   color = v.tint + v.tone;
}

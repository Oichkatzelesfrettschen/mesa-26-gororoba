// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
layout(location = 0) out Varyings {
   flat vec4 tint;
   vec4 tone;
} v;
void main() {
   v.tint = position;
   v.tone = position;
   gl_Position = position;
}

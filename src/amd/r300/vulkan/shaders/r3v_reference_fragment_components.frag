// SPDX-License-Identifier: MIT
#version 450
layout(location = 0, component = 0) in vec2 uv;
layout(location = 0, component = 2) in vec2 rest;
layout(location = 0) out vec4 color;
void main() {
   color = vec4(uv, rest);
}

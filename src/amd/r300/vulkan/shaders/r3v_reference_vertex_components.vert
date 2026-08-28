// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
layout(location = 0, component = 0) out vec2 uv;
layout(location = 0, component = 2) out vec2 rest;
void main() {
   uv = position.xy;
   rest = position.zw;
   gl_Position = position;
}

// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
layout(location = 0, component = 1) out vec2 tint;
void main() {
   tint = fma(position, vec4(0.5, 0.5, 0.0, 0.0),
              vec4(0.5, 0.5, 0.25, 1.0)).xy;
   gl_Position = position;
}

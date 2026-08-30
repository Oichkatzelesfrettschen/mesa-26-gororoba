// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
layout(location = 0) out vec4 tint;
layout(location = 1) out vec4 tone;
void main() {
   vec4 shade = fma(position, vec4(0.5, 0.5, 0.0, 0.0),
                    vec4(0.5, 0.5, 0.25, 1.0));
   tint = shade;
   tone = shade;
   gl_Position = position;
}

// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
void main() {
   vec4 t = fma(position, vec4(2.0, -0.5, 4.0, 1.0),
                vec4(2.0, -0.5, 4.0, 1.0));
   float d = dot(t, vec4(2.0, -0.5, 4.0, 1.0));
   gl_Position = vec4(d, d, d, d);
}

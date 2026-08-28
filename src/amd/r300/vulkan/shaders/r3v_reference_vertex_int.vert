// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
layout(location = 0) flat out ivec4 tag;
void main() {
   tag = ivec4(position);
   gl_Position = position;
}

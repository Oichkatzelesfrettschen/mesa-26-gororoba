// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) flat in ivec4 tag;
layout(location = 0) out vec4 color;
void main() {
   color = vec4(tag);
}

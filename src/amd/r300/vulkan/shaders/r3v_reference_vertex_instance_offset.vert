// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
layout(location = 1) in vec4 instance_offset;
void main() {
   gl_Position = position + instance_offset;
}

// SPDX-License-Identifier: MIT
#version 450
layout(location = 0) in vec4 position;
out float gl_ClipDistance[3];
out float gl_CullDistance[2];
void main() {
   gl_ClipDistance[0] = position.x;
   gl_ClipDistance[1] = position.y;
   gl_ClipDistance[2] = position.z;
   gl_CullDistance[0] = position.w;
   gl_CullDistance[1] = position.x;
   gl_Position = position;
}

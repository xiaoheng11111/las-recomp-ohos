#version 450
// 固定色输出。颜色选绿，和「HARDWARE」判定同色系，便于肉眼看链路是否通。
layout(location = 0) out vec4 o;

void main() {
    o = vec4(0.247, 0.725, 0.314, 1.0);
}

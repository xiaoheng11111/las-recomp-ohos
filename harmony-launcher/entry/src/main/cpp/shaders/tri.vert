#version 450
// 链路验证用：固定三顶点，居中正三角。不用顶点缓冲，避免把顶点取数混进来。
vec2 kPos[3] = vec2[3](
    vec2( 0.00,  0.72),
    vec2(-0.72, -0.62),
    vec2( 0.72, -0.62)
);

void main() {
    gl_Position = vec4(kPos[gl_VertexIndex], 0.0, 1.0);
}

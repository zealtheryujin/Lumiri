#version 460
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(binding=0) uniform sampler2D planeY;
layout(binding=1) uniform sampler2D planeU;
layout(binding=2) uniform sampler2D planeV;
layout(std140,binding=0) uniform Params {
    vec4 rowR;
    vec4 rowG;
    vec4 rowB;
    vec4 scaleY;
    vec4 scaleU;
    vec4 scaleV;
};
void main() {

    vec2 yuv = clamp(uv * scaleY.xy, scaleY.zw, scaleY.xy - scaleY.zw);
    vec2 cuv = clamp(uv * scaleU.xy, scaleU.zw, scaleU.xy - scaleU.zw);
    float y = texture(planeY, yuv).r;
    vec2 c = texture(planeU, cuv).rg;
    if (scaleV.x > 0.0) {
        vec2 vuv = clamp(uv * scaleV.xy, scaleV.zw, scaleV.xy - scaleV.zw);
        c.y = texture(planeV, vuv).r;
    }
    vec4 sampleValue = vec4(y, c, 1.0);
    color = vec4(dot(rowR, sampleValue), dot(rowG, sampleValue), dot(rowB, sampleValue), 1.0);
}

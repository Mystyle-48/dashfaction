#define TEXTURE_SOURCE_NONE                   0
#define TEXTURE_SOURCE_WRAP                   1
#define TEXTURE_SOURCE_CLAMP                  2
#define TEXTURE_SOURCE_CLAMP_NO_FILTERING     3
#define TEXTURE_SOURCE_CLAMP_1_WRAP_0         4
#define TEXTURE_SOURCE_CLAMP_1_WRAP_0_MOD2X   5
#define TEXTURE_SOURCE_CLAMP_1_CLAMP_0        6
#define TEXTURE_SOURCE_BUMPENV_MAP            7
#define TEXTURE_SOURCE_MT_U_WRAP_V_CLAMP      8
#define TEXTURE_SOURCE_MT_U_CLAMP_V_WRAP      9
#define TEXTURE_SOURCE_MT_WRAP_TRILIN        10
#define TEXTURE_SOURCE_MT_CLAMP_TRILIN       11

#define COLOR_SOURCE_VERTEX                   0
#define COLOR_SOURCE_TEXTURE                  1
#define COLOR_SOURCE_VERTEX_TIMES_TEXTURE     2
#define COLOR_SOURCE_VERTEX_PLUS_TEXTURE      3
#define COLOR_SOURCE_VERTEX_TIMES_TEXTURE_2X  4

#define ALPHA_SOURCE_VERTEX                0
#define ALPHA_SOURCE_VERTEX_NONDARKENING   1
#define ALPHA_SOURCE_TEXTURE               2
#define ALPHA_SOURCE_VERTEX_TIMES_TEXTURE  3

struct VsOutput
{
    float4 pos : SV_POSITION;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 color : COLOR;
};

cbuffer PsConstantBuffer : register(b0)
{
    float ts;
    float cs;
    float as;
    float alpha_test;
};

Texture2D tex0;
Texture2D tex1;
SamplerState samp0;
SamplerState samp1;

float4 main(VsOutput input) : SV_TARGET
{
    float4 tex0_color;
    if (ts == TEXTURE_SOURCE_NONE) {
        tex0_color = float4(1, 1, 1, 1);
    } else {
        tex0_color = tex0.Sample(samp0, input.uv0);
    }

    float4 target;
    if (cs == COLOR_SOURCE_VERTEX && ts != TEXTURE_SOURCE_CLAMP) { // hack
        target.rgb = input.color.rgb;
    }
    else if (cs == COLOR_SOURCE_TEXTURE && ts == TEXTURE_SOURCE_CLAMP) {
        target.rgb = tex0_color.rgb;
    }
    else if (cs == COLOR_SOURCE_VERTEX_TIMES_TEXTURE) {
        target.rgb = input.color.rgb * tex0_color.rgb;
    }
    else if (cs == COLOR_SOURCE_VERTEX_PLUS_TEXTURE) {
        target.rgb = input.color.rgb + tex0_color.rgb;
    }
    else if (cs == COLOR_SOURCE_VERTEX_TIMES_TEXTURE_2X) {
        target.rgb = input.color.rgb * tex0_color.rgb;
    }
    if (as == ALPHA_SOURCE_VERTEX) {
        target.a = input.color.a;
    }
    else if (as == ALPHA_SOURCE_VERTEX_NONDARKENING) {
        target.a = input.color.a;
    }
    else if (as == ALPHA_SOURCE_TEXTURE) {
        target.a = tex0_color.a;
    }
    else if (as == ALPHA_SOURCE_VERTEX_TIMES_TEXTURE) {
        target.a = input.color.a * tex0_color.a;
    }
    if (ts >= 4) {
        target.rgb *= tex1.Sample(samp1, input.uv1).rgb;
    }
    if (ts == TEXTURE_SOURCE_CLAMP_1_WRAP_0_MOD2X) {
        target.rgb *= 2;
    }
    if (alpha_test == 1 && target.a < 0.1)
        discard;
    return target;
}
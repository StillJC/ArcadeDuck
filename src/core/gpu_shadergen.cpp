// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#include "gpu_shadergen.h"

GPUShaderGen::GPUShaderGen(RenderAPI render_api, bool supports_dual_source_blend, bool supports_framebuffer_fetch)
  : ShaderGen(render_api, GetShaderLanguageForAPI(render_api), supports_dual_source_blend, supports_framebuffer_fetch)
{
}

GPUShaderGen::~GPUShaderGen() = default;

void GPUShaderGen::WriteDisplayUniformBuffer(std::stringstream& ss)
{
  // Rotation matrix split into rows to avoid padding in HLSL.
  DeclareUniformBuffer(ss,
                       {"float4 u_src_rect", "float4 u_src_size", "float4 u_clamp_rect", "float4 u_params",
                        "float2 u_rotation_matrix0", "float2 u_rotation_matrix1"},
                       true);

  ss << R"(
float2 ClampUV(float2 uv) {
  return clamp(uv, u_clamp_rect.xy, u_clamp_rect.zw);
})";
}

std::string GPUShaderGen::GenerateDisplayVertexShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareVertexEntryPoint(ss, {}, 0, 1, {}, true);
  ss << R"(
{
  float2 pos = float2(float((v_id << 1) & 2u), float(v_id & 2u));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  v_pos = float4(pos * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);

  // Avoid HLSL/GLSL constructor differences by explicitly multiplying the matrix.
  v_pos.xy = float2(dot(u_rotation_matrix0, v_pos.xy), dot(u_rotation_matrix1, v_pos.xy));

  #if API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
}
)";

  return ss.str();
}

std::string GPUShaderGen::GenerateDisplayOffscreenVertexShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareVertexEntryPoint(ss, {}, 0, 1, {}, true);
  ss << R"(
{
  float2 pos = float2(float((v_id << 1) & 2u), float(v_id & 2u));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  v_pos = float4(pos * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);

  // Avoid HLSL/GLSL constructor differences by explicitly multiplying the matrix.
  v_pos.xy = float2(dot(u_rotation_matrix0, v_pos.xy), dot(u_rotation_matrix1, v_pos.xy));

  #if API_OPENGL || API_OPENGL_ES || API_VULKAN
    v_pos.y = -v_pos.y;
  #endif
}
)";

  return ss.str();
}
std::string GPUShaderGen::GenerateDisplayFragmentShader(bool clamp_uv)
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0);
  DeclareFragmentEntryPoint(ss, 0, 1);
  if (clamp_uv)
    ss << "{\n  o_col0 = float4(SAMPLE_TEXTURE(samp0, ClampUV(v_tex0)).rgb, 1.0f);\n }";
  else
    ss << "{\n  o_col0 = float4(SAMPLE_TEXTURE(samp0, v_tex0).rgb, 1.0f);\n }";

  return ss.str();
}

std::string GPUShaderGen::GenerateDisplaySharpBilinearFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);

  // Based on
  // https://github.com/rsn8887/Sharp-Bilinear-Shaders/blob/master/Copy_To_RetroPie/shaders/sharp-bilinear-simple.glsl
  DeclareFragmentEntryPoint(ss, 0, 1);
  ss << R"(
{
  float2 scale = u_params.xy;
  float2 region_range = u_params.zw;

  float2 texel = v_tex0 * u_src_size.xy;
  float2 texel_floored = floor(texel);
  float2 s = frac(texel);

  float2 center_dist = s - 0.5;
  float2 f = (center_dist - clamp(center_dist, -region_range, region_range)) * scale + 0.5;
  float2 mod_texel = texel_floored + f;

  o_col0 = float4(SAMPLE_TEXTURE(samp0, ClampUV(mod_texel * u_src_size.zw)).rgb, 1.0f);
})";

  return ss.str();
}

// Snapdragon Game Super Resolution 1 (SGSR1)
// Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//
// Adapted from Qualcomm's SGSR1 edge-direction GLSL reference.
// textureGather is expanded to explicit clamped texel loads so the scaler
// stays on ArcadeDuck's existing cross-backend fragment-shader path and
// cannot sample outside an active VRAM display sub-rectangle.
std::string GPUShaderGen::GenerateDisplaySGSR1FragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);

  ss << R"(
CONSTANT float SGSR_EDGE_THRESHOLD = 8.0f / 255.0f;
CONSTANT float SGSR_EDGE_SHARPNESS = 2.0f;

float SGSRFastLanczos2(float x)
{
  float wa = x - 4.0f;
  float wb = x * wa - wa;
  wa *= wa;
  return wb * wa;
}

float2 SGSREdgeDirection(float4 left, float4 right)
{
  float rx_lz = right.x - left.z;
  float rw_ly = right.w - left.y;
  float2 delta = float2(rx_lz + rw_ly, rx_lz - rw_ly);
  float length_inv = 1.0f / sqrt(dot(delta, delta) + 3.075740e-05f);
  return delta * length_inv;
}

float2 SGSRWeight(float dx, float dy, float c, float3 data)
{
  float stddev = data.x;
  float2 dir = data.yz;
  float edge_dist = dx * dir.y + dy * dir.x;
  float x = (dx * dx + dy * dy) +
            (edge_dist * edge_dist) * (clamp((c * c) * stddev, 0.0f, 1.0f) * 0.7f - 1.0f);
  float w = SGSRFastLanczos2(x);
  return float2(w, w * c);
}

float SGSRLoadGreen(int2 coord, int2 source_min, int2 source_max)
{
  return LOAD_TEXTURE(samp0, clamp(coord, source_min, source_max), 0).g;
}
)";

  DeclareFragmentEntryPoint(ss, 0, 1);
  ss << R"(
{
  int2 source_min = int2(u_params.xy);
  int2 source_max = source_min + int2(u_params.zw) - int2(1, 1);

  float3 pix = SAMPLE_TEXTURE_LEVEL(samp0, ClampUV(v_tex0), 0.0f).rgb;

  float2 img_coord = v_tex0 * u_src_size.xy + float2(-0.5f, 0.5f);
  int2 base_coord = int2(floor(img_coord));
  float2 pl = frac(img_coord);

  float4 left = float4(
    SGSRLoadGreen(base_coord + int2(-1, 1), source_min, source_max),
    SGSRLoadGreen(base_coord + int2( 0, 1), source_min, source_max),
    SGSRLoadGreen(base_coord + int2( 0, 0), source_min, source_max),
    SGSRLoadGreen(base_coord + int2(-1, 0), source_min, source_max));

  float edge_vote = abs(left.z - left.y) + abs(pix.g - left.y) + abs(pix.g - left.z);
  if (edge_vote > SGSR_EDGE_THRESHOLD)
  {
    float4 right = float4(
      SGSRLoadGreen(base_coord + int2(1, 1), source_min, source_max),
      SGSRLoadGreen(base_coord + int2(2, 1), source_min, source_max),
      SGSRLoadGreen(base_coord + int2(2, 0), source_min, source_max),
      SGSRLoadGreen(base_coord + int2(1, 0), source_min, source_max));

    float4 up_down = float4(
      SGSRLoadGreen(base_coord + int2(0, -1), source_min, source_max),
      SGSRLoadGreen(base_coord + int2(1, -1), source_min, source_max),
      SGSRLoadGreen(base_coord + int2(1,  2), source_min, source_max),
      SGSRLoadGreen(base_coord + int2(0,  2), source_min, source_max));

    float mean = (left.y + left.z + right.x + right.w) * 0.25f;
    left -= float4(mean, mean, mean, mean);
    right -= float4(mean, mean, mean, mean);
    up_down -= float4(mean, mean, mean, mean);
    float pix_g = pix.g - mean;

    float sum =
      abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w) +
      abs(right.x) + abs(right.y) + abs(right.z) + abs(right.w) +
      abs(up_down.x) + abs(up_down.y) + abs(up_down.z) + abs(up_down.w);

    float sum_mean = 1.014185e+01f / sum;
    float stddev = sum_mean * sum_mean;
    float3 data = float3(stddev, SGSREdgeDirection(left, right));

    float2 awy = SGSRWeight(pl.x,        pl.y + 1.0f, up_down.x, data);
    awy += SGSRWeight(pl.x - 1.0f, pl.y + 1.0f, up_down.y, data);
    awy += SGSRWeight(pl.x - 1.0f, pl.y - 2.0f, up_down.z, data);
    awy += SGSRWeight(pl.x,        pl.y - 2.0f, up_down.w, data);
    awy += SGSRWeight(pl.x + 1.0f, pl.y - 1.0f, left.x, data);
    awy += SGSRWeight(pl.x,        pl.y - 1.0f, left.y, data);
    awy += SGSRWeight(pl.x,        pl.y,        left.z, data);
    awy += SGSRWeight(pl.x + 1.0f, pl.y,        left.w, data);
    awy += SGSRWeight(pl.x - 1.0f, pl.y - 1.0f, right.x, data);
    awy += SGSRWeight(pl.x - 2.0f, pl.y - 1.0f, right.y, data);
    awy += SGSRWeight(pl.x - 2.0f, pl.y,        right.z, data);
    awy += SGSRWeight(pl.x - 1.0f, pl.y,        right.w, data);

    float final_y = awy.y / awy.x;
    float max_y = max(max(left.y, left.z), max(right.x, right.w));
    float min_y = min(min(left.y, left.z), min(right.x, right.w));
    float delta_y = clamp(SGSR_EDGE_SHARPNESS * final_y, min_y, max_y) - pix_g;

    delta_y = clamp(delta_y, -23.0f / 255.0f, 23.0f / 255.0f);
    pix = saturate(pix + delta_y);
  }

  o_col0 = float4(pix, 1.0f);
}
)";

  return ss.str();
}
// NVIDIA Image Scaling SDK 1.0.3
// Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// ArcadeDuck adaptation:
// NVIDIA's reference NVScaler is a compute shader which caches the 6x6 source
// support, edge maps, and filter coefficients in thread-group shared memory.
// This version preserves the NVScaler per-output-pixel filtering math and
// coefficient banks, but evaluates them directly in the existing portable
// display fragment-shader stage. That avoids introducing backend-specific
// compute/UAV plumbing just for the scaler. The tradeoff is extra source loads.
std::string GPUShaderGen::GenerateDisplayNISFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);

  ss << R"(
CONSTANT float NIS_DETECT_RATIO = 2.0f * 1127.0f / 1024.0f;
CONSTANT float NIS_DETECT_THRESHOLD = 64.0f / 1024.0f;
CONSTANT float NIS_EPSILON = 1.0f / 255.0f;
CONSTANT float NIS_MIN_CONTRAST_RATIO = 2.0f;
CONSTANT float NIS_RATIO_NORM = 1.0f / 8.0f;
CONSTANT float NIS_SHARP_START_Y = 0.45f;
CONSTANT float NIS_SHARP_SCALE_Y = 1.0f / 0.45f;

// NIS sharpness slider = 0.5 (reference default).
CONSTANT float NIS_SHARP_STRENGTH_MIN = 0.4f;
CONSTANT float NIS_SHARP_STRENGTH_SCALE = 1.2f;
CONSTANT float NIS_SHARP_LIMIT_MIN = 0.14f;
CONSTANT float NIS_SHARP_LIMIT_SCALE = 0.36f;

CONSTANT float NIS_SCALE_COEF[384] = BEGIN_ARRAY(float, 384)
  0.0f, 0.0f, 1.0000f, 0.0f, 0.0f, 0.0f,
  0.0029f, -0.0127f, 1.0000f, 0.0132f, -0.0034f, 0.0f,
  0.0063f, -0.0249f, 0.9985f, 0.0269f, -0.0068f, 0.0f,
  0.0088f, -0.0361f, 0.9956f, 0.0415f, -0.0103f, 0.0005f,
  0.0117f, -0.0474f, 0.9932f, 0.0562f, -0.0142f, 0.0005f,
  0.0142f, -0.0576f, 0.9897f, 0.0713f, -0.0181f, 0.0005f,
  0.0166f, -0.0674f, 0.9844f, 0.0874f, -0.0220f, 0.0010f,
  0.0186f, -0.0762f, 0.9785f, 0.1040f, -0.0264f, 0.0015f,
  0.0205f, -0.0850f, 0.9727f, 0.1206f, -0.0308f, 0.0020f,
  0.0225f, -0.0928f, 0.9648f, 0.1382f, -0.0352f, 0.0024f,
  0.0239f, -0.1006f, 0.9575f, 0.1558f, -0.0396f, 0.0029f,
  0.0254f, -0.1074f, 0.9487f, 0.1738f, -0.0439f, 0.0034f,
  0.0264f, -0.1138f, 0.9390f, 0.1929f, -0.0488f, 0.0044f,
  0.0278f, -0.1191f, 0.9282f, 0.2119f, -0.0537f, 0.0049f,
  0.0288f, -0.1245f, 0.9170f, 0.2310f, -0.0581f, 0.0059f,
  0.0293f, -0.1294f, 0.9058f, 0.2510f, -0.0630f, 0.0063f,
  0.0303f, -0.1333f, 0.8926f, 0.2710f, -0.0679f, 0.0073f,
  0.0308f, -0.1367f, 0.8789f, 0.2915f, -0.0728f, 0.0083f,
  0.0308f, -0.1401f, 0.8657f, 0.3120f, -0.0776f, 0.0093f,
  0.0313f, -0.1426f, 0.8506f, 0.3330f, -0.0825f, 0.0103f,
  0.0313f, -0.1445f, 0.8354f, 0.3540f, -0.0874f, 0.0112f,
  0.0313f, -0.1460f, 0.8193f, 0.3755f, -0.0923f, 0.0122f,
  0.0313f, -0.1470f, 0.8022f, 0.3965f, -0.0967f, 0.0137f,
  0.0308f, -0.1479f, 0.7856f, 0.4185f, -0.1016f, 0.0146f,
  0.0303f, -0.1479f, 0.7681f, 0.4399f, -0.1060f, 0.0156f,
  0.0298f, -0.1479f, 0.7505f, 0.4614f, -0.1104f, 0.0166f,
  0.0293f, -0.1470f, 0.7314f, 0.4829f, -0.1147f, 0.0181f,
  0.0288f, -0.1460f, 0.7119f, 0.5049f, -0.1187f, 0.0190f,
  0.0278f, -0.1445f, 0.6929f, 0.5264f, -0.1226f, 0.0200f,
  0.0273f, -0.1431f, 0.6724f, 0.5479f, -0.1260f, 0.0215f,
  0.0264f, -0.1411f, 0.6528f, 0.5693f, -0.1299f, 0.0225f,
  0.0254f, -0.1387f, 0.6323f, 0.5903f, -0.1328f, 0.0234f,
  0.0244f, -0.1357f, 0.6113f, 0.6113f, -0.1357f, 0.0244f,
  0.0234f, -0.1328f, 0.5903f, 0.6323f, -0.1387f, 0.0254f,
  0.0225f, -0.1299f, 0.5693f, 0.6528f, -0.1411f, 0.0264f,
  0.0215f, -0.1260f, 0.5479f, 0.6724f, -0.1431f, 0.0273f,
  0.0200f, -0.1226f, 0.5264f, 0.6929f, -0.1445f, 0.0278f,
  0.0190f, -0.1187f, 0.5049f, 0.7119f, -0.1460f, 0.0288f,
  0.0181f, -0.1147f, 0.4829f, 0.7314f, -0.1470f, 0.0293f,
  0.0166f, -0.1104f, 0.4614f, 0.7505f, -0.1479f, 0.0298f,
  0.0156f, -0.1060f, 0.4399f, 0.7681f, -0.1479f, 0.0303f,
  0.0146f, -0.1016f, 0.4185f, 0.7856f, -0.1479f, 0.0308f,
  0.0137f, -0.0967f, 0.3965f, 0.8022f, -0.1470f, 0.0313f,
  0.0122f, -0.0923f, 0.3755f, 0.8193f, -0.1460f, 0.0313f,
  0.0112f, -0.0874f, 0.3540f, 0.8354f, -0.1445f, 0.0313f,
  0.0103f, -0.0825f, 0.3330f, 0.8506f, -0.1426f, 0.0313f,
  0.0093f, -0.0776f, 0.3120f, 0.8657f, -0.1401f, 0.0308f,
  0.0083f, -0.0728f, 0.2915f, 0.8789f, -0.1367f, 0.0308f,
  0.0073f, -0.0679f, 0.2710f, 0.8926f, -0.1333f, 0.0303f,
  0.0063f, -0.0630f, 0.2510f, 0.9058f, -0.1294f, 0.0293f,
  0.0059f, -0.0581f, 0.2310f, 0.9170f, -0.1245f, 0.0288f,
  0.0049f, -0.0537f, 0.2119f, 0.9282f, -0.1191f, 0.0278f,
  0.0044f, -0.0488f, 0.1929f, 0.9390f, -0.1138f, 0.0264f,
  0.0034f, -0.0439f, 0.1738f, 0.9487f, -0.1074f, 0.0254f,
  0.0029f, -0.0396f, 0.1558f, 0.9575f, -0.1006f, 0.0239f,
  0.0024f, -0.0352f, 0.1382f, 0.9648f, -0.0928f, 0.0225f,
  0.0020f, -0.0308f, 0.1206f, 0.9727f, -0.0850f, 0.0205f,
  0.0015f, -0.0264f, 0.1040f, 0.9785f, -0.0762f, 0.0186f,
  0.0010f, -0.0220f, 0.0874f, 0.9844f, -0.0674f, 0.0166f,
  0.0005f, -0.0181f, 0.0713f, 0.9897f, -0.0576f, 0.0142f,
  0.0005f, -0.0142f, 0.0562f, 0.9932f, -0.0474f, 0.0117f,
  0.0005f, -0.0103f, 0.0415f, 0.9956f, -0.0361f, 0.0088f,
  0.0f, -0.0068f, 0.0269f, 0.9985f, -0.0249f, 0.0063f,
  0.0f, -0.0034f, 0.0132f, 1.0000f, -0.0127f, 0.0029f
END_ARRAY;

CONSTANT float NIS_USM_COEF[384] = BEGIN_ARRAY(float, 384)
  0.0f, -0.6001f, 1.2002f, -0.6001f, 0.0f, 0.0f,
  0.0029f, -0.6084f, 1.1987f, -0.5903f, -0.0029f, 0.0f,
  0.0049f, -0.6147f, 1.1958f, -0.5791f, -0.0068f, 0.0005f,
  0.0073f, -0.6196f, 1.1890f, -0.5659f, -0.0103f, 0.0f,
  0.0093f, -0.6235f, 1.1802f, -0.5513f, -0.0151f, 0.0f,
  0.0112f, -0.6265f, 1.1699f, -0.5352f, -0.0195f, 0.0005f,
  0.0122f, -0.6270f, 1.1582f, -0.5181f, -0.0259f, 0.0005f,
  0.0142f, -0.6284f, 1.1455f, -0.5005f, -0.0317f, 0.0005f,
  0.0156f, -0.6265f, 1.1274f, -0.4790f, -0.0386f, 0.0005f,
  0.0166f, -0.6235f, 1.1089f, -0.4570f, -0.0454f, 0.0010f,
  0.0176f, -0.6187f, 1.0879f, -0.4346f, -0.0532f, 0.0010f,
  0.0181f, -0.6138f, 1.0659f, -0.4102f, -0.0615f, 0.0015f,
  0.0190f, -0.6069f, 1.0405f, -0.3843f, -0.0698f, 0.0015f,
  0.0195f, -0.6006f, 1.0161f, -0.3574f, -0.0796f, 0.0020f,
  0.0200f, -0.5928f, 0.9893f, -0.3286f, -0.0898f, 0.0024f,
  0.0200f, -0.5820f, 0.9580f, -0.2988f, -0.1001f, 0.0029f,
  0.0200f, -0.5728f, 0.9292f, -0.2690f, -0.1104f, 0.0034f,
  0.0200f, -0.5620f, 0.8975f, -0.2368f, -0.1226f, 0.0039f,
  0.0205f, -0.5498f, 0.8643f, -0.2046f, -0.1343f, 0.0044f,
  0.0200f, -0.5371f, 0.8301f, -0.1709f, -0.1465f, 0.0049f,
  0.0195f, -0.5239f, 0.7944f, -0.1367f, -0.1587f, 0.0054f,
  0.0195f, -0.5107f, 0.7598f, -0.1021f, -0.1724f, 0.0059f,
  0.0190f, -0.4966f, 0.7231f, -0.0649f, -0.1865f, 0.0063f,
  0.0186f, -0.4819f, 0.6846f, -0.0288f, -0.1997f, 0.0068f,
  0.0186f, -0.4668f, 0.6460f, 0.0093f, -0.2144f, 0.0073f,
  0.0176f, -0.4507f, 0.6055f, 0.0479f, -0.2290f, 0.0083f,
  0.0171f, -0.4370f, 0.5693f, 0.0859f, -0.2446f, 0.0088f,
  0.0161f, -0.4199f, 0.5283f, 0.1255f, -0.2598f, 0.0098f,
  0.0161f, -0.4048f, 0.4883f, 0.1655f, -0.2754f, 0.0103f,
  0.0151f, -0.3887f, 0.4497f, 0.2041f, -0.2910f, 0.0107f,
  0.0142f, -0.3711f, 0.4072f, 0.2446f, -0.3066f, 0.0117f,
  0.0137f, -0.3555f, 0.3672f, 0.2852f, -0.3228f, 0.0122f,
  0.0132f, -0.3394f, 0.3262f, 0.3262f, -0.3394f, 0.0132f,
  0.0122f, -0.3228f, 0.2852f, 0.3672f, -0.3555f, 0.0137f,
  0.0117f, -0.3066f, 0.2446f, 0.4072f, -0.3711f, 0.0142f,
  0.0107f, -0.2910f, 0.2041f, 0.4497f, -0.3887f, 0.0151f,
  0.0103f, -0.2754f, 0.1655f, 0.4883f, -0.4048f, 0.0161f,
  0.0098f, -0.2598f, 0.1255f, 0.5283f, -0.4199f, 0.0161f,
  0.0088f, -0.2446f, 0.0859f, 0.5693f, -0.4370f, 0.0171f,
  0.0083f, -0.2290f, 0.0479f, 0.6055f, -0.4507f, 0.0176f,
  0.0073f, -0.2144f, 0.0093f, 0.6460f, -0.4668f, 0.0186f,
  0.0068f, -0.1997f, -0.0288f, 0.6846f, -0.4819f, 0.0186f,
  0.0063f, -0.1865f, -0.0649f, 0.7231f, -0.4966f, 0.0190f,
  0.0059f, -0.1724f, -0.1021f, 0.7598f, -0.5107f, 0.0195f,
  0.0054f, -0.1587f, -0.1367f, 0.7944f, -0.5239f, 0.0195f,
  0.0049f, -0.1465f, -0.1709f, 0.8301f, -0.5371f, 0.0200f,
  0.0044f, -0.1343f, -0.2046f, 0.8643f, -0.5498f, 0.0205f,
  0.0039f, -0.1226f, -0.2368f, 0.8975f, -0.5620f, 0.0200f,
  0.0034f, -0.1104f, -0.2690f, 0.9292f, -0.5728f, 0.0200f,
  0.0029f, -0.1001f, -0.2988f, 0.9580f, -0.5820f, 0.0200f,
  0.0024f, -0.0898f, -0.3286f, 0.9893f, -0.5928f, 0.0200f,
  0.0020f, -0.0796f, -0.3574f, 1.0161f, -0.6006f, 0.0195f,
  0.0015f, -0.0698f, -0.3843f, 1.0405f, -0.6069f, 0.0190f,
  0.0015f, -0.0615f, -0.4102f, 1.0659f, -0.6138f, 0.0181f,
  0.0010f, -0.0532f, -0.4346f, 1.0879f, -0.6187f, 0.0176f,
  0.0010f, -0.0454f, -0.4570f, 1.1089f, -0.6235f, 0.0166f,
  0.0005f, -0.0386f, -0.4790f, 1.1274f, -0.6265f, 0.0156f,
  0.0005f, -0.0317f, -0.5005f, 1.1455f, -0.6284f, 0.0142f,
  0.0005f, -0.0259f, -0.5181f, 1.1582f, -0.6270f, 0.0122f,
  0.0005f, -0.0195f, -0.5352f, 1.1699f, -0.6265f, 0.0112f,
  0.0f, -0.0151f, -0.5513f, 1.1802f, -0.6235f, 0.0093f,
  0.0f, -0.0103f, -0.5659f, 1.1890f, -0.6196f, 0.0073f,
  0.0005f, -0.0068f, -0.5791f, 1.1958f, -0.6147f, 0.0049f,
  0.0f, -0.0029f, -0.5903f, 1.1987f, -0.6084f, 0.0029f
END_ARRAY;

float NISGetY(float3 rgb)
{
  return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

float NISLoadY(int2 coord, int2 source_min, int2 source_max)
{
  return NISGetY(LOAD_TEXTURE(samp0, clamp(coord, source_min, source_max), 0).rgb);
}

float NISScaleCoef(int phase, int tap)
{
  return NIS_SCALE_COEF[phase * 6 + tap];
}

float NISUSMCoef(int phase, int tap)
{
  return NIS_USM_COEF[phase * 6 + tap];
}

float4 NISGetEdgeMapAt(in float p[36], int oy, int ox)
{
  int r0 = oy * 6 + ox;
  int r1 = r0 + 6;
  int r2 = r1 + 6;

  float g0 = abs(p[r0] + p[r0 + 1] + p[r0 + 2] -
                 p[r2] - p[r2 + 1] - p[r2 + 2]);
  float g45 = abs(p[r1] + p[r0] + p[r0 + 1] -
                  p[r1 + 2] - p[r2 + 2] - p[r2 + 1]);
  float g90 = abs(p[r0] + p[r1] + p[r2] -
                  p[r0 + 2] - p[r1 + 2] - p[r2 + 2]);
  float g135 = abs(p[r1] + p[r2] + p[r2 + 1] -
                   p[r0 + 1] - p[r0 + 2] - p[r1 + 2]);

  float g0_90_max = max(g0, g90);
  float g0_90_min = min(g0, g90);
  float g45_135_max = max(g45, g135);
  float g45_135_min = min(g45, g135);

  if ((g0_90_max + g45_135_max) == 0.0f)
    return float4(0.0f, 0.0f, 0.0f, 0.0f);

  float e0_90 = min(g0_90_max / (g0_90_max + g45_135_max), 1.0f);
  float e45_135 = 1.0f - e0_90;

  bool c0_90 = (g0_90_max > (g0_90_min * NIS_DETECT_RATIO)) &&
               (g0_90_max > NIS_DETECT_THRESHOLD) &&
               (g0_90_max > g45_135_min);
  bool c45_135 = (g45_135_max > (g45_135_min * NIS_DETECT_RATIO)) &&
                 (g45_135_max > NIS_DETECT_THRESHOLD) &&
                 (g45_135_max > g0_90_min);
  bool cg0_90 = (g0_90_max == g0);
  bool cg45_135 = (g45_135_max == g45);

  float fe0_90 = (c0_90 && c45_135) ? e0_90 : 1.0f;
  float fe45_135 = (c0_90 && c45_135) ? e45_135 : 1.0f;

  return float4(
    (c0_90 && cg0_90) ? fe0_90 : 0.0f,
    (c0_90 && !cg0_90) ? fe0_90 : 0.0f,
    (c45_135 && cg45_135) ? fe45_135 : 0.0f,
    (c45_135 && !cg45_135) ? fe45_135 : 0.0f);
}

float NISCalcLTI(float p0, float p1, float p2, float p3, float p4, float p5, int phase)
{
  bool selector = (phase <= 32);
  float sel = selector ? p0 : p3;
  float a_min = min(min(p1, p2), sel);
  float a_max = max(max(p1, p2), sel);

  sel = selector ? p2 : p5;
  float b_min = min(min(p3, p4), sel);
  float b_max = max(max(p3, p4), sel);

  float a_cont = a_max - a_min;
  float b_cont = b_max - b_min;
  float cont_ratio = max(a_cont, b_cont) / (min(a_cont, b_cont) + NIS_EPSILON);

  return 1.0f - saturate((cont_ratio - NIS_MIN_CONTRAST_RATIO) * NIS_RATIO_NORM);
}

float NISEvalPoly6(in float pxl[6], int phase)
{
  float y = 0.0f;
  float y_usm = 0.0f;

  FOR_UNROLL (int i = 0; i < 6; ++i)
  {
    y += NISScaleCoef(phase, i) * pxl[i];
    y_usm += NISUSMCoef(phase, i) * pxl[i];
  }

  float y_scale = 1.0f - saturate((y - NIS_SHARP_START_Y) * NIS_SHARP_SCALE_Y);
  float y_sharpness = y_scale * NIS_SHARP_STRENGTH_SCALE + NIS_SHARP_STRENGTH_MIN;
  y_usm *= y_sharpness;

  float y_limit = (y_scale * NIS_SHARP_LIMIT_SCALE + NIS_SHARP_LIMIT_MIN) * y;
  y_usm = clamp(y_usm, -y_limit, y_limit);
  y_usm *= NISCalcLTI(pxl[0], pxl[1], pxl[2], pxl[3], pxl[4], pxl[5], phase);

  return y + y_usm;
}

float NISFilterNormal(in float p[36], int phase_x, int phase_y)
{
  float h_acc = 0.0f;

  FOR_UNROLL (int x = 0; x < 6; ++x)
  {
    float v_acc = 0.0f;
    FOR_UNROLL (int y = 0; y < 6; ++y)
      v_acc += p[y * 6 + x] * NISScaleCoef(phase_y, y);

    h_acc += v_acc * NISScaleCoef(phase_x, x);
  }

  return h_acc;
}

float NISAddDirFilters(in float p[36], float fx, float fy, int phase_x, int phase_y, float4 w)
{
  float f = 0.0f;

  if (w.x > 0.0f)
  {
    float v[6];
    FOR_UNROLL (int i = 0; i < 6; ++i)
      v[i] = lerp(p[i * 6 + 2], p[i * 6 + 3], fx);

    f += NISEvalPoly6(v, phase_y) * w.x;
  }

  if (w.y > 0.0f)
  {
    float v[6];
    FOR_UNROLL (int i = 0; i < 6; ++i)
      v[i] = lerp(p[2 * 6 + i], p[3 * 6 + i], fy);

    f += NISEvalPoly6(v, phase_x) * w.y;
  }

  if (w.z > 0.0f)
  {
    float pphase_b45 = 0.5f + 0.5f * (fx - fy);
    float temp[7];

    temp[1] = lerp(p[2 * 6 + 1], p[1 * 6 + 2], pphase_b45);
    temp[3] = lerp(p[3 * 6 + 2], p[2 * 6 + 3], pphase_b45);
    temp[5] = lerp(p[4 * 6 + 3], p[3 * 6 + 4], pphase_b45);

    pphase_b45 -= 0.5f;
    float a = (pphase_b45 >= 0.0f) ? p[0 * 6 + 2] : p[2 * 6 + 0];
    float b = (pphase_b45 >= 0.0f) ? p[1 * 6 + 3] : p[3 * 6 + 1];
    float c = (pphase_b45 >= 0.0f) ? p[2 * 6 + 4] : p[4 * 6 + 2];
    float d = (pphase_b45 >= 0.0f) ? p[3 * 6 + 5] : p[5 * 6 + 3];

    temp[0] = lerp(p[1 * 6 + 1], a, abs(pphase_b45));
    temp[2] = lerp(p[2 * 6 + 2], b, abs(pphase_b45));
    temp[4] = lerp(p[3 * 6 + 3], c, abs(pphase_b45));
    temp[6] = lerp(p[4 * 6 + 4], d, abs(pphase_b45));

    float v[6];
    float pphase_p45 = fx + fy;
    int offset = 0;
    if (pphase_p45 >= 1.0f)
    {
      offset = 1;
      pphase_p45 -= 1.0f;
    }

    FOR_UNROLL (int i = 0; i < 6; ++i)
      v[i] = temp[i + offset];

    int phase = min(int(pphase_p45 * 64.0f), 63);
    f += NISEvalPoly6(v, phase) * w.z;
  }

  if (w.w > 0.0f)
  {
    float pphase_b135 = 0.5f * (fx + fy);
    float temp[7];

    temp[1] = lerp(p[3 * 6 + 1], p[4 * 6 + 2], pphase_b135);
    temp[3] = lerp(p[2 * 6 + 2], p[3 * 6 + 3], pphase_b135);
    temp[5] = lerp(p[1 * 6 + 3], p[2 * 6 + 4], pphase_b135);

    pphase_b135 -= 0.5f;
    float a = (pphase_b135 >= 0.0f) ? p[5 * 6 + 2] : p[3 * 6 + 0];
    float b = (pphase_b135 >= 0.0f) ? p[4 * 6 + 3] : p[2 * 6 + 1];
    float c = (pphase_b135 >= 0.0f) ? p[3 * 6 + 4] : p[1 * 6 + 2];
    float d = (pphase_b135 >= 0.0f) ? p[2 * 6 + 5] : p[0 * 6 + 3];

    temp[0] = lerp(p[4 * 6 + 1], a, abs(pphase_b135));
    temp[2] = lerp(p[3 * 6 + 2], b, abs(pphase_b135));
    temp[4] = lerp(p[2 * 6 + 3], c, abs(pphase_b135));
    temp[6] = lerp(p[1 * 6 + 4], d, abs(pphase_b135));

    float v[6];
    float pphase_p135 = 1.0f + (fx - fy);
    int offset = 0;
    if (pphase_p135 >= 1.0f)
    {
      offset = 1;
      pphase_p135 -= 1.0f;
    }

    FOR_UNROLL (int i = 0; i < 6; ++i)
      v[i] = temp[i + offset];

    int phase = min(int(pphase_p135 * 64.0f), 63);
    f += NISEvalPoly6(v, phase) * w.w;
  }

  return f;
}
)";

  DeclareFragmentEntryPoint(ss, 0, 1);
  ss << R"(
{
  int2 source_min = int2(u_params.xy);
  int2 source_max = source_min + int2(u_params.zw) - int2(1, 1);

  // v_tex0 is interpolated at destination-pixel centers. In source-texel
  // coordinates this exactly matches NVScaler's:
  //   (dst + 0.5) * source/destination - 0.5
  float2 src_pos = v_tex0 * u_src_size.xy - float2(0.5f, 0.5f);
  int2 base_coord = int2(floor(src_pos));
  float2 phase_frac = frac(src_pos);

  int phase_x = min(int(phase_frac.x * 64.0f), 63);
  int phase_y = min(int(phase_frac.y * 64.0f), 63);

  float p[36];
  FOR_UNROLL (int y = 0; y < 6; ++y)
  {
    FOR_UNROLL (int x = 0; x < 6; ++x)
    {
      p[y * 6 + x] =
        NISLoadY(base_coord + int2(x - 2, y - 2), source_min, source_max);
    }
  }

  float4 edge00 = NISGetEdgeMapAt(p, 1, 1);
  float4 edge01 = NISGetEdgeMapAt(p, 1, 2);
  float4 edge10 = NISGetEdgeMapAt(p, 2, 1);
  float4 edge11 = NISGetEdgeMapAt(p, 2, 2);

  float4 edge0 = lerp(edge00, edge01, phase_frac.x);
  float4 edge1 = lerp(edge10, edge11, phase_frac.x);
  float4 w = lerp(edge0, edge1, phase_frac.y);

  float base_weight = 1.0f - w.x - w.y - w.z - w.w;
  float op_y = NISFilterNormal(p, phase_x, phase_y) * base_weight;
  op_y += NISAddDirFilters(p, phase_frac.x, phase_frac.y, phase_x, phase_y, w);

  float4 op = SAMPLE_TEXTURE_LEVEL(samp0, ClampUV(v_tex0), 0.0f);
  float input_y = NISGetY(op.rgb);
  float correction = op_y - input_y;

  o_col0 = float4(op.rgb + correction, 1.0f);
}
)";

  return ss.str();
}

// AMD FidelityFX Super Resolution 1.0 - EASU
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT
//
// ArcadeDuck adaptation of the non-packed 32-bit EASU path from ffx_fsr1.h.
// The reference gather operations are expanded to explicit clamped texel loads
// so the scaler stays within ArcadeDuck's existing cross-backend display path
// and cannot sample outside an active VRAM display sub-rectangle.
std::string GPUShaderGen::GenerateDisplayFSR1EASUFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);

  ss << R"(
float3 FSR1LoadColor(int2 coord, int2 source_min, int2 source_max)
{
  return LOAD_TEXTURE(samp0, clamp(coord, source_min, source_max), 0).rgb;
}

float FSR1Luma(float3 c)
{
  return c.b * 0.5f + c.r * 0.5f + c.g;
}

void FSR1EasuSet(inout float2 dir, inout float len, float weight,
                 float l_a, float l_b, float l_c, float l_d, float l_e)
{
  float dc = l_d - l_c;
  float cb = l_c - l_b;
  float len_x = max(abs(dc), abs(cb));
  float dir_x = l_d - l_b;
  dir.x += dir_x * weight;
  len_x = saturate(abs(dir_x) / max(len_x, 1.0e-6f));
  len += (len_x * len_x) * weight;

  float ec = l_e - l_c;
  float ca = l_c - l_a;
  float len_y = max(abs(ec), abs(ca));
  float dir_y = l_e - l_a;
  dir.y += dir_y * weight;
  len_y = saturate(abs(dir_y) / max(len_y, 1.0e-6f));
  len += (len_y * len_y) * weight;
}

void FSR1EasuTap(inout float3 accum_color, inout float accum_weight,
                 float2 offset, float2 dir, float2 len,
                 float lob, float clp, float3 color)
{
  float2 v;
  v.x = offset.x * dir.x + offset.y * dir.y;
  v.y = offset.x * -dir.y + offset.y * dir.x;
  v *= len;

  float d2 = min(dot(v, v), clp);
  float wb = (2.0f / 5.0f) * d2 - 1.0f;
  float wa = lob * d2 - 1.0f;
  wb *= wb;
  wa *= wa;
  wb = (25.0f / 16.0f) * wb - (25.0f / 16.0f - 1.0f);
  float w = wb * wa;

  accum_color += color * w;
  accum_weight += w;
}
)";

  DeclareFragmentEntryPoint(ss, 0, 1);
  ss << R"(
{
  int2 source_min = int2(u_params.xy);
  int2 source_size = int2(round(u_src_rect.zw * u_src_size.xy));
  int2 source_max = source_min + source_size - int2(1, 1);
  float2 output_size = u_params.zw;

  // Convert the interpolated active-view UV into an integer output pixel.
  float2 local_uv = (v_tex0 - u_src_rect.xy) / u_src_rect.zw;
  int2 ip = clamp(int2(local_uv * output_size), int2(0, 0), int2(output_size) - int2(1, 1));

  // FsrEasuCon() mapping, including an input viewport offset.
  float2 scale = float2(source_size) / output_size;
  float2 pp = float2(ip) * scale + (0.5f * scale - 0.5f) + float2(source_min);
  float2 fp = floor(pp);
  pp -= fp;
  int2 base = int2(fp);

  // 12-tap EASU footprint:
  //    b c
  //  e f g h
  //  i j k l
  //    n o
  float3 b = FSR1LoadColor(base + int2( 0, -1), source_min, source_max);
  float3 c = FSR1LoadColor(base + int2( 1, -1), source_min, source_max);
  float3 e = FSR1LoadColor(base + int2(-1,  0), source_min, source_max);
  float3 f = FSR1LoadColor(base + int2( 0,  0), source_min, source_max);
  float3 g = FSR1LoadColor(base + int2( 1,  0), source_min, source_max);
  float3 h = FSR1LoadColor(base + int2( 2,  0), source_min, source_max);
  float3 i = FSR1LoadColor(base + int2(-1,  1), source_min, source_max);
  float3 j = FSR1LoadColor(base + int2( 0,  1), source_min, source_max);
  float3 k = FSR1LoadColor(base + int2( 1,  1), source_min, source_max);
  float3 l = FSR1LoadColor(base + int2( 2,  1), source_min, source_max);
  float3 n = FSR1LoadColor(base + int2( 0,  2), source_min, source_max);
  float3 o = FSR1LoadColor(base + int2( 1,  2), source_min, source_max);

  float b_l = FSR1Luma(b);
  float c_l = FSR1Luma(c);
  float e_l = FSR1Luma(e);
  float f_l = FSR1Luma(f);
  float g_l = FSR1Luma(g);
  float h_l = FSR1Luma(h);
  float i_l = FSR1Luma(i);
  float j_l = FSR1Luma(j);
  float k_l = FSR1Luma(k);
  float l_l = FSR1Luma(l);
  float n_l = FSR1Luma(n);
  float o_l = FSR1Luma(o);

  float2 dir = float2(0.0f, 0.0f);
  float len = 0.0f;

  float w_s = (1.0f - pp.x) * (1.0f - pp.y);
  float w_t = pp.x * (1.0f - pp.y);
  float w_u = (1.0f - pp.x) * pp.y;
  float w_v = pp.x * pp.y;

  FSR1EasuSet(dir, len, w_s, b_l, e_l, f_l, g_l, j_l);
  FSR1EasuSet(dir, len, w_t, c_l, f_l, g_l, h_l, k_l);
  FSR1EasuSet(dir, len, w_u, f_l, i_l, j_l, k_l, n_l);
  FSR1EasuSet(dir, len, w_v, g_l, j_l, k_l, l_l, o_l);

  float dir_r = dot(dir, dir);
  if (dir_r < (1.0f / 32768.0f))
  {
    dir = float2(1.0f, 0.0f);
  }
  else
  {
    dir *= 1.0f / sqrt(dir_r);
  }

  len = 0.5f * len;
  len *= len;

  float stretch = dot(dir, dir) / max(max(abs(dir.x), abs(dir.y)), 1.0e-6f);
  float2 len2 = float2(1.0f + (stretch - 1.0f) * len, 1.0f - 0.5f * len);
  float lob = 0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len;
  float clp = 1.0f / lob;

  float3 min4 = min(min(f, g), min(j, k));
  float3 max4 = max(max(f, g), max(j, k));

  float3 accum_color = float3(0.0f, 0.0f, 0.0f);
  float accum_weight = 0.0f;

  FSR1EasuTap(accum_color, accum_weight, float2( 0.0f, -1.0f) - pp, dir, len2, lob, clp, b);
  FSR1EasuTap(accum_color, accum_weight, float2( 1.0f, -1.0f) - pp, dir, len2, lob, clp, c);
  FSR1EasuTap(accum_color, accum_weight, float2(-1.0f,  1.0f) - pp, dir, len2, lob, clp, i);
  FSR1EasuTap(accum_color, accum_weight, float2( 0.0f,  1.0f) - pp, dir, len2, lob, clp, j);
  FSR1EasuTap(accum_color, accum_weight, float2( 0.0f,  0.0f) - pp, dir, len2, lob, clp, f);
  FSR1EasuTap(accum_color, accum_weight, float2(-1.0f,  0.0f) - pp, dir, len2, lob, clp, e);
  FSR1EasuTap(accum_color, accum_weight, float2( 1.0f,  1.0f) - pp, dir, len2, lob, clp, k);
  FSR1EasuTap(accum_color, accum_weight, float2( 2.0f,  1.0f) - pp, dir, len2, lob, clp, l);
  FSR1EasuTap(accum_color, accum_weight, float2( 2.0f,  0.0f) - pp, dir, len2, lob, clp, h);
  FSR1EasuTap(accum_color, accum_weight, float2( 1.0f,  0.0f) - pp, dir, len2, lob, clp, g);
  FSR1EasuTap(accum_color, accum_weight, float2( 1.0f,  2.0f) - pp, dir, len2, lob, clp, o);
  FSR1EasuTap(accum_color, accum_weight, float2( 0.0f,  2.0f) - pp, dir, len2, lob, clp, n);

  float3 pix = accum_color / max(accum_weight, 1.0e-6f);
  pix = min(max4, max(min4, pix));

  o_col0 = float4(pix, 1.0f);
}
)";

  return ss.str();
}

// AMD FidelityFX Super Resolution 1.0 - RCAS
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT
//
// Non-scaling RCAS pass applied after EASU. The initial ArcadeDuck value uses
// AMD's sample default sharpening attenuation of 0.25 stops:
//   exp2(-0.25) = 0.8408964153.
std::string GPUShaderGen::GenerateDisplayFSR1RCASFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  WriteDisplayUniformBuffer(ss);
  DeclareTexture(ss, "samp0", 0, false);

  ss << R"(
CONSTANT float FSR1_RCAS_LIMIT = 0.25f - (1.0f / 16.0f);
CONSTANT float FSR1_RCAS_SHARPNESS = 0.8408964153f;

float3 FSR1RCASLoad(int2 coord, int2 source_max)
{
  return LOAD_TEXTURE(samp0, clamp(coord, int2(0, 0), source_max), 0).rgb;
}

float FSR1Max3(float3 v)
{
  return max(v.x, max(v.y, v.z));
}
)";

  DeclareFragmentEntryPoint(ss, 0, 1);
  ss << R"(
{
  int2 source_size = int2(u_src_size.xy);
  int2 source_max = source_size - int2(1, 1);
  int2 p = clamp(int2(v_tex0 * u_src_size.xy), int2(0, 0), source_max);

  float3 b = FSR1RCASLoad(p + int2( 0, -1), source_max);
  float3 d = FSR1RCASLoad(p + int2(-1,  0), source_max);
  float3 e = FSR1RCASLoad(p, source_max);
  float3 f = FSR1RCASLoad(p + int2( 1,  0), source_max);
  float3 h = FSR1RCASLoad(p + int2( 0,  1), source_max);

  float3 mn4 = min(min(b, d), min(f, h));
  float3 mx4 = max(max(b, d), max(f, h));

  float3 hit_min = min(mn4, e) / max(4.0f * mx4, float3(1.0e-6f, 1.0e-6f, 1.0e-6f));

  float3 hit_max_denom = 4.0f * mn4 - float3(4.0f, 4.0f, 4.0f);
  hit_max_denom = min(hit_max_denom, float3(-1.0e-6f, -1.0e-6f, -1.0e-6f));
  float3 hit_max = (float3(1.0f, 1.0f, 1.0f) - max(mx4, e)) / hit_max_denom;

  float3 lobe_rgb = max(-hit_min, hit_max);
  float lobe = max(-FSR1_RCAS_LIMIT, min(FSR1Max3(lobe_rgb), 0.0f)) * FSR1_RCAS_SHARPNESS;

  float rcp_l = 1.0f / (4.0f * lobe + 1.0f);
  float3 pix = (lobe * (b + d + f + h) + e) * rcp_l;

  o_col0 = float4(pix, 1.0f);
}
)";

  return ss.str();
}

std::string GPUShaderGen::GenerateInterleavedFieldExtractFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"uint2 u_src_offset", "uint u_line_skip"}, true);
  DeclareTexture(ss, "samp0", 0, false);

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true);
  ss << R"(
{
  uint2 tcoord = u_src_offset + uint2(uint(v_pos.x), uint(v_pos.y) << u_line_skip);
  o_col0 = LOAD_TEXTURE(samp0, int2(tcoord), 0);
}
)";

  return ss.str();
}

std::string GPUShaderGen::GenerateDeinterlaceWeaveFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"uint2 u_src_offset", "uint u_render_field", "uint u_line_skip"}, true);
  DeclareTexture(ss, "samp0", 0, false);

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true);
  ss << R"(
{
  uint2 fcoord = uint2(v_pos.xy);
  if ((fcoord.y & 1) != u_render_field)
    discard;

  uint2 tcoord = u_src_offset + uint2(fcoord.x, (fcoord.y / 2u) << u_line_skip);
  o_col0 = LOAD_TEXTURE(samp0, int2(tcoord), 0);
})";

  return ss.str();
}

std::string GPUShaderGen::GenerateDeinterlaceBlendFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareTexture(ss, "samp0", 0, false);
  DeclareTexture(ss, "samp1", 1, false);

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true);
  ss << R"(
{
  uint2 uv = uint2(v_pos.xy);
  float4 c0 = LOAD_TEXTURE(samp0, int2(uv), 0);
  float4 c1 = LOAD_TEXTURE(samp1, int2(uv), 0);
  o_col0 = (c0 + c1) * 0.5f;
}
)";

  return ss.str();
}

std::string GPUShaderGen::GenerateFastMADReconstructFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"uint u_current_field", "uint u_height"}, true);
  DeclareTexture(ss, "samp0", 0, false);
  DeclareTexture(ss, "samp1", 1, false);
  DeclareTexture(ss, "samp2", 2, false);
  DeclareTexture(ss, "samp3", 3, false);

  ss << R"(
CONSTANT float3 SENSITIVITY = float3(0.08f, 0.08f, 0.08f);
)";

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true);
  ss << R"(
{
  int2 uv = int2(int(v_pos.x), int(v_pos.y) >> 1);
  float3 cur = LOAD_TEXTURE(samp0, uv, 0).rgb;

  float3 hn = LOAD_TEXTURE(samp0, uv + int2(0, -1), 0).rgb;
  float3 cn = LOAD_TEXTURE(samp1, uv, 0).rgb;
  float3 ln = LOAD_TEXTURE(samp0, uv + int2(0, 1), 0).rgb;

  float3 ho = LOAD_TEXTURE(samp2, uv + int2(0, -1), 0).rgb;
  float3 co = LOAD_TEXTURE(samp3, uv, 0).rgb;
  float3 lo = LOAD_TEXTURE(samp2, uv + int2(0, 1), 0).rgb;

  float3 mh = abs(hn.rgb - ho.rgb) - SENSITIVITY;
  float3 mc = abs(cn.rgb - co.rgb) - SENSITIVITY;
  float3 ml = abs(ln.rgb - lo.rgb) - SENSITIVITY;
  float3 mmaxv = max(mh, max(mc, ml));
  float mmax = max(mmaxv.r, max(mmaxv.g, mmaxv.b));

  // Is pixel F [n][ x , y ] present in the Current Field f [n] ?
  uint row = uint(v_pos.y);
  if ((row & 1u) == u_current_field)
  {
    // Directly uses the pixel from the Current Field
    o_col0.rgb = cur;
  }
  else if (row > 0u && row < u_height && mmax > 0.0f)
  {
    // Reconstructs the missing pixel as the average of the same pixel from the line above and the
    // line below it in the Current Field.
    o_col0.rgb = (hn + ln) / 2.0;
  }
  else
  {
    // Reconstructs the missing pixel as the same pixel from the Previous Field.
    o_col0.rgb = cn;
  }
  o_col0.a = 1.0f;
}
)";

  return ss.str();
}

std::string GPUShaderGen::GenerateChromaSmoothingFragmentShader()
{
  std::stringstream ss;
  WriteHeader(ss);
  DeclareUniformBuffer(ss, {"uint2 u_sample_offset", "uint2 u_clamp_size"}, true);
  DeclareTexture(ss, "samp0", 0);

  ss << R"(
float3 RGBToYUV(float3 rgb)
{
  return float3(dot(rgb.rgb, float3(0.299f, 0.587f, 0.114f)),
                dot(rgb.rgb, float3(-0.14713f, -0.28886f, 0.436f)),
                dot(rgb.rgb, float3(0.615f, -0.51499f, -0.10001f)));
}

float3 YUVToRGB(float3 yuv)
{
  return float3(dot(yuv, float3(1.0f, 0.0f, 1.13983f)),
                dot(yuv, float3(1.0f, -0.39465f, -0.58060f)),
                dot(yuv, float3(1.0f, 2.03211f, 0.0f)));
}

float3 SampleVRAMAverage2x2(uint2 icoords)
{
  float3 value = LOAD_TEXTURE(samp0, int2(icoords), 0).rgb;
  value += LOAD_TEXTURE(samp0, int2(icoords + uint2(0, 1)), 0).rgb;
  value += LOAD_TEXTURE(samp0, int2(icoords + uint2(1, 0)), 0).rgb;
  value += LOAD_TEXTURE(samp0, int2(icoords + uint2(1, 1)), 0).rgb;
  return value * 0.25;
}
)";

  DeclareFragmentEntryPoint(ss, 0, 1, {}, true);
  ss << R"(
{
  uint2 icoords = uint2(v_pos.xy) + u_sample_offset;
  int2 base = int2(icoords) - 1;
  uint2 low = uint2(max(base & ~1, int2(0, 0)));
  uint2 high = min(low + 2u, u_clamp_size);
  float2 coeff = vec2(base & 1) * 0.5 + 0.25;

  float3 p = LOAD_TEXTURE(samp0, int2(icoords), 0).rgb;
  float3 p00 = SampleVRAMAverage2x2(low);
  float3 p01 = SampleVRAMAverage2x2(uint2(low.x, high.y));
  float3 p10 = SampleVRAMAverage2x2(uint2(high.x, low.y));
  float3 p11 = SampleVRAMAverage2x2(high);

  float3 s = lerp(lerp(p00, p10, coeff.x),
                  lerp(p01, p11, coeff.x),
                  coeff.y);

  float y = RGBToYUV(p).x;
  float2 uv = RGBToYUV(s).yz;
  o_col0 = float4(YUVToRGB(float3(y, uv)), 1.0);
}
)";

  return ss.str();
}

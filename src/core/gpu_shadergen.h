// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

#include "util/shadergen.h"

class GPUShaderGen : public ShaderGen
{
public:
  GPUShaderGen(RenderAPI render_api, bool supports_dual_source_blend, bool supports_framebuffer_fetch);
  ~GPUShaderGen();

  std::string GenerateDisplayVertexShader();
  std::string GenerateDisplayOffscreenVertexShader();
  std::string GenerateDisplayFragmentShader(bool clamp_uv);
  std::string GenerateDisplaySharpBilinearFragmentShader();
  std::string GenerateDisplaySGSR1FragmentShader();
  std::string GenerateDisplayNISFragmentShader();
  std::string GenerateDisplayFSR1EASUFragmentShader();
  std::string GenerateDisplayFSR1RCASFragmentShader();

  std::string GenerateInterleavedFieldExtractFragmentShader();
  std::string GenerateDeinterlaceWeaveFragmentShader();
  std::string GenerateDeinterlaceBlendFragmentShader();
  std::string GenerateFastMADReconstructFragmentShader();

  std::string GenerateChromaSmoothingFragmentShader();

private:
  void WriteDisplayUniformBuffer(std::stringstream& ss);
};

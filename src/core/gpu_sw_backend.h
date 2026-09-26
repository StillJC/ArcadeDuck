// SPDX-FileCopyrightText: 2019-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-FileCopyrightText: 2026 StillJC
// SPDX-License-Identifier: GPL-3.0-only
//
// Modified for ArcadeDuck by StillJC, 2026.

#pragma once

#include "gpu.h"
#include "gpu_backend.h"

#include <array>
#include <tuple>

class GPU_SW_Backend final : public GPUBackend
{
public:
  GPU_SW_Backend();
  ~GPU_SW_Backend() override;

  bool Initialize(bool force_thread) override;
  void Reset() override;

  ALWAYS_INLINE_RELEASE u16 GetPixel(const u32 x, const u32 y) const { return g_vram[VRAM_WIDTH * y + x]; }
  ALWAYS_INLINE_RELEASE void SetPixel(const u32 x, const u32 y, const u16 value)
  {
    g_vram[VRAM_WIDTH * y + x] = value;
  }

  static constexpr u32 DITHER_LUT_SIZE = 512;
  using DitherLUT =
    std::array<std::array<std::array<u8, DITHER_LUT_SIZE>, DITHER_MATRIX_SIZE>, DITHER_MATRIX_SIZE>;
  static constexpr DitherLUT ComputeDitherLUT();

protected:
  union VRAMPixel
  {
    u16 bits;

    BitField<u16, u8, 0, 5> r;
    BitField<u16, u8, 5, 5> g;
    BitField<u16, u8, 10, 5> b;
    BitField<u16, bool, 15, 1> c;
  };

  static constexpr std::tuple<u8, u8> UnpackTexcoord(u16 texcoord)
  {
    return std::make_tuple(static_cast<u8>(texcoord), static_cast<u8>(texcoord >> 8));
  }

  static constexpr std::tuple<u8, u8, u8> UnpackColorRGB24(u32 rgb24)
  {
    return std::make_tuple(static_cast<u8>(rgb24), static_cast<u8>(rgb24 >> 8), static_cast<u8>(rgb24 >> 16));
  }

  ALWAYS_INLINE_RELEASE u16 SampleTexture(const GPUBackendDrawCommand* cmd, u8 texcoord_x, u8 texcoord_y) const;

  void FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color, GPUBackendCommandParameters params) override;
  void UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data, GPUBackendCommandParameters params) override;
  void CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height,
                GPUBackendCommandParameters params) override;

  void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) override;
  void DrawLine(const GPUBackendDrawLineCommand* cmd) override;
  void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd) override;
  void DrawingAreaChanged(const GPUDrawingArea& new_drawing_area, const GSVector4i clamped_drawing_area) override;
  void UpdateCLUT(GPUTexturePaletteReg reg, bool clut_is_8bit) override;
  void FlushRender() override;

  template<bool texture_enable, bool raw_texture_enable, bool transparency_enable, bool dithering_enable>
  void ShadePixel(const GPUBackendDrawCommand* cmd, u32 x, u32 y, u8 color_r, u8 color_g, u8 color_b, u8 texcoord_x,
                  u8 texcoord_y);

  template<bool texture_enable, bool raw_texture_enable, bool transparency_enable>
  void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd);

  using DrawRectangleFunction = void (GPU_SW_Backend::*)(const GPUBackendDrawRectangleCommand* cmd);
  DrawRectangleFunction GetDrawRectangleFunction(bool texture_enable, bool raw_texture_enable,
                                                 bool transparency_enable);

  struct i_deltas
  {
    u32 du_dx, dv_dx;
    u32 dr_dx, dg_dx, db_dx;

    u32 du_dy, dv_dy;
    u32 dr_dy, dg_dy, db_dy;
  };

  struct i_group
  {
    u32 u, v;
    u32 r, g, b;
  };

  template<bool shading_enable, bool texture_enable>
  bool CalcIDeltas(i_deltas& idl, const GPUBackendDrawPolygonCommand::Vertex* A,
                   const GPUBackendDrawPolygonCommand::Vertex* B, const GPUBackendDrawPolygonCommand::Vertex* C);

  template<bool shading_enable, bool texture_enable>
  void AddIDeltas_DX(i_group& ig, const i_deltas& idl, u32 count = 1);

  template<bool shading_enable, bool texture_enable>
  void AddIDeltas_DY(i_group& ig, const i_deltas& idl, u32 count = 1);

  template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
           bool dithering_enable>
  void DrawSpan(const GPUBackendDrawPolygonCommand* cmd, s32 y, s32 x_start, s32 x_bound, i_group ig,
                const i_deltas& idl);

  template<bool shading_enable, bool texture_enable, bool raw_texture_enable, bool transparency_enable,
           bool dithering_enable>
  void DrawTriangle(const GPUBackendDrawPolygonCommand* cmd, const GPUBackendDrawPolygonCommand::Vertex* v0,
                    const GPUBackendDrawPolygonCommand::Vertex* v1, const GPUBackendDrawPolygonCommand::Vertex* v2);

  using DrawTriangleFunction = void (GPU_SW_Backend::*)(const GPUBackendDrawPolygonCommand* cmd,
                                                        const GPUBackendDrawPolygonCommand::Vertex* v0,
                                                        const GPUBackendDrawPolygonCommand::Vertex* v1,
                                                        const GPUBackendDrawPolygonCommand::Vertex* v2);
  DrawTriangleFunction GetDrawTriangleFunction(bool shading_enable, bool texture_enable, bool raw_texture_enable,
                                               bool transparency_enable, bool dithering_enable);

  template<bool shading_enable, bool transparency_enable, bool dithering_enable>
  void DrawLine(const GPUBackendDrawLineCommand* cmd, const GPUBackendDrawLineCommand::Vertex* p0,
                const GPUBackendDrawLineCommand::Vertex* p1);

  using DrawLineFunction = void (GPU_SW_Backend::*)(const GPUBackendDrawLineCommand* cmd,
                                                    const GPUBackendDrawLineCommand::Vertex* p0,
                                                    const GPUBackendDrawLineCommand::Vertex* p1);
  DrawLineFunction GetDrawLineFunction(bool shading_enable, bool transparency_enable, bool dithering_enable);

  GPUDrawingArea m_drawing_area = {};
};

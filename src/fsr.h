#pragma once
#include "common.h"

#include <d3d11.h>

// AMD FSR 3.1 / FSR 4 upscaling via the FidelityFX SDK signed DLLs (Direct3D 12), bridged to the app's
// Direct3D 11 device with shared textures and a shared fence.
//
// Capture video has no depth and no camera jitter, so FSR is fed:
//  - colour: the processed source frame
//  - motion vectors: our own GPU motion estimation (per pixel, pixel units)
//  - depth: constant
class FsrUpscaler {
 public:
  FsrUpscaler();
  ~FsrUpscaler();
  bool Init(ID3D11Device* dev11, ID3D11DeviceContext* ctx11);
  bool Ready() const { return ready_; }
  bool HasMajor(int major) const;  // 3 or 4 available?
  std::string status;
  std::vector<std::string> versions;  // e.g. "FSR 3.1.5", "FSR 4.1.2" as reported by the SDK

  // Upscales `color` (rw x rh, RGBA8) to ow x oh. drawMotion must render per-pixel motion vectors (R16G16_FLOAT,
  // pixel units, pointing to the previous frame) into the given render target. Returns an SRV of the result, or
  // nullptr (then the caller should fall back to another upscaler).
  ID3D11ShaderResourceView* Upscale(int slot, int major, ID3D11Texture2D* color, UINT rw, UINT rh,
                                    const std::function<void(ID3D11RenderTargetView*, UINT, UINT)>& drawMotion, UINT ow,
                                    UINT oh, float mvScale, float sharpness, bool reset);
  std::string ActiveName(int slot) const;
  bool Failed(int major) const { return major == 4 ? failed4_ : failed3_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
  bool ready_ = false, failed3_ = false, failed4_ = false;
};

#pragma once
#include "common.h"

#include <d3d11.h>

// AMD Advanced Media Framework effects, loaded at runtime from the graphics driver (amfrt64.dll):
//  - FRC: frame generation (doubles the frame rate by interpolating in-between frames)
//  - HQ Scaler: AMD VideoSR upscaling (FSR-based video upscaler)
// Everything here runs on the UI thread with the app's D3D11 device.
class AmdFx {
 public:
  AmdFx();
  ~AmdFx();
  bool Init(ID3D11Device* dev, UINT vendorId);
  bool Ready() const { return ready_; }
  std::string status;   // shown in the UI
  std::string version;  // AMF runtime version

  // Frame generation. Call FrcConfigure whenever the source size may have changed.
  bool FrcConfigure(UINT w, UINT h, std::string& err);
  void FrcReset();
  // Submits one RGBA frame. Writes up to 2 output textures in display order (interpolated first, then the real
  // frame). Returned textures belong to AMF and stay valid until the next call. Returns -1 on failure.
  int FrcProcess(ID3D11Texture2D* in, ID3D11Texture2D* outs[2]);

  // AMD VideoSR upscale. slot separates preview/recording instances. algo: 2 = VideoSR 1.0, 4 = VideoSR 1.1.
  // Returns an AMF-owned texture (valid until the next call for that slot) or nullptr.
  ID3D11Texture2D* Scale(int slot, ID3D11Texture2D* in, UINT inW, UINT inH, UINT outW, UINT outH, int algo, float sharp);
  bool ScaleFailed() const { return scaleFailed_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
  bool ready_ = false;
  bool scaleFailed_ = false;
};

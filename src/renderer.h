#pragma once
#include "capture.h"
#include "common.h"

#include <d3d11.h>

struct RenderTarget {
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11ShaderResourceView> srv;
  UINT w = 0, h = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};

struct EnhanceParams {
  int scaler = 0;      // 0 bilinear, 1 bicubic (Catmull-Rom), 2 Lanczos
  float sharpen = 0;   // 0..1
  float denoise = 0;   // 0..1
  float bri = 0;       // -0.2..0.2
  float con = 1;       // 0.5..1.5
  float sat = 1;       // 0..2
};

struct ColorParams {
  bool fullRange = false;
  bool bt709 = true;
};

enum Fit { FitContain = 0, FitCover = 1, FitStretch = 2, FitNative = 3 };

class Renderer {
 public:
  bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::string& err);
  void Upload(const VideoFrame& f);
  bool HasFrame() const { return srcW > 0; }
  UINT srcW = 0, srcH = 0;

  // YUV -> RGB (+ noise reduction) into the internal source-resolution texture.
  void Convert(const ColorParams& c, float denoise);
  // Upscale/sharpen/colour into `t`. splitPx >= 0 shows the original left of that x position.
  void Draw(RenderTarget& t, int fit, const EnhanceParams& e, const ColorParams& c, float splitPx, float lineW);

  bool Ensure(RenderTarget& t, UINT w, UINT h, DXGI_FORMAT fmt);

  // Recording readback: convert `src` to NV12 on the GPU and read it back a couple of frames later.
  void SubmitNV12(RenderTarget& src, int64_t t);
  void CollectNV12(const std::function<void(const uint8_t*, size_t, int64_t)>& sink, bool flushAll);
  void ResetNV12();

  bool ReadRGBA(RenderTarget& src, std::vector<uint8_t>& out);

 private:
  void Pass(ID3D11RenderTargetView* rtv, float x, float y, float w, float h, ID3D11PixelShader* ps, const void* cb,
            size_t cbSize, std::initializer_list<ID3D11ShaderResourceView*> srvs);

  ID3D11Device* dev_ = nullptr;
  ID3D11DeviceContext* ctx_ = nullptr;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> psConvert_, psMain_, psY_, psUV_;
  ComPtr<ID3D11Buffer> cb_;
  ComPtr<ID3D11SamplerState> samp_;
  ComPtr<ID3D11Texture2D> texY_, texUV_;
  ComPtr<ID3D11ShaderResourceView> srvY_, srvUV_;
  RenderTarget rgb_;

  RenderTarget nvY_, nvUV_;
  struct Slot {
    ComPtr<ID3D11Texture2D> y, uv;
    int64_t t = 0;
  };
  std::vector<Slot> slots_;
  std::deque<int> busy_;
  std::vector<int> free_;
  std::vector<uint8_t> packed_;
  ComPtr<ID3D11Texture2D> rgbaStaging_;
};

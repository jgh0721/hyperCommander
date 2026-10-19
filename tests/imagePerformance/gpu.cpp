#include "stdafx.h"
#include "gpu.hpp"
#include <QElapsedTimer>
#include <QColorSpace>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace
{
const char *Shader = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Params : register(b0) { uint sw, sh, dw, dh; uint filter; uint3 padding; };
float cubic(float x, float b, float c) {
 x=abs(x); if(x<1) return ((12-9*b-6*c)*x*x*x+(-18+12*b+6*c)*x*x+(6-2*b))/6;
 if(x<2) return ((-b-6*c)*x*x*x+(6*b+30*c)*x*x+(-12*b-48*c)*x+(8*b+24*c))/6;
 return 0;
}
float sinc(float x) { return abs(x)<1e-6 ? 1 : sin(3.14159265359*x)/(3.14159265359*x); }
float kernel(float x) {
 x=abs(x);
 if(filter==0) return max(0,1-x);
 if(filter==1) return x<3 ? sinc(x)*sinc(x/3) : 0;
 if(filter==2) {
   if(x<1) return ((13./11*x-453./209)*x-3./209)*x+1;
   if(x<2) {x-=1;return ((-6./11*x+270./209)*x-156./209)*x;}
   if(x<3) {x-=2;return ((x/11-45./209)*x+26./209)*x;} return 0;
 }
 return cubic(x,6/(13+7*sqrt(2.)),7/(2+12*sqrt(2.)));
}
float3 linearize(float3 x) { return float3(x.x<=.04045 ? x.x/12.92 : pow((x.x+.055)/1.055,2.4),x.y<=.04045 ? x.y/12.92 : pow((x.y+.055)/1.055,2.4),x.z<=.04045 ? x.z/12.92 : pow((x.z+.055)/1.055,2.4)); }
float3 encode(float3 x) { x=saturate(x);return float3(x.x<=.0031308 ? 12.92*x.x : 1.055*pow(x.x,1/2.4)-.055,x.y<=.0031308 ? 12.92*x.y : 1.055*pow(x.y,1/2.4)-.055,x.z<=.0031308 ? 12.92*x.z : 1.055*pow(x.z,1/2.4)-.055); }
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 if(id.x>=dw || id.y>=dh) return;
 float2 scale=float2(sw,sh)/float2(dw,dh), s=max(float2(1,1),scale);
 float2 p=(float2(id.xy)+.5)*scale-.5;
 float support=filter==0 ? 1 : filter==3 ? 2 : 3;
 float2 radius=support*s;
 float4 sum=0; float weight=0;
 for(int y=(int)ceil(p.y-radius.y);y<=(int)floor(p.y+radius.y);++y)
 for(int x=(int)ceil(p.x-radius.x);x<=(int)floor(p.x+radius.x);++x) {
   float2 d=(float2(x,y)-p)/s;
   float w=filter==3 ? kernel(length(d)) : kernel(d.x)*kernel(d.y);
   float4 v=src.Load(int3(clamp(int2(x,y),int2(0,0),int2(sw-1,sh-1)),0));
   v.rgb=linearize(v.rgb)*v.a;sum+=w*v;weight+=w;
 }
 float a=abs(weight)>1e-8 ? saturate(sum.a/weight) : 0;
 float3 rgb=a>1e-8 ? encode(sum.rgb/weight/a) : 0;
 dst[id.xy]=float4(rgb,a);
})";
}
struct GpuResizer::State
{
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> params_;
    QString adapter_, error_;
};
GpuResizer::GpuResizer(bool Software) : state_(std::make_unique<State>())
{
    auto &S=*state_;D3D_FEATURE_LEVEL Level;
    auto Hr=D3D11CreateDevice(nullptr,Software?D3D_DRIVER_TYPE_WARP:D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&S.device_,&Level,&S.context_);
    if(FAILED(Hr)){S.error_=QString("D3D11CreateDevice: %1").arg(quint32(Hr),8,16);return;}
    ComPtr<IDXGIDevice> Dxgi;ComPtr<IDXGIAdapter> Adapter;DXGI_ADAPTER_DESC Desc{};
    S.device_.As(&Dxgi);Dxgi->GetAdapter(&Adapter);Adapter->GetDesc(&Desc);S.adapter_=QString::fromWCharArray(Desc.Description);
    ComPtr<ID3DBlob> Code,Error;
    Hr=D3DCompile(Shader,std::strlen(Shader),nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&Code,&Error);
    if(FAILED(Hr)){S.error_=Error?QString::fromUtf8((char*)Error->GetBufferPointer()):"D3DCompile";return;}
    Hr=S.device_->CreateComputeShader(Code->GetBufferPointer(),Code->GetBufferSize(),nullptr,&S.shader_);
    if(FAILED(Hr)){S.error_="CreateComputeShader";return;}
    D3D11_BUFFER_DESC B{};B.ByteWidth=32;B.Usage=D3D11_USAGE_DEFAULT;B.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(S.device_->CreateBuffer(&B,nullptr,&S.params_)))S.error_="CreateBuffer";
}
GpuResizer::~GpuResizer()=default;
GpuResizeResult GpuResizer::Resize(const QImage &Input,QSize Output,const QString &Filter)
{
    auto &S=*state_;GpuResizeResult R;R.adapter_=S.adapter_;
    if(!S.error_.isEmpty()){R.error_=S.error_;return R;}
    QElapsedTimer T;T.start();const auto I=Input.convertToFormat(QImage::Format_RGBA8888);
    D3D11_TEXTURE2D_DESC D{};D.Width=I.width();D.Height=I.height();D.MipLevels=1;D.ArraySize=1;D.Format=DXGI_FORMAT_R8G8B8A8_UNORM;D.SampleDesc.Count=1;D.Usage=D3D11_USAGE_IMMUTABLE;D.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA Data{I.constBits(),UINT(I.bytesPerLine()),0};ComPtr<ID3D11Texture2D> Source,Target,Staging;ComPtr<ID3D11ShaderResourceView> View;ComPtr<ID3D11UnorderedAccessView> Uav;
    auto Fail=[&](HRESULT Hr){if(FAILED(Hr)){R.error_=QString("D3D11 resource: %1").arg(quint32(Hr),8,16);return true;}return false;};
    if(Fail(S.device_->CreateTexture2D(&D,&Data,&Source))||Fail(S.device_->CreateShaderResourceView(Source.Get(),nullptr,&View)))return R;
    D.Width=Output.width();D.Height=Output.height();D.Usage=D3D11_USAGE_DEFAULT;D.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    if(Fail(S.device_->CreateTexture2D(&D,nullptr,&Target))||Fail(S.device_->CreateUnorderedAccessView(Target.Get(),nullptr,&Uav)))return R;
    D.Usage=D3D11_USAGE_STAGING;D.BindFlags=0;D.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(Fail(S.device_->CreateTexture2D(&D,nullptr,&Staging)))return R;
    R.uploadMs_=T.nsecsElapsed()/1e6;T.restart();
    UINT Params[8]={UINT(I.width()),UINT(I.height()),UINT(Output.width()),UINT(Output.height()),Filter=="bilinear"?0u:Filter=="spline36"?2u:Filter=="ewaRobidouxSharp"?3u:1u,0,0,0};
    S.context_->UpdateSubresource(S.params_.Get(),0,nullptr,Params,0,0);
    S.context_->CSSetShader(S.shader_.Get(),nullptr,0);auto* P=S.params_.Get();auto* V=View.Get();auto* U=Uav.Get();
    S.context_->CSSetConstantBuffers(0,1,&P);S.context_->CSSetShaderResources(0,1,&V);S.context_->CSSetUnorderedAccessViews(0,1,&U,nullptr);
    S.context_->Dispatch((D.Width+7)/8,(D.Height+7)/8,1);
    // Map is the completion boundary. Include dispatch, synchronization and transfer,
    // rather than reporting asynchronous command submission as GPU execution time.
    S.context_->CopyResource(Staging.Get(),Target.Get());D3D11_MAPPED_SUBRESOURCE M{};
    if(Fail(S.context_->Map(Staging.Get(),0,D3D11_MAP_READ,0,&M)))return R;
    R.processMs_=T.nsecsElapsed()/1e6;T.restart();R.image_=QImage(Output,QImage::Format_RGBA8888);
    for(int Y=0;Y<Output.height();++Y)std::memcpy(R.image_.scanLine(Y),(const char*)M.pData+Y*M.RowPitch,Output.width()*4);
    S.context_->Unmap(Staging.Get(),0);V=nullptr;U=nullptr;S.context_->CSSetShaderResources(0,1,&V);S.context_->CSSetUnorderedAccessViews(0,1,&U,nullptr);
    R.readbackMs_=T.nsecsElapsed()/1e6;R.image_.setColorSpace(Input.colorSpace());return R;
}

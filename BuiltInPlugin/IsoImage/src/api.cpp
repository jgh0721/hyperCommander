// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (C) 2015-2022 Chris Lalancette; C++ modifications (C) 2026 HyperCommander contributors.
#include "image.hpp"
#include <cstring>

struct HcIsoImage { hc::iso::Reader reader_; HcIsoImage(const std::filesystem::path& Path, uint32_t Namespace) : reader_(Path, Namespace) {} };
namespace {
thread_local std::string LastError;
template<class F> int Boundary(F&& Function) noexcept
{
    try { LastError.clear(); Function(); return HC_ISO_OK; }
    catch (const hc::iso::Error& E) { LastError = E.what(); return E.status_; }
    catch (const std::exception& E) { LastError = E.what(); return HC_ISO_IO; }
    catch (...) { LastError = "Unknown ISO engine failure"; return HC_ISO_IO; }
}
}
extern "C" {
const char* hcIsoLastError() { return LastError.c_str(); }
int hcIsoOpen(const char* Path, uint32_t Namespace, HcIsoImage** Image)
{
    if (Image) *Image = nullptr;
    return Boundary([&] { hc::iso::Require(Image != nullptr, "Missing image output"); *Image = new HcIsoImage(hc::iso::DiskPath(Path), Namespace); });
}
void hcIsoClose(HcIsoImage* Image) { delete Image; }
int hcIsoGetInfo(const HcIsoImage* Image, HcIsoInfo* Info)
{ return Boundary([&] { hc::iso::Require(Image && Info && Info->structSize_ == sizeof(*Info), "Invalid info arguments"); *Info = Image->reader_.Info(); }); }
int hcIsoGetEntry(const HcIsoImage* Image, uint64_t Index, HcIsoEntry* Entry)
{
    return Boundary([&] {
        hc::iso::Require(Image && Entry && Entry->structSize_ == sizeof(*Entry) && Index < Image->reader_.Entries().size(), "Invalid entry arguments");
        const auto& Item = Image->reader_.Entries()[size_t(Index)]; Entry->path_ = Item.path_.c_str(); Entry->directory_ = Item.directory_; Entry->size_ = Item.size_;
    });
}
int hcIsoRead(HcIsoImage* Image, uint64_t Index, uint64_t Offset, void* Buffer, uint32_t Capacity, uint32_t* Read)
{
    if (Read) *Read = 0;
    return Boundary([&] {
        hc::iso::Require(Image && Read && (Buffer || Capacity == 0) && Index <= SIZE_MAX, "Invalid read arguments");
        *Read = Image->reader_.Read(size_t(Index), Offset, {static_cast<uint8_t*>(Buffer), Capacity});
    });
}
int hcIsoBuild(const char* StagingPath, const HcIsoSource* Sources, uint64_t Count, const HcIsoOptions* Options)
{
    return Boundary([&] { hc::iso::Require(Options != nullptr, "Missing build options"); hc::iso::Writer(Sources, Count, *Options).Write(hc::iso::DiskPath(StagingPath)); });
}
}

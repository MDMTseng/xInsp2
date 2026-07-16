// mf_enc.cpp — native MediaFoundation hardware H.264 encode CPU probe.
// Feeds N system-memory NV12 frames through the MF Sink Writer with hardware
// transforms enabled (uses the platform's hardware encoder MFT — QSV on Intel,
// AMF on AMD, NVENC on NVIDIA: vendor-agnostic on Windows). Measures process CPU
// (GetProcessTimes: user+kernel) over the encode loop -> "cores busy". This
// removes ffmpeg's per-frame framework overhead to expose the native floor.
//
// Build: cl /O2 /EHsc /std:c++17 mf_enc.cpp /link mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib
// Run:   mf_enc.exe [W] [H] [N] [fps]
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <chrono>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

// This SDK's mfreadwrite.h didn't expose the attribute name; the GUID itself
// lives in mfuuid.lib, so declare it extern (value per MS docs).
#ifndef MF_SINK_WRITER_ENABLE_HARDWARE_TRANSFORMS
EXTERN_GUID(MF_SINK_WRITER_ENABLE_HARDWARE_TRANSFORMS,
            0xa634a91c, 0x822b, 0x41b9, 0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0);
#endif

#define CK(hr, msg) do{ HRESULT _h=(hr); if(FAILED(_h)){ std::fprintf(stderr,"FAIL %s: 0x%08lx\n",msg,(unsigned long)_h); return 2; } }while(0)

static double cpu_seconds() {
    FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto to = [](FILETIME f){ ULARGE_INTEGER x; x.LowPart=f.dwLowDateTime; x.HighPart=f.dwHighDateTime; return double(x.QuadPart)*1e-7; };
    return to(k) + to(u);   // kernel + user, in seconds
}

int main(int argc, char** argv) {
    UINT32 W = argc>1?atoi(argv[1]):2448;
    UINT32 H = argc>2?atoi(argv[2]):2048;
    int    N = argc>3?atoi(argv[3]):120;
    UINT32 FPS = argc>4?atoi(argv[4]):30;

    CK(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInit");
    CK(MFStartup(MF_VERSION), "MFStartup");

    IMFAttributes* attr=nullptr;
    CK(MFCreateAttributes(&attr, 3), "MFCreateAttributes");
    attr->SetUINT32(MF_SINK_WRITER_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attr->SetUINT32(MF_LOW_LATENCY, TRUE);

    IMFSinkWriter* writer=nullptr;
    CK(MFCreateSinkWriterFromURL(L"mf_out.mp4", nullptr, attr, &writer), "CreateSinkWriter");

    // output: H.264
    IMFMediaType* outT=nullptr; MFCreateMediaType(&outT);
    outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outT->SetUINT32(MF_MT_AVG_BITRATE, 20000000);
    outT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outT, MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(outT, MF_MT_FRAME_RATE, FPS, 1);
    MFSetAttributeRatio(outT, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream=0;
    CK(writer->AddStream(outT, &stream), "AddStream");

    // input: NV12
    IMFMediaType* inT=nullptr; MFCreateMediaType(&inT);
    inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inT, MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(inT, MF_MT_FRAME_RATE, FPS, 1);
    MFSetAttributeRatio(inT, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    CK(writer->SetInputMediaType(stream, inT, nullptr), "SetInputMediaType");

    CK(writer->BeginWriting(), "BeginWriting");

    const DWORD ysize = W*H, csize = W*H/2, fsize = ysize+csize;
    std::vector<uint8_t> nv12(fsize);
    for (DWORD i=0;i<ysize;i++) nv12[i]=(uint8_t)(i*7);        // luma pattern
    for (DWORD i=0;i<csize;i++) nv12[ysize+i]=128;             // neutral chroma

    LONGLONG dur = 10000000LL / FPS;   // 100ns units
    auto wall0 = std::chrono::steady_clock::now();
    double cpu0 = cpu_seconds();

    for (int f=0; f<N; ++f) {
        IMFMediaBuffer* buf=nullptr;
        if (FAILED(MFCreateMemoryBuffer(fsize, &buf))) return 2;
        BYTE* p=nullptr; DWORD mx=0; buf->Lock(&p,&mx,nullptr);
        // vary a few bytes per frame so it's not a trivial static stream
        nv12[(f*131)%ysize] = (uint8_t)f;
        memcpy(p, nv12.data(), fsize);
        buf->Unlock(); buf->SetCurrentLength(fsize);
        IMFSample* smp=nullptr; MFCreateSample(&smp);
        smp->AddBuffer(buf);
        smp->SetSampleTime((LONGLONG)f*dur);
        smp->SetSampleDuration(dur);
        HRESULT hr = writer->WriteSample(stream, smp);
        smp->Release(); buf->Release();
        if (FAILED(hr)) { std::fprintf(stderr,"WriteSample %d: 0x%08lx\n", f,(unsigned long)hr); return 2; }
    }
    CK(writer->Finalize(), "Finalize");

    double cpu1 = cpu_seconds();
    auto wall1 = std::chrono::steady_clock::now();
    double wall = std::chrono::duration<double>(wall1-wall0).count();
    double cpu = cpu1-cpu0;

    std::printf("MF hardware H.264  %ux%u  %d frames @ %ufps\n", W,H,N,FPS);
    std::printf("  wall        : %.3f s  (%.0f fps)\n", wall, N/wall);
    std::printf("  CPU time    : %.3f s  (user+kernel)\n", cpu);
    std::printf("  CPU cores   : %.3f  (cpu/wall)\n", cpu/wall);
    std::printf("  CPU per frame: %.2f ms\n", cpu/N*1000.0);

    writer->Release(); outT->Release(); inT->Release(); attr->Release();
    MFShutdown(); CoUninitialize();
    return 0;
}

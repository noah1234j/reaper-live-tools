#include "MusicSheetRender.h"

#ifdef _WIN32

#include <objbase.h>
#include <gdiplus.h>
#include <roapi.h>
#include <shcore.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.data.pdf.h>
#include <windows.storage.h>
#include <windows.storage.streams.h>
#include <windows.foundation.h>

#include <atomic>
#include <thread>

#pragma comment(lib, "gdiplus.lib")

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using Microsoft::WRL::Wrappers::HStringReference;
namespace WF  = ABI::Windows::Foundation;
namespace WSS = ABI::Windows::Storage::Streams;
namespace WST = ABI::Windows::Storage;
namespace WDP = ABI::Windows::Data::Pdf;

// ---------------------------------------------------------------------------
// GDI+ lifetime
//
// Started on first use rather than at plugin load: a rig that never opens a
// sheet should not pay for the imaging stack at all. Started on the main
// thread only (MusicSheetRender_Startup); the workers just check the token,
// which was set before they were launched.
// ---------------------------------------------------------------------------
static ULONG_PTR         s_gdiplusToken = 0;
static bool              s_gdiplusFailed = false;
static std::atomic<bool> s_gdiplusReady{ false };
static std::atomic<int>  s_workers{ 0 };       // load threads still running

bool MusicSheetRender_Startup()
{
    if (s_gdiplusToken) return true;
    if (s_gdiplusFailed) return false;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&s_gdiplusToken, &in, nullptr) != Gdiplus::Ok)
    {
        s_gdiplusFailed = true;
        return false;
    }
    s_gdiplusReady.store(true);
    return true;
}

static bool EnsureGdiplus()
{
    return s_gdiplusReady.load();
}

void MusicSheetRender_Cleanup()
{
    MusicSheetRender_Cancel();
    if (s_gdiplusToken)
    {
        // A worker still decoding when the plugin unloads would be using it;
        // give an in-flight one a moment to notice it has been cancelled.
        for (int i = 0; i < 50 && s_workers.load() > 0; ++i) Sleep(20);
        s_gdiplusReady.store(false);
        Gdiplus::GdiplusShutdown(s_gdiplusToken);
        s_gdiplusToken = 0;
    }
}

// ---------------------------------------------------------------------------
// A 32-bit top-down DIB, pre-filled white.
//
// White rather than transparent: a score is ink on paper, and a PDF page
// renders with a transparent background that would otherwise show whatever
// the window happens to be painted in.
// ---------------------------------------------------------------------------
static HBITMAP MakePageBitmap(int w, int h, void** bitsOut)
{
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return nullptr;
    if (bits) memset(bits, 0xFF, (size_t)w * (size_t)h * 4);
    if (bitsOut) *bitsOut = bits;
    return bmp;
}

static std::wstring Widen(const char* utf8)
{
    if (!utf8 || !*utf8) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    return w;
}

// ---------------------------------------------------------------------------
// Images - one page, via GDI+
// ---------------------------------------------------------------------------
static bool LoadImageDoc(const std::wstring& wpath, SheetDoc* doc)
{
    if (!EnsureGdiplus()) { doc->error = "Imaging is unavailable on this system."; return false; }

    Gdiplus::Bitmap src(wpath.c_str(), FALSE);
    if (src.GetLastStatus() != Gdiplus::Ok || src.GetWidth() == 0)
    {
        doc->error = "Could not read that image.";
        return false;
    }

    // Scaled to the standard render width, but never blown up past its own
    // size: enlarging a phone photo of a chart only makes it blurrier and
    // costs the memory of the larger bitmap.
    const int sw = (int)src.GetWidth(), sh = (int)src.GetHeight();
    int w = sw < kSheetRenderWidth ? sw : kSheetRenderWidth;
    int h = (int)((double)sh * w / sw + 0.5);
    if (h < 1) h = 1;

    SheetDocPage pg;
    pg.bmp = MakePageBitmap(w, h, nullptr);
    if (!pg.bmp) { doc->error = "Out of memory rendering the image."; return false; }
    pg.w = w; pg.h = h;

    HDC screen = GetDC(nullptr);
    HDC mem    = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, pg.bmp);
    {
        Gdiplus::Graphics g(mem);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.DrawImage(&src, 0, 0, w, h);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    doc->pages.push_back(pg);
    return true;
}

// ---------------------------------------------------------------------------
// PDF - every page, via Windows.Data.Pdf
//
// The WinRT calls are asynchronous and this is a worker thread with no
// message loop, so each one is waited on with an event rather than a
// coroutine: put_Completed signals, the thread blocks, the result is taken.
// Straightforward, and it keeps the whole dependency to headers that ship
// with the Windows SDK.
// ---------------------------------------------------------------------------
// TLogical is the operation's WinRT type (StorageFile*), TIface the ABI
// interface its GetResults hands back (IStorageFile). Both are deduced from
// the call; working one out from the other is what the SDK's own type traits
// are for, and they are not worth the template gymnastics here.
template <typename TLogical, typename TIface>
static bool AwaitOperation(WF::IAsyncOperation<TLogical>* op, ComPtr<TIface>& out)
{
    if (!op) return false;

    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!done) return false;

    using Handler = WF::IAsyncOperationCompletedHandler<TLogical>;
    auto cb = Callback<Handler>(
        [done](WF::IAsyncOperation<TLogical>*, WF::AsyncStatus) -> HRESULT
        {
            SetEvent(done);
            return S_OK;
        });

    if (FAILED(op->put_Completed(cb.Get()))) { CloseHandle(done); return false; }

    // A bounded wait: a PDF that never comes back must not wedge the worker
    // for the life of the session.
    WaitForSingleObject(done, 30000);
    CloseHandle(done);

    return SUCCEEDED(op->GetResults(out.GetAddressOf()));
}

// Activate a WinRT runtime class and hand back the interface wanted.
template <typename TIface>
static HRESULT ActivateAs(const wchar_t* className, ComPtr<TIface>& out)
{
    ComPtr<IInspectable> insp;
    HRESULT hr = RoActivateInstance(HStringReference(className).Get(), &insp);
    if (FAILED(hr)) return hr;
    return insp.As(&out);
}

static bool LoadPdfDoc(const std::wstring& wpath, SheetDoc* doc)
{
    if (!EnsureGdiplus()) { doc->error = "Imaging is unavailable on this system."; return false; }

    // The file, as a StorageFile.
    ComPtr<WST::IStorageFileStatics> fileStatics;
    HRESULT hr = RoGetActivationFactory(
        HStringReference(RuntimeClass_Windows_Storage_StorageFile).Get(),
        __uuidof(WST::IStorageFileStatics),
        reinterpret_cast<void**>(fileStatics.GetAddressOf()));
    if (FAILED(hr))
    {
        doc->error = "This version of Windows cannot render PDFs. "
                     "Export the pages as PNG and attach those instead.";
        return false;
    }

    ComPtr<WF::IAsyncOperation<WST::StorageFile*>> fileOp;
    hr = fileStatics->GetFileFromPathAsync(
        HStringReference(wpath.c_str()).Get(), &fileOp);
    if (FAILED(hr)) { doc->error = "Could not open that PDF."; return false; }

    ComPtr<WST::IStorageFile> file;
    if (!AwaitOperation(fileOp.Get(), file) || !file)
    {
        doc->error = "Could not open that PDF.";
        return false;
    }

    // The document.
    ComPtr<WDP::IPdfDocumentStatics> pdfStatics;
    hr = RoGetActivationFactory(
        HStringReference(RuntimeClass_Windows_Data_Pdf_PdfDocument).Get(),
        __uuidof(WDP::IPdfDocumentStatics),
        reinterpret_cast<void**>(pdfStatics.GetAddressOf()));
    if (FAILED(hr))
    {
        doc->error = "This version of Windows cannot render PDFs. "
                     "Export the pages as PNG and attach those instead.";
        return false;
    }

    ComPtr<WF::IAsyncOperation<WDP::PdfDocument*>> docOp;
    hr = pdfStatics->LoadFromFileAsync(file.Get(), &docOp);
    if (FAILED(hr)) { doc->error = "Could not read that PDF."; return false; }

    ComPtr<WDP::IPdfDocument> pdf;
    if (!AwaitOperation(docOp.Get(), pdf) || !pdf)
    {
        // Password-protected and corrupt files both land here, and the
        // difference is not worth guessing at.
        doc->error = "Could not read that PDF (it may be password-protected).";
        return false;
    }

    UINT32 pageCount = 0;
    pdf->get_PageCount(&pageCount);
    if (pageCount == 0) { doc->error = "That PDF has no pages."; return false; }

    for (UINT32 i = 0; i < pageCount; ++i)
    {
        ComPtr<WDP::IPdfPage> page;
        if (FAILED(pdf->GetPage(i, &page)) || !page) break;

        WF::Size sz = {};
        page->get_Size(&sz);
        if (sz.Width <= 0.0f || sz.Height <= 0.0f) continue;

        const int w = kSheetRenderWidth;
        const int h = (int)((double)sz.Height * w / sz.Width + 0.5);
        if (h < 1) continue;

        // Render the page into an in-memory stream as a bitmap, then decode
        // that with GDI+. The alternative, IPdfRendererNative, wants a D3D
        // device and a DXGI surface for no gain here.
        ComPtr<WSS::IRandomAccessStream> stream;
        hr = ActivateAs(RuntimeClass_Windows_Storage_Streams_InMemoryRandomAccessStream,
                        stream);
        if (FAILED(hr)) break;

        ComPtr<WDP::IPdfPageRenderOptions> opts;
        hr = ActivateAs(RuntimeClass_Windows_Data_Pdf_PdfPageRenderOptions, opts);
        if (SUCCEEDED(hr) && opts)
            opts->put_DestinationWidth((UINT32)w);

        ComPtr<WF::IAsyncAction> renderOp;
        hr = opts ? page->RenderWithOptionsToStreamAsync(stream.Get(), opts.Get(), &renderOp)
                  : page->RenderToStreamAsync(stream.Get(), &renderOp);
        if (FAILED(hr) || !renderOp) break;

        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        auto cb = Callback<WF::IAsyncActionCompletedHandler>(
            [done](WF::IAsyncAction*, WF::AsyncStatus) -> HRESULT
            { SetEvent(done); return S_OK; });
        if (SUCCEEDED(renderOp->put_Completed(cb.Get())))
            WaitForSingleObject(done, 30000);
        CloseHandle(done);
        if (FAILED(renderOp->GetResults())) break;

        // The WinRT stream, as an IStream GDI+ can decode.
        ComPtr<IStream> istream;
        if (FAILED(CreateStreamOverRandomAccessStream(stream.Get(), IID_PPV_ARGS(&istream))))
            break;

        Gdiplus::Bitmap rendered(istream.Get(), FALSE);
        if (rendered.GetLastStatus() != Gdiplus::Ok) break;

        SheetDocPage pg;
        pg.bmp = MakePageBitmap(w, h, nullptr);
        if (!pg.bmp) break;
        pg.w = w; pg.h = h;

        HDC screen = GetDC(nullptr);
        HDC mem    = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, pg.bmp);
        {
            Gdiplus::Graphics g(mem);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(&rendered, 0, 0, w, h);
        }
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);

        doc->pages.push_back(pg);
    }

    if (doc->pages.empty() && doc->error.empty())
        doc->error = "Could not render any page of that PDF.";
    return !doc->pages.empty();
}

// ---------------------------------------------------------------------------
// The worker
//
// One request at a time. A newer one bumps the generation, and a worker whose
// generation is stale throws its result away instead of posting it - flipping
// through scenes cannot pile up documents or deliver pages for a scene that
// has already gone.
// ---------------------------------------------------------------------------
static std::atomic<unsigned> s_generation{ 0 };

void MusicSheetRender_Cancel()
{
    s_generation.fetch_add(1);
}

void MusicSheetRender_Free(SheetDoc* doc)
{
    if (!doc) return;
    for (auto& pg : doc->pages)
        if (pg.bmp) DeleteObject(pg.bmp);
    delete doc;
}

void MusicSheetRender_LoadAsync(const char* absPath, HWND notifyWnd, UINT notifyMsg)
{
    if (!absPath || !*absPath || !notifyWnd) return;

    // Here, on the main thread, so no two workers ever race to start it. A
    // failure is not fatal yet: the worker reports it as the load's error.
    MusicSheetRender_Startup();

    const unsigned myGen = s_generation.fetch_add(1) + 1;
    const std::string path = absPath;

    s_workers.fetch_add(1);
    std::thread([path, notifyWnd, notifyMsg, myGen]()
    {
        struct Done { ~Done() { s_workers.fetch_sub(1); } } done;

        // The worker owns its own apartment. Multithreaded on purpose: the
        // async waits below would deadlock an STA with no message pump, and
        // REAPER's main thread is not ours to re-initialise.
        const HRESULT roHr = RoInitialize(RO_INIT_MULTITHREADED);

        SheetDoc* doc = new SheetDoc();
        doc->path = path;

        const std::wstring wpath = Widen(path.c_str());
        const size_t dot = path.find_last_of('.');
        std::string ext = (dot == std::string::npos) ? "" : path.substr(dot);
        for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

        if (wpath.empty())
            doc->error = "That path cannot be opened.";
        else if (ext == ".pdf")
            LoadPdfDoc(wpath, doc);
        else
            LoadImageDoc(wpath, doc);

        if (SUCCEEDED(roHr)) RoUninitialize();

        // Superseded while we worked: drop it rather than hand the window
        // pages for a sheet it is no longer showing.
        if (s_generation.load() != myGen || !IsWindow(notifyWnd))
        {
            MusicSheetRender_Free(doc);
            return;
        }
        PostMessage(notifyWnd, notifyMsg, 0, (LPARAM)doc);
    }).detach();
}

#else   // !_WIN32

// macOS/Linux: the viewer reports that rendering is not built here yet rather
// than silently showing nothing. PDFKit is the equivalent on macOS and slots
// in behind the same three functions.
void MusicSheetRender_LoadAsync(const char* absPath, HWND notifyWnd, UINT notifyMsg)
{
    if (!notifyWnd) return;
    SheetDoc* doc = new SheetDoc();
    if (absPath) doc->path = absPath;
    doc->error = "Music sheet rendering is not available on this platform yet.";
    PostMessage(notifyWnd, notifyMsg, 0, (LPARAM)doc);
}

void MusicSheetRender_Cancel() {}

void MusicSheetRender_Free(SheetDoc* doc)
{
    if (!doc) return;
    delete doc;
}

bool MusicSheetRender_Startup() { return false; }

void MusicSheetRender_Cleanup() {}

#endif

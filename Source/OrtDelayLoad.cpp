/*
    OrtDelayLoad.cpp - resolve onnxruntime.dll from the plugin's own folder.

    The plugin links against onnxruntime.dll through its import table. When a
    host loads the VST3 dll with a plain LoadLibraryW (no
    LOAD_WITH_ALTERED_SEARCH_PATH), the bundle's Contents/x86_64-win directory
    is NOT in the DLL search path, so Windows cannot find onnxruntime.dll even
    though it sits right next to the plugin - and the host reports an
    anonymous "problem opening the plugin".

    Fix: link with /DELAYLOAD:onnxruntime.dll (MSVC delayimp) and install a
    dliNotePreLoadLibrary hook. On the first ORT call we locate our own dll
    (module of the hook's code address), take its directory and LoadLibraryEx
    the ORT dll from there with an absolute path. Works in every host,
    regardless of its search-path habits. Bonus: if the user never switches to
    the Neural engine, onnxruntime.dll is never loaded at all.

    Windows/MSVC only - compiles to an empty TU elsewhere.
*/
#if defined (_WIN32) && defined (_MSC_VER) && defined (VFEM_NEURAL)

 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif

 #include <windows.h>
 #include <delayimp.h>

static FARPROC WINAPI ortDelayLoadHook (unsigned dliNotify, PDelayLoadInfo pdli)
{
    if (dliNotify != dliNotePreLoadLibrary || pdli == nullptr || pdli->szDll == nullptr)
        return nullptr;

    // we only handle the runtime itself; anything else goes through the
    // default loader logic
    if (lstrcmpiA (pdli->szDll, "onnxruntime.dll") != 0)
        return nullptr;

    // directory of THIS dll (the plugin), not of the host exe
    HMODULE self = nullptr;
    if (! GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR> (&ortDelayLoadHook),
                              &self)
        || self == nullptr)
        return nullptr;

    WCHAR path[MAX_PATH * 2] {};
    const DWORD n = GetModuleFileNameW (self, path, (DWORD) (sizeof (path) / sizeof (path[0])));
    if (n == 0 || n >= (DWORD) (sizeof (path) / sizeof (path[0])))
        return nullptr;

    // cut the file name off, leaving the directory (with the trailing slash)
    for (DWORD i = n; i > 0; --i)
        if (path[i - 1] == L'\\' || path[i - 1] == L'/')
        {
            path[i] = L'\0';
            break;
        }

    if (lstrcatW (path, L"onnxruntime.dll") == nullptr)
        return nullptr;

    // LOAD_WITH_ALTERED_SEARCH_PATH: dependencies of ORT itself (if any) are
    // searched relative to the ORT dll's directory
    const HMODULE ort = LoadLibraryExW (path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    // returning a module here makes the delay-load helper use it instead of
    // calling LoadLibrary itself; NULL -> default behaviour (standard error)
    return reinterpret_cast<FARPROC> (ort);
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = ortDelayLoadHook;

#endif // _WIN32 && _MSC_VER && VFEM_NEURAL

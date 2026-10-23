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
#if defined (_WIN32) && defined (_MSC_VER) && defined (DYV_NEURAL)

 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif

 #include <windows.h>
 #include <delayimp.h>

// Loads onnxruntime.dll by absolute path from the directory of THIS plugin dll.
// Safe to call repeatedly; returns false (never throws) if it cannot be loaded.
static HMODULE loadOrtFromPluginDir()
{
    HMODULE self = nullptr;
    if (! GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR> (&loadOrtFromPluginDir),
                              &self)
        || self == nullptr)
        return nullptr;

    WCHAR path[MAX_PATH * 2] {};
    const DWORD n = GetModuleFileNameW (self, path, (DWORD) (sizeof (path) / sizeof (path[0])));
    if (n == 0 || n >= (DWORD) (sizeof (path) / sizeof (path[0])))
        return nullptr;

    for (DWORD i = n; i > 0; --i)
        if (path[i - 1] == L'\\' || path[i - 1] == L'/')
        {
            path[i] = L'\0';
            break;
        }

    if (lstrcatW (path, L"onnxruntime.dll") == nullptr)
        return nullptr;

    return LoadLibraryExW (path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

bool dyvLoadOrt()
{
    return loadOrtFromPluginDir() != nullptr;
}

static FARPROC WINAPI ortDelayLoadHook (unsigned dliNotify, PDelayLoadInfo pdli)
{
    if (dliNotify != dliNotePreLoadLibrary || pdli == nullptr || pdli->szDll == nullptr)
        return nullptr;
    if (lstrcmpiA (pdli->szDll, "onnxruntime.dll") != 0)
        return nullptr;
    return reinterpret_cast<FARPROC> (loadOrtFromPluginDir());
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = ortDelayLoadHook;

#endif // _WIN32 && _MSC_VER && DYV_NEURAL

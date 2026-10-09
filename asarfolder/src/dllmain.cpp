// dllmain.cpp - asarfolder.dll COM entry points.
#include "folder.h"

#include <new>

HINSTANCE g_hInstance = nullptr;
LONG g_cDllRefs = 0;

static LONG g_cServerLocks = 0;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInstance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow(void) {
    return (g_cDllRefs == 0 && g_cServerLocks == 0) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    static class CFactory : public IClassFactory {
    public:
        STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
            if (!ppv) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_IClassFactory) {
                *ppv = static_cast<IClassFactory*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHOD_(ULONG, AddRef)() override { return 2; }
        STDMETHOD_(ULONG, Release)() override { return 2; }
        STDMETHOD(CreateInstance)(IUnknown* outer, REFIID riid, void** ppv) override {
            if (outer) return CLASS_E_NOAGGREGATION;
            CAsarFolder* folder = new (std::nothrow) CAsarFolder();
            if (!folder) return E_OUTOFMEMORY;
            HRESULT hr = folder->QueryInterface(riid, ppv);
            folder->Release();
            return hr;
        }
        STDMETHOD(LockServer)(BOOL lock) override {
            if (lock) InterlockedIncrement(&g_cServerLocks);
            else InterlockedDecrement(&g_cServerLocks);
            return S_OK;
        }
    } s_factory;

    if (rclsid != CLSID_AsarFolder) return CLASS_E_CLASSNOTAVAILABLE;
    return s_factory.QueryInterface(riid, ppv);
}

STDAPI DllRegisterServer(void) { return RegisterAsarFolder(); }

STDAPI DllUnregisterServer(void) { return UnregisterAsarFolder(); }

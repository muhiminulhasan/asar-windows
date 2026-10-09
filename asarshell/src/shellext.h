// shellext.h - asarshell.dll: Explorer context menu extension for ASAR archives.
#pragma once

#include <windows.h>
#include <shlobj.h>

#include <string>
#include <vector>

// {8E1845A5-6803-4BED-87F1-BCB87BD610CA}
static const GUID CLSID_AsarMenu = {
    0x8E1845A5, 0x6803, 0x4BED, {0x87, 0xF1, 0xBC, 0xB8, 0x7B, 0xD6, 0x10, 0xCA}};

// Context menu command ids (relative to idCmdFirst).
enum {
    CMD_EXTRACT_TO = 0,   // extract into "<archive name>" folder
    CMD_EXTRACT_HERE = 1, // extract into the archive's parent folder
    CMD_OPEN = 2,         // open in asarview.exe
    CMD_PACK = 3,         // pack a directory into "<dirname>.asar"
};

extern HINSTANCE g_hInstance;
extern LONG g_cDllRefs;

std::wstring GetModuleDir();
void ShowError(HWND hwnd, const std::wstring& title, const std::wstring& msg);

// actions.cpp
bool ActionExtract(HWND hwnd, const std::wstring& asarPath, bool intoSubdir);
bool ActionOpen(HWND hwnd, const std::wstring& asarPath);
bool ActionPack(HWND hwnd, const std::wstring& dirPath);

// contextmenu.cpp
class CAsarMenu final : public IShellExtInit, public IContextMenu {
public:
    CAsarMenu();

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IShellExtInit
    STDMETHOD(Initialize)(PCIDLIST_ABSOLUTE pidlFolder, IDataObject* pdtobj,
                          HKEY hkeyProgID) override;

    // IContextMenu
    STDMETHOD(QueryContextMenu)(HMENU hmenu, UINT indexMenu, UINT idCmdFirst,
                                UINT idCmdLast, UINT uFlags) override;
    STDMETHOD(InvokeCommand)(LPCMINVOKECOMMANDINFO pici) override;
    STDMETHOD(GetCommandString)(UINT_PTR idCmd, UINT uType, UINT* pwReserved,
                                CHAR* pszName, UINT cchMax) override;

private:
    ~CAsarMenu();

    LONG m_refs = 1;
    enum class Mode { None, AsarFile, Directory } m_mode = Mode::None;
    std::wstring m_target;  // .asar path or directory path
};

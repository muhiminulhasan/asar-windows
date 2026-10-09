// ui.h - per-item shell UI objects: icons, drag-out data object, menu.
#pragma once

#include "folder.h"

// IExtractIconW: extension-appropriate icon (via SHGetFileInfo with
// SHGFI_USEFILEATTRIBUTES, so it looks exactly like a real file's icon).
HRESULT CreateItemIcon(HWND hwnd, UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                       REFIID riid, void** ppvOut);

// IDataObject with CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS: lets users
// drag/copy files out of the .asar into any folder (the zipfldr model).
// subPath is this folder's in-archive path ("" = root); child PIDLs store
// leaf names, full paths are rebuilt as subPath + "/" + leaf.
HRESULT CreateItemDataObject(const std::wstring& archivePath,
                             const std::wstring& subPath, UINT cidl,
                             PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                             void** ppvOut);

// IContextMenu: Open/Copy/Properties on selected items (manual verbs only;
// no SHCreateDefaultContextMenu: system default menu crashes Explorer on
// some archives when parsing our PIDLs). parentPidl is this folder's
// absolute pidl (for opening subfolders); may be null.
HRESULT CreateItemMenu(const std::wstring& archivePath,
                       const std::wstring& subPath, PCIDLIST_ABSOLUTE parentPidl,
                       UINT cidl, PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                       void** ppvOut);

// IPropertyStore: System.Size + System.ItemNameDisplay so details view and
// FolderItem.Size work (single-item only).
HRESULT CreateItemPropertyStore(UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                                REFIID riid, void** ppvOut);

// Merged context menu: custom Open + system defaults (Copy/Properties/...).
HRESULT CreateCompositeMenu(IContextMenu* custom, IContextMenu* def,
                            REFIID riid, void** ppvOut);

// Modern (Win11) commands, no extra registration needed.
HRESULT CreateItemExplorerCommands(const std::wstring& archivePath,
                                   const std::wstring& subPath,
                                   PCIDLIST_ABSOLUTE parentPidl, UINT cidl,
                                   PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                                   void** ppvOut);

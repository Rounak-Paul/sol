#include "../file_dialog.h"
#include "../logger.h"

#include <windows.h>
#include <shobjidl.h>
#include <commdlg.h>

namespace sol {

std::optional<std::filesystem::path> FileDialog::OpenFile(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::filesystem::path& defaultPath) {
    
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    
    IFileOpenDialog* pFileOpen;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
                                   IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    
    if (SUCCEEDED(hr)) {
        std::wstring wtitle(title.begin(), title.end());
        pFileOpen->SetTitle(wtitle.c_str());
        
        if (!defaultPath.empty()) {
            IShellItem* pFolder;
            std::wstring wpath = defaultPath.wstring();
            hr = SHCreateItemFromParsingName(wpath.c_str(), NULL, IID_PPV_ARGS(&pFolder));
            if (SUCCEEDED(hr)) {
                pFileOpen->SetFolder(pFolder);
                pFolder->Release();
            }
        }
        
        hr = pFileOpen->Show(NULL);
        
        if (SUCCEEDED(hr)) {
            IShellItem* pItem;
            hr = pFileOpen->GetResult(&pItem);
            if (SUCCEEDED(hr)) {
                PWSTR pszFilePath;
                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                if (SUCCEEDED(hr)) {
                    std::filesystem::path result(pszFilePath);
                    CoTaskMemFree(pszFilePath);
                    pItem->Release();
                    pFileOpen->Release();
                    CoUninitialize();
                    return result;
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
    }
    
    CoUninitialize();
    return std::nullopt;
}

std::vector<std::filesystem::path> FileDialog::OpenFiles(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::filesystem::path& defaultPath) {
    
    std::vector<std::filesystem::path> result;
    
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    
    IFileOpenDialog* pFileOpen;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
                                   IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    
    if (SUCCEEDED(hr)) {
        DWORD dwFlags;
        pFileOpen->GetOptions(&dwFlags);
        pFileOpen->SetOptions(dwFlags | FOS_ALLOWMULTISELECT);
        
        std::wstring wtitle(title.begin(), title.end());
        pFileOpen->SetTitle(wtitle.c_str());
        
        hr = pFileOpen->Show(NULL);
        
        if (SUCCEEDED(hr)) {
            IShellItemArray* pItems;
            hr = pFileOpen->GetResults(&pItems);
            if (SUCCEEDED(hr)) {
                DWORD count;
                pItems->GetCount(&count);
                for (DWORD i = 0; i < count; i++) {
                    IShellItem* pItem;
                    pItems->GetItemAt(i, &pItem);
                    PWSTR pszFilePath;
                    pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                    result.push_back(std::filesystem::path(pszFilePath));
                    CoTaskMemFree(pszFilePath);
                    pItem->Release();
                }
                pItems->Release();
            }
        }
        pFileOpen->Release();
    }
    
    CoUninitialize();
    return result;
}

std::optional<std::filesystem::path> FileDialog::OpenFolder(
    const std::string& title,
    const std::filesystem::path& defaultPath) {
    
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    
    IFileOpenDialog* pFileOpen;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
                                   IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    
    if (SUCCEEDED(hr)) {
        DWORD dwFlags;
        pFileOpen->GetOptions(&dwFlags);
        pFileOpen->SetOptions(dwFlags | FOS_PICKFOLDERS);
        
        std::wstring wtitle(title.begin(), title.end());
        pFileOpen->SetTitle(wtitle.c_str());
        
        if (!defaultPath.empty()) {
            IShellItem* pFolder;
            std::wstring wpath = defaultPath.wstring();
            hr = SHCreateItemFromParsingName(wpath.c_str(), NULL, IID_PPV_ARGS(&pFolder));
            if (SUCCEEDED(hr)) {
                pFileOpen->SetFolder(pFolder);
                pFolder->Release();
            }
        }
        
        hr = pFileOpen->Show(NULL);
        
        if (SUCCEEDED(hr)) {
            IShellItem* pItem;
            hr = pFileOpen->GetResult(&pItem);
            if (SUCCEEDED(hr)) {
                PWSTR pszFilePath;
                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                if (SUCCEEDED(hr)) {
                    std::filesystem::path result(pszFilePath);
                    CoTaskMemFree(pszFilePath);
                    pItem->Release();
                    pFileOpen->Release();
                    CoUninitialize();
                    return result;
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
    }
    
    CoUninitialize();
    return std::nullopt;
}

std::optional<std::filesystem::path> FileDialog::SaveFile(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::filesystem::path& defaultPath) {
    
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    
    IFileSaveDialog* pFileSave;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_ALL,
                                   IID_IFileSaveDialog, reinterpret_cast<void**>(&pFileSave));
    
    if (SUCCEEDED(hr)) {
        std::wstring wtitle(title.begin(), title.end());
        pFileSave->SetTitle(wtitle.c_str());
        
        if (!defaultPath.empty()) {
            std::wstring wname = defaultPath.filename().wstring();
            pFileSave->SetFileName(wname.c_str());
        }
        
        hr = pFileSave->Show(NULL);
        
        if (SUCCEEDED(hr)) {
            IShellItem* pItem;
            hr = pFileSave->GetResult(&pItem);
            if (SUCCEEDED(hr)) {
                PWSTR pszFilePath;
                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                if (SUCCEEDED(hr)) {
                    std::filesystem::path result(pszFilePath);
                    CoTaskMemFree(pszFilePath);
                    pItem->Release();
                    pFileSave->Release();
                    CoUninitialize();
                    return result;
                }
                pItem->Release();
            }
        }
        pFileSave->Release();
    }
    
    CoUninitialize();
    return std::nullopt;
}

} // namespace sol

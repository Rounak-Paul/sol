#pragma once

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <filesystem>
#include <functional>
#include <mutex>
#include "text/text_buffer.h"

struct ImGuiInputTextCallbackData;

namespace sol {

enum class ResourceType {
    Unknown,
    Text,
    Image,
    Binary
};

class Resource {
public:
    using Id = size_t;
    
    Resource(const std::filesystem::path& path);
    virtual ~Resource() = default;
    
    Id GetId() const { return m_Id; }
    const std::filesystem::path& GetPath() const { return m_Path; }
    const std::string& GetName() const { return m_Name; }
    ResourceType GetType() const { return m_Type; }
    bool IsModified() const { return m_Modified; }
    void SetModified(bool modified) { m_Modified = modified; }
    
    virtual bool Load() = 0;
    virtual bool Save() = 0;
    
protected:
    Id m_Id;
    std::filesystem::path m_Path;
    std::string m_Name;
    ResourceType m_Type = ResourceType::Unknown;
    bool m_Modified = false;
    
private:
    static Id GenerateId();
};

class TextResource : public Resource {
public:
    TextResource(const std::filesystem::path& path);
    
    bool Load() override;
    bool Save() override;
    
    // TextBuffer access (nvim-like rope + tree-sitter)
    TextBuffer& GetBuffer() { return m_Buffer; }
    const TextBuffer& GetBuffer() const { return m_Buffer; }
    
    // Legacy compatibility
    std::string GetContent() const { return m_Buffer.ToString(); }
    void SetContent(const std::string& content);
    
    // For ImGui InputText - provides a resizable buffer
    struct EditState {
        TextResource* resource;
    };
    static int InputTextCallback(ImGuiInputTextCallbackData* data);
    
private:
    TextBuffer m_Buffer;
};

class Buffer {
public:
    using Id = size_t;
    
    Buffer(std::shared_ptr<Resource> resource);
    
    Id GetId() const { return m_Id; }
    std::shared_ptr<Resource> GetResource() const { return m_Resource; }
    const std::string& GetName() const { return m_Resource->GetName(); }
    bool IsActive() const { return m_Active; }
    void SetActive(bool active) { m_Active = active; }
    bool IsFloating() const { return m_Floating; }
    void SetFloating(bool floating) { m_Floating = floating; }
    
private:
    Id m_Id;
    std::shared_ptr<Resource> m_Resource;
    bool m_Active = false;
    bool m_Floating = false;
    
    static Id GenerateId();
};

class ResourceSystem {
public:
    static ResourceSystem& GetInstance();
    
    ResourceSystem(const ResourceSystem&) = delete;
    ResourceSystem& operator=(const ResourceSystem&) = delete;
    
    // File operations
    std::shared_ptr<Buffer> OpenFile(const std::filesystem::path& path);
    void CloseBuffer(Buffer::Id id);
    void CloseAllBuffers();
    bool SaveBuffer(Buffer::Id id);
    bool SaveAllBuffers();
    
    // Folder operations
    void SetWorkingDirectory(const std::filesystem::path& path);
    const std::filesystem::path& GetWorkingDirectory() const { return m_WorkingDirectory; }
    bool HasWorkingDirectory() const { return !m_WorkingDirectory.empty(); }
    
    // Buffer access
    std::shared_ptr<Buffer> GetBuffer(Buffer::Id id);
    std::shared_ptr<Buffer> GetActiveBuffer();
    void SetActiveBuffer(Buffer::Id id);
    const std::vector<std::shared_ptr<Buffer>>& GetBuffers() const { return m_Buffers; }
    
    // Callbacks
    using BufferCallback = std::function<void(std::shared_ptr<Buffer>)>;
    void SetOnBufferOpened(BufferCallback callback) { m_OnBufferOpened = callback; }
    void SetOnBufferClosed(BufferCallback callback) { m_OnBufferClosed = callback; }
    void SetOnActiveBufferChanged(BufferCallback callback) { m_OnActiveBufferChanged = callback; }
    
private:
    ResourceSystem() = default;
    ~ResourceSystem() = default;
    
    std::shared_ptr<Resource> CreateResource(const std::filesystem::path& path);
    ResourceType DetectResourceType(const std::filesystem::path& path);
    
    mutable std::recursive_mutex m_Mutex;
    std::filesystem::path m_WorkingDirectory;
    std::vector<std::shared_ptr<Buffer>> m_Buffers;
    std::map<std::filesystem::path, std::shared_ptr<Resource>> m_ResourceCache;
    Buffer::Id m_ActiveBufferId = 0;
    
    BufferCallback m_OnBufferOpened;
    BufferCallback m_OnBufferClosed;
    BufferCallback m_OnActiveBufferChanged;
};

} // namespace sol

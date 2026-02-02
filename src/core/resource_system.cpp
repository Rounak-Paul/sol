#include "resource_system.h"
#include "logger.h"
#include <fstream>
#include <sstream>
#include <imgui.h>

namespace sol {

// Resource implementation
Resource::Id Resource::GenerateId() {
    static Id nextId = 1;
    return nextId++;
}

Resource::Resource(const std::filesystem::path& path)
    : m_Id(GenerateId()), m_Path(path), m_Name(path.filename().string()) {
}

// TextResource implementation
TextResource::TextResource(const std::filesystem::path& path)
    : Resource(path) {
    m_Type = ResourceType::Text;
}

bool TextResource::Load() {
    std::ifstream file(m_Path);
    if (!file.is_open()) {
        Logger::Error("Failed to open file: " + m_Path.string());
        return false;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    m_Content = buffer.str();
    m_Modified = false;
    
    Logger::Info("Loaded file: " + m_Path.string());
    return true;
}

bool TextResource::Save() {
    std::ofstream file(m_Path);
    if (!file.is_open()) {
        Logger::Error("Failed to save file: " + m_Path.string());
        return false;
    }
    
    file << m_Content;
    m_Modified = false;
    
    Logger::Info("Saved file: " + m_Path.string());
    return true;
}

void TextResource::SetContent(const std::string& content) {
    if (m_Content != content) {
        m_Content = content;
        m_Modified = true;
    }
}

int TextResource::InputTextCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* str = static_cast<std::string*>(data->UserData);
        str->resize(data->BufTextLen);
        data->Buf = str->data();
    }
    return 0;
}

// Buffer implementation
Buffer::Id Buffer::GenerateId() {
    static Id nextId = 1;
    return nextId++;
}

Buffer::Buffer(std::shared_ptr<Resource> resource)
    : m_Id(GenerateId()), m_Resource(resource) {
}

// ResourceSystem implementation
ResourceSystem& ResourceSystem::GetInstance() {
    static ResourceSystem instance;
    return instance;
}

std::shared_ptr<Buffer> ResourceSystem::OpenFile(const std::filesystem::path& path) {
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    
    // Check if already open
    for (auto& buffer : m_Buffers) {
        if (buffer->GetResource()->GetPath() == path) {
            SetActiveBuffer(buffer->GetId());
            return buffer;
        }
    }
    
    // Create and load resource
    auto resource = CreateResource(path);
    if (!resource || !resource->Load()) {
        return nullptr;
    }
    
    // Create buffer
    auto buffer = std::make_shared<Buffer>(resource);
    m_Buffers.push_back(buffer);
    
    if (m_OnBufferOpened) {
        m_OnBufferOpened(buffer);
    }
    
    SetActiveBuffer(buffer->GetId());
    return buffer;
}

void ResourceSystem::CloseBuffer(Buffer::Id id) {
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    
    auto it = std::find_if(m_Buffers.begin(), m_Buffers.end(),
        [id](const auto& b) { return b->GetId() == id; });
    
    if (it != m_Buffers.end()) {
        auto buffer = *it;
        m_Buffers.erase(it);
        
        if (m_OnBufferClosed) {
            m_OnBufferClosed(buffer);
        }
        
        // Update active buffer
        if (m_ActiveBufferId == id) {
            m_ActiveBufferId = m_Buffers.empty() ? 0 : m_Buffers.back()->GetId();
            if (m_OnActiveBufferChanged && !m_Buffers.empty()) {
                m_OnActiveBufferChanged(m_Buffers.back());
            }
        }
        
        Logger::Info("Closed buffer: " + buffer->GetName());
    }
}

void ResourceSystem::CloseAllBuffers() {
    while (!m_Buffers.empty()) {
        CloseBuffer(m_Buffers.back()->GetId());
    }
}

bool ResourceSystem::SaveBuffer(Buffer::Id id) {
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    
    auto buffer = GetBuffer(id);
    if (buffer) {
        return buffer->GetResource()->Save();
    }
    return false;
}

bool ResourceSystem::SaveAllBuffers() {
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    
    bool allSaved = true;
    for (auto& buffer : m_Buffers) {
        if (buffer->GetResource()->IsModified()) {
            if (!buffer->GetResource()->Save()) {
                allSaved = false;
            }
        }
    }
    return allSaved;
}

void ResourceSystem::SetWorkingDirectory(const std::filesystem::path& path) {
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    
    if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
        m_WorkingDirectory = path;
        Logger::Info("Working directory set to: " + path.string());
    } else {
        Logger::Error("Invalid directory: " + path.string());
    }
}

std::shared_ptr<Buffer> ResourceSystem::GetBuffer(Buffer::Id id) {
    for (auto& buffer : m_Buffers) {
        if (buffer->GetId() == id) {
            return buffer;
        }
    }
    return nullptr;
}

std::shared_ptr<Buffer> ResourceSystem::GetActiveBuffer() {
    return GetBuffer(m_ActiveBufferId);
}

void ResourceSystem::SetActiveBuffer(Buffer::Id id) {
    std::lock_guard<std::recursive_mutex> lock(m_Mutex);
    
    if (m_ActiveBufferId != id) {
        // Deactivate old buffer
        if (auto oldBuffer = GetBuffer(m_ActiveBufferId)) {
            oldBuffer->SetActive(false);
        }
        
        m_ActiveBufferId = id;
        
        // Activate new buffer
        if (auto newBuffer = GetBuffer(id)) {
            newBuffer->SetActive(true);
            if (m_OnActiveBufferChanged) {
                m_OnActiveBufferChanged(newBuffer);
            }
        }
    }
}

std::shared_ptr<Resource> ResourceSystem::CreateResource(const std::filesystem::path& path) {
    // Check cache
    auto it = m_ResourceCache.find(path);
    if (it != m_ResourceCache.end()) {
        return it->second;
    }
    
    if (!std::filesystem::exists(path)) {
        Logger::Error("File does not exist: " + path.string());
        return nullptr;
    }
    
    auto type = DetectResourceType(path);
    std::shared_ptr<Resource> resource;
    
    switch (type) {
        case ResourceType::Text:
            resource = std::make_shared<TextResource>(path);
            break;
        default:
            Logger::Warning("Unsupported file type: " + path.string());
            resource = std::make_shared<TextResource>(path); // Fallback to text
            break;
    }
    
    m_ResourceCache[path] = resource;
    return resource;
}

ResourceType ResourceSystem::DetectResourceType(const std::filesystem::path& path) {
    static const std::map<std::string, ResourceType> extensionMap = {
        {".txt", ResourceType::Text},
        {".cpp", ResourceType::Text},
        {".h", ResourceType::Text},
        {".hpp", ResourceType::Text},
        {".c", ResourceType::Text},
        {".py", ResourceType::Text},
        {".js", ResourceType::Text},
        {".ts", ResourceType::Text},
        {".json", ResourceType::Text},
        {".xml", ResourceType::Text},
        {".yaml", ResourceType::Text},
        {".yml", ResourceType::Text},
        {".md", ResourceType::Text},
        {".cmake", ResourceType::Text},
        {".glsl", ResourceType::Text},
        {".vert", ResourceType::Text},
        {".frag", ResourceType::Text},
        {".lua", ResourceType::Text},
        {".sh", ResourceType::Text},
        {".png", ResourceType::Image},
        {".jpg", ResourceType::Image},
        {".jpeg", ResourceType::Image},
        {".bmp", ResourceType::Image},
    };
    
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    auto it = extensionMap.find(ext);
    if (it != extensionMap.end()) {
        return it->second;
    }
    
    return ResourceType::Unknown;
}

} // namespace sol

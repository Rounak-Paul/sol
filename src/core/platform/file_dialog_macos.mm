#include "../file_dialog.h"
#include "../logger.h"

#include <Cocoa/Cocoa.h>
#include <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace sol {

static NSArray<UTType*>* CreateContentTypes(const std::vector<FileFilter>& filters) {
    NSMutableArray<UTType*>* contentTypes = [NSMutableArray array];
    for (const auto& filter : filters) {
        std::string exts = filter.extensions;
        size_t pos = 0;
        while ((pos = exts.find(',')) != std::string::npos) {
            NSString* ext = [NSString stringWithUTF8String:exts.substr(0, pos).c_str()];
            UTType* type = [UTType typeWithFilenameExtension:ext];
            if (type) [contentTypes addObject:type];
            exts.erase(0, pos + 1);
        }
        if (!exts.empty()) {
            NSString* ext = [NSString stringWithUTF8String:exts.c_str()];
            UTType* type = [UTType typeWithFilenameExtension:ext];
            if (type) [contentTypes addObject:type];
        }
    }
    return contentTypes;
}

std::optional<std::filesystem::path> FileDialog::OpenFile(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::filesystem::path& defaultPath) {
    
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        
        if (!defaultPath.empty()) {
            NSString* path = [NSString stringWithUTF8String:defaultPath.c_str()];
            [panel setDirectoryURL:[NSURL fileURLWithPath:path]];
        }
        
        if (!filters.empty()) {
            [panel setAllowedContentTypes:CreateContentTypes(filters)];
        }
        
        if ([panel runModal] == NSModalResponseOK) {
            NSURL* url = [[panel URLs] objectAtIndex:0];
            return std::filesystem::path([[url path] UTF8String]);
        }
    }
    return std::nullopt;
}

std::vector<std::filesystem::path> FileDialog::OpenFiles(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::filesystem::path& defaultPath) {
    
    std::vector<std::filesystem::path> result;
    
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:YES];
        
        if (!defaultPath.empty()) {
            NSString* path = [NSString stringWithUTF8String:defaultPath.c_str()];
            [panel setDirectoryURL:[NSURL fileURLWithPath:path]];
        }
        
        if ([panel runModal] == NSModalResponseOK) {
            for (NSURL* url in [panel URLs]) {
                result.push_back(std::filesystem::path([[url path] UTF8String]));
            }
        }
    }
    return result;
}

std::optional<std::filesystem::path> FileDialog::OpenFolder(
    const std::string& title,
    const std::filesystem::path& defaultPath) {
    
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        
        if (!defaultPath.empty()) {
            NSString* path = [NSString stringWithUTF8String:defaultPath.c_str()];
            [panel setDirectoryURL:[NSURL fileURLWithPath:path]];
        }
        
        if ([panel runModal] == NSModalResponseOK) {
            NSURL* url = [[panel URLs] objectAtIndex:0];
            return std::filesystem::path([[url path] UTF8String]);
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> FileDialog::SaveFile(
    const std::string& title,
    const std::vector<FileFilter>& filters,
    const std::filesystem::path& defaultPath) {
    
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        [panel setTitle:[NSString stringWithUTF8String:title.c_str()]];
        
        if (!defaultPath.empty()) {
            NSString* path = [NSString stringWithUTF8String:defaultPath.parent_path().c_str()];
            [panel setDirectoryURL:[NSURL fileURLWithPath:path]];
            [panel setNameFieldStringValue:[NSString stringWithUTF8String:defaultPath.filename().c_str()]];
        }
        
        if (!filters.empty()) {
            [panel setAllowedContentTypes:CreateContentTypes(filters)];
        }
        
        if ([panel runModal] == NSModalResponseOK) {
            NSURL* url = [panel URL];
            return std::filesystem::path([[url path] UTF8String]);
        }
    }
    return std::nullopt;
}

} // namespace sol

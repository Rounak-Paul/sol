#include "standard_mode.h"
#include "command.h"
#include "core/text/text_buffer.h"
#include "core/text/undo_tree.h"
#include "ui/editor_settings.h"
#include <algorithm>

namespace sol {

InputResult StandardMode::HandleKeyboard(EditorState& state) {
    InputResult result;
    ImGuiIO& io = ImGui::GetIO();
    
    bool ctrl = io.KeyCtrl;
    bool shift = io.KeyShift;
    bool alt = io.KeyAlt;
    
    // Undo/Redo
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (shift) {
            // Ctrl+Shift+Z = Redo
            if (state.undoTree) {
                auto newPos = TextOps::Redo(*state.buffer, *state.undoTree);
                if (newPos) {
                    result.handled = true;
                    result.textChanged = true;
                    result.newCursorPos = *newPos;
                }
            }
        } else {
            // Ctrl+Z = Undo
            if (state.undoTree) {
                auto newPos = TextOps::Undo(*state.buffer, *state.undoTree);
                if (newPos) {
                    result.handled = true;
                    result.textChanged = true;
                    result.newCursorPos = *newPos;
                }
            }
        }
        return result;
    }
    
    // Ctrl+Y = Redo (alternative)
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        if (state.undoTree) {
            auto newPos = TextOps::Redo(*state.buffer, *state.undoTree);
            if (newPos) {
                result.handled = true;
                result.textChanged = true;
                result.newCursorPos = *newPos;
            }
        }
        return result;
    }
    
    // Configurable navigation keys in Command mode (default: WASD)
    bool isCommandMode = InputSystem::GetInstance().GetInputMode() == EditorInputMode::Command;
    if (isCommandMode && !ctrl && !alt) {
        const auto& keybinds = EditorSettings::Get().GetKeybinds();
        ImGuiKey navKey = ImGuiKey_None;
        if (ImGui::IsKeyPressed(keybinds.navLeft)) navKey = ImGuiKey_LeftArrow;
        else if (ImGui::IsKeyPressed(keybinds.navRight)) navKey = ImGuiKey_RightArrow;
        else if (ImGui::IsKeyPressed(keybinds.navUp)) navKey = ImGuiKey_UpArrow;
        else if (ImGui::IsKeyPressed(keybinds.navDown)) navKey = ImGuiKey_DownArrow;
        
        if (navKey != ImGuiKey_None) {
            return HandleNavigation(state, navKey, shift, false);
        }
    }
    
    // Navigation keys
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
        ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
        ImGui::IsKeyPressed(ImGuiKey_UpArrow) ||
        ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
        ImGui::IsKeyPressed(ImGuiKey_Home) ||
        ImGui::IsKeyPressed(ImGuiKey_End) ||
        ImGui::IsKeyPressed(ImGuiKey_PageUp) ||
        ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
        
        ImGuiKey key = ImGuiKey_None;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) key = ImGuiKey_LeftArrow;
        else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) key = ImGuiKey_RightArrow;
        else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) key = ImGuiKey_UpArrow;
        else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) key = ImGuiKey_DownArrow;
        else if (ImGui::IsKeyPressed(ImGuiKey_Home)) key = ImGuiKey_Home;
        else if (ImGui::IsKeyPressed(ImGuiKey_End)) key = ImGuiKey_End;
        else if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) key = ImGuiKey_PageUp;
        else if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) key = ImGuiKey_PageDown;
        
        return HandleNavigation(state, key, shift, ctrl);
    }
    
    // Editing keys - only in Insert mode
    if (isCommandMode) {
        // Block editing keys in Command mode - only navigation is allowed
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) ||
            ImGui::IsKeyPressed(ImGuiKey_Delete) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            // Consume the key but don't edit
            InputResult blocked;
            blocked.handled = true;
            return blocked;
        }
    }
    
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) ||
        ImGui::IsKeyPressed(ImGuiKey_Delete) ||
        ImGui::IsKeyPressed(ImGuiKey_Enter) ||
        ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        
        ImGuiKey key = ImGuiKey_None;
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) key = ImGuiKey_Backspace;
        else if (ImGui::IsKeyPressed(ImGuiKey_Delete)) key = ImGuiKey_Delete;
        else if (ImGui::IsKeyPressed(ImGuiKey_Enter)) key = ImGuiKey_Enter;
        else if (ImGui::IsKeyPressed(ImGuiKey_Tab)) key = ImGuiKey_Tab;
        
        return HandleEditing(state, key, ctrl);
    }
    
    // Select all
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
        result.handled = true;
        result.selectionChanged = true;
        result.newSelectionStart = 0;
        result.newSelectionEnd = state.buffer->Length();
        result.newCursorPos = state.buffer->Length();
        return result;
    }
    
    // Cut/Copy/Paste
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X)) {
        // Cut
        if (state.hasSelection) {
            size_t start = std::min(state.selectionStart, state.selectionEnd);
            size_t end = std::max(state.selectionStart, state.selectionEnd);
            std::string selected = state.buffer->Substring(start, end - start);
            ImGui::SetClipboardText(selected.c_str());
            DeleteSelection(state);
            result.handled = true;
            result.textChanged = true;
            result.newCursorPos = start;
            result.selectionChanged = true;
            result.newSelectionStart = start;
            result.newSelectionEnd = start;
        }
        return result;
    }
    
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
        // Copy
        if (state.hasSelection) {
            size_t start = std::min(state.selectionStart, state.selectionEnd);
            size_t end = std::max(state.selectionStart, state.selectionEnd);
            std::string selected = state.buffer->Substring(start, end - start);
            ImGui::SetClipboardText(selected.c_str());
            result.handled = true;
        }
        return result;
    }
    
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
        // Paste
        const char* clipboard = ImGui::GetClipboardText();
        if (clipboard && strlen(clipboard) > 0) {
            if (state.hasSelection) {
                DeleteSelection(state);
            }
            InsertText(state, clipboard);
            result.handled = true;
            result.textChanged = true;
            result.newCursorPos = state.cursorPos + strlen(clipboard);
            result.selectionChanged = true;
            result.newSelectionStart = result.newCursorPos;
            result.newSelectionEnd = result.newCursorPos;
        }
        return result;
    }
    
    return result;
}

InputResult StandardMode::HandleNavigation(EditorState& state, ImGuiKey key, bool shift, bool ctrl) {
    InputResult result;
    result.handled = true;
    
    size_t newPos = state.cursorPos;
    
    switch (key) {
        case ImGuiKey_LeftArrow:
            if (ctrl) {
                newPos = TextOps::PrevWord(*state.buffer, state.cursorPos);
            } else if (state.hasSelection && !shift) {
                newPos = std::min(state.selectionStart, state.selectionEnd);
            } else if (state.cursorPos > 0) {
                --newPos;
            }
            break;
            
        case ImGuiKey_RightArrow:
            if (ctrl) {
                newPos = TextOps::NextWord(*state.buffer, state.cursorPos);
            } else if (state.hasSelection && !shift) {
                newPos = std::max(state.selectionStart, state.selectionEnd);
            } else if (state.cursorPos < state.buffer->Length()) {
                ++newPos;
            }
            break;
            
        case ImGuiKey_UpArrow: {
            auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
            if (line > 0) {
                std::string prevLine = state.buffer->Line(line - 1);
                size_t newCol = std::min(col, prevLine.length());
                newPos = state.buffer->LineColToPos(line - 1, newCol);
            }
            break;
        }
            
        case ImGuiKey_DownArrow: {
            auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
            if (line < state.buffer->LineCount() - 1) {
                std::string nextLine = state.buffer->Line(line + 1);
                size_t newCol = std::min(col, nextLine.length());
                newPos = state.buffer->LineColToPos(line + 1, newCol);
            }
            break;
        }
            
        case ImGuiKey_Home:
            if (ctrl) {
                newPos = 0;
            } else {
                newPos = TextOps::LineStart(*state.buffer, state.cursorPos);
            }
            break;
            
        case ImGuiKey_End:
            if (ctrl) {
                newPos = state.buffer->Length();
            } else {
                newPos = TextOps::LineEnd(*state.buffer, state.cursorPos);
            }
            break;
            
        case ImGuiKey_PageUp: {
            size_t visibleLines = state.lastVisibleLine - state.firstVisibleLine;
            auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
            size_t newLine = (line > visibleLines) ? line - visibleLines : 0;
            std::string targetLine = state.buffer->Line(newLine);
            size_t newCol = std::min(col, targetLine.length());
            newPos = state.buffer->LineColToPos(newLine, newCol);
            break;
        }
            
        case ImGuiKey_PageDown: {
            size_t visibleLines = state.lastVisibleLine - state.firstVisibleLine;
            auto [line, col] = state.buffer->PosToLineCol(state.cursorPos);
            size_t lineCount = state.buffer->LineCount();
            size_t newLine = std::min(line + visibleLines, lineCount - 1);
            std::string targetLine = state.buffer->Line(newLine);
            size_t newCol = std::min(col, targetLine.length());
            newPos = state.buffer->LineColToPos(newLine, newCol);
            break;
        }
            
        default:
            result.handled = false;
            return result;
    }
    
    result.cursorMoved = (newPos != state.cursorPos);
    result.newCursorPos = newPos;
    
    if (shift) {
        // Extend selection
        if (!state.hasSelection) {
            result.newSelectionStart = state.cursorPos;
        } else {
            result.newSelectionStart = state.selectionStart;
        }
        result.newSelectionEnd = newPos;
        result.selectionChanged = true;
    } else {
        // Clear selection
        result.newSelectionStart = newPos;
        result.newSelectionEnd = newPos;
        result.selectionChanged = state.hasSelection;
    }
    
    return result;
}

InputResult StandardMode::HandleEditing(EditorState& state, ImGuiKey key, bool ctrl) {
    InputResult result;
    result.handled = true;
    
    switch (key) {
        case ImGuiKey_Backspace:
            if (state.hasSelection) {
                size_t start = std::min(state.selectionStart, state.selectionEnd);
                DeleteSelection(state);
                result.textChanged = true;
                result.newCursorPos = start;
            } else if (state.cursorPos > 0) {
                size_t deleteLen = 1;
                if (ctrl) {
                    // Delete word
                    size_t wordStart = TextOps::PrevWord(*state.buffer, state.cursorPos);
                    deleteLen = state.cursorPos - wordStart;
                }
                size_t deletePos = state.cursorPos - deleteLen;
                if (state.undoTree) {
                    TextOps::Delete(*state.buffer, *state.undoTree, deletePos, deleteLen, state.cursorPos);
                } else {
                    state.buffer->Delete(deletePos, deleteLen);
                }
                result.textChanged = true;
                result.newCursorPos = deletePos;
            }
            result.selectionChanged = true;
            result.newSelectionStart = result.newCursorPos.value_or(state.cursorPos);
            result.newSelectionEnd = result.newCursorPos.value_or(state.cursorPos);
            break;
            
        case ImGuiKey_Delete:
            if (state.hasSelection) {
                size_t start = std::min(state.selectionStart, state.selectionEnd);
                DeleteSelection(state);
                result.textChanged = true;
                result.newCursorPos = start;
            } else if (state.cursorPos < state.buffer->Length()) {
                size_t deleteLen = 1;
                if (ctrl) {
                    // Delete word forward
                    size_t wordEnd = TextOps::NextWord(*state.buffer, state.cursorPos);
                    deleteLen = wordEnd - state.cursorPos;
                }
                if (state.undoTree) {
                    TextOps::Delete(*state.buffer, *state.undoTree, state.cursorPos, deleteLen, state.cursorPos);
                } else {
                    state.buffer->Delete(state.cursorPos, deleteLen);
                }
                result.textChanged = true;
                result.newCursorPos = state.cursorPos;
            }
            result.selectionChanged = true;
            result.newSelectionStart = result.newCursorPos.value_or(state.cursorPos);
            result.newSelectionEnd = result.newCursorPos.value_or(state.cursorPos);
            break;
            
        case ImGuiKey_Enter:
            if (state.hasSelection) {
                DeleteSelection(state);
            }
            InsertText(state, "\n");
            result.textChanged = true;
            result.newCursorPos = state.cursorPos + 1;
            result.selectionChanged = true;
            result.newSelectionStart = result.newCursorPos;
            result.newSelectionEnd = result.newCursorPos;
            break;
            
        case ImGuiKey_Tab:
            if (state.hasSelection) {
                // TODO: Indent selection
                DeleteSelection(state);
            }
            InsertText(state, "    ");  // 4 spaces
            result.textChanged = true;
            result.newCursorPos = state.cursorPos + 4;
            result.selectionChanged = true;
            result.newSelectionStart = result.newCursorPos;
            result.newSelectionEnd = result.newCursorPos;
            break;
            
        default:
            result.handled = false;
            break;
    }
    
    return result;
}

InputResult StandardMode::HandleTextInput(EditorState& state, const ImWchar* chars, int count) {
    InputResult result;
    
    if (count == 0) return result;
    
    // Delete selection first
    if (state.hasSelection) {
        size_t start = std::min(state.selectionStart, state.selectionEnd);
        DeleteSelection(state);
        state.cursorPos = start;
    }
    
    // Convert ImWchar to UTF-8
    std::string text;
    for (int i = 0; i < count; ++i) {
        ImWchar c = chars[i];
        if (c == '\r') continue;
        
        if (c < 0x80) {
            text += static_cast<char>(c);
        } else if (c < 0x800) {
            text += static_cast<char>(0xC0 | (c >> 6));
            text += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            text += static_cast<char>(0xE0 | (c >> 12));
            text += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            text += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    
    InsertText(state, text);
    
    result.handled = true;
    result.textChanged = true;
    result.newCursorPos = state.cursorPos + text.length();
    result.selectionChanged = true;
    result.newSelectionStart = result.newCursorPos;
    result.newSelectionEnd = result.newCursorPos;
    
    return result;
}

InputResult StandardMode::HandleMouse(EditorState& state, ImVec2 mousePos, bool clicked, bool dragging, bool released) {
    InputResult result;
    
    // Mouse handling is done in the editor widget for now
    // This can be expanded for more complex mouse operations
    
    return result;
}

void StandardMode::DeleteSelection(EditorState& state) {
    if (!state.hasSelection) return;
    
    size_t start = std::min(state.selectionStart, state.selectionEnd);
    size_t end = std::max(state.selectionStart, state.selectionEnd);
    size_t len = end - start;
    
    if (state.undoTree) {
        TextOps::Delete(*state.buffer, *state.undoTree, start, len, state.cursorPos);
    } else {
        state.buffer->Delete(start, len);
    }
    
    state.cursorPos = start;
    state.hasSelection = false;
}

void StandardMode::InsertText(EditorState& state, const std::string& text) {
    if (state.undoTree) {
        TextOps::Insert(*state.buffer, *state.undoTree, state.cursorPos, text, state.cursorPos);
    } else {
        state.buffer->Insert(state.cursorPos, text);
    }
}

} // namespace sol

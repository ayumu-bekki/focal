#include "edit/undo_stack.h"

#include <algorithm>

namespace focal {

void UndoStack::push(Settings before, Settings after) {
    commands_.resize(pos_);  // redo できる分は捨てる
    commands_.push_back({std::move(before), std::move(after)});
    pos_ = commands_.size();
}

std::optional<Settings> UndoStack::undo() {
    if (!can_undo()) return std::nullopt;
    return commands_[--pos_].before;
}

std::optional<Settings> UndoStack::redo() {
    if (!can_redo()) return std::nullopt;
    return commands_[pos_++].after;
}

UndoStack& UndoHistory::stack(int64_t photo_id) {
    auto it = std::find(order_.begin(), order_.end(), photo_id);
    if (it != order_.end()) {
        order_.splice(order_.begin(), order_, it);
    } else {
        order_.push_front(photo_id);
        while (order_.size() > kMaxPhotos) {
            stacks_.erase(order_.back());
            order_.pop_back();
        }
    }
    return stacks_[photo_id];
}

} // namespace focal

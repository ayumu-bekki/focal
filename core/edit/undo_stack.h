#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

#include "edit/settings.h"

namespace focal {

// 写真 1 枚分の Undo スタック（9.3 章、ADR-08）。コマンドは「変更前と変更後の Settings」。
class UndoStack {
public:
    void push(Settings before, Settings after);
    bool can_undo() const { return pos_ > 0; }
    bool can_redo() const { return pos_ < commands_.size(); }
    // 戻した後の Settings。戻せなければ nullopt
    std::optional<Settings> undo();
    std::optional<Settings> redo();
    size_t size() const { return commands_.size(); }

private:
    struct Command {
        Settings before, after;
    };
    std::vector<Command> commands_;
    size_t pos_ = 0;  // 次に redo するコマンドの位置
};

// 写真ごとの Undo スタックを直近 kMaxPhotos 枚分だけ持つ（古い写真から捨てる。セッション内のみで永続化しない）
class UndoHistory {
public:
    static constexpr size_t kMaxPhotos = 20;

    UndoStack& stack(int64_t photo_id);

private:
    std::list<int64_t> order_;  // 先頭が最近使った写真
    std::unordered_map<int64_t, UndoStack> stacks_;
};

} // namespace focal

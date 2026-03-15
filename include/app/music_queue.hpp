#pragma once

#include "app.h"
#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>

namespace vita_ma {

// Observer callback for queue changes
using QueueChangeCallback = std::function<void()>;

class MusicQueue {
public:
    static MusicQueue& instance();

    // Queue management
    void setItems(const std::vector<QueueItem>& items);
    void clear();
    void addItem(const QueueItem& item);
    void removeItem(int index);
    void moveItem(int fromIndex, int toIndex);

    // Playback control
    void setCurrentIndex(int index);
    int getCurrentIndex() const;
    QueueItem getCurrentItem() const;
    bool hasNext() const;
    bool hasPrevious() const;
    int next();
    int previous();

    // Queue state
    std::vector<QueueItem> getItems() const;
    int size() const;
    bool isEmpty() const;

    // Shuffle
    void setShuffle(bool enabled);
    bool isShuffleEnabled() const { return m_shuffleEnabled.load(); }

    // Repeat
    void setRepeatMode(RepeatMode mode);
    RepeatMode getRepeatMode() const { return m_repeatMode; }

    // Observer
    void setOnChange(QueueChangeCallback cb) { m_onChange = std::move(cb); }

    // Persistence
    void saveState();
    void loadState();

private:
    MusicQueue() = default;

    std::vector<QueueItem> m_items;
    std::vector<int> m_shuffleOrder;
    std::atomic<int> m_currentIndex{-1};
    std::atomic<bool> m_shuffleEnabled{false};
    RepeatMode m_repeatMode = RepeatMode::OFF;
    mutable std::mutex m_mutex;
    QueueChangeCallback m_onChange;

    void generateShuffleOrder();
    int getActualIndex(int logicalIndex) const;
    void notifyChange();
};

} // namespace vita_ma

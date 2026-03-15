#include "app/music_queue.hpp"
#include <borealis.hpp>
#include <algorithm>
#include <random>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <ctime>

namespace vita_ma {

MusicQueue& MusicQueue::instance() {
    static MusicQueue instance;
    return instance;
}

void MusicQueue::setItems(const std::vector<QueueItem>& items) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items = items;
    if (m_shuffleEnabled.load()) {
        generateShuffleOrder();
    }
    notifyChange();
}

void MusicQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.clear();
    m_shuffleOrder.clear();
    m_currentIndex.store(-1);
    notifyChange();
}

void MusicQueue::addItem(const QueueItem& item) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_items.push_back(item);
    if (m_shuffleEnabled.load()) {
        m_shuffleOrder.push_back(static_cast<int>(m_items.size()) - 1);
    }
    notifyChange();
}

void MusicQueue::removeItem(int index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index < 0 || index >= static_cast<int>(m_items.size())) return;

    m_items.erase(m_items.begin() + index);
    if (m_shuffleEnabled.load()) {
        generateShuffleOrder();
    }

    int cur = m_currentIndex.load();
    if (index < cur) {
        m_currentIndex.store(cur - 1);
    } else if (index == cur && cur >= static_cast<int>(m_items.size())) {
        m_currentIndex.store(static_cast<int>(m_items.size()) - 1);
    }
    notifyChange();
}

void MusicQueue::moveItem(int fromIndex, int toIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (fromIndex < 0 || fromIndex >= static_cast<int>(m_items.size())) return;
    if (toIndex < 0 || toIndex >= static_cast<int>(m_items.size())) return;

    QueueItem item = m_items[fromIndex];
    m_items.erase(m_items.begin() + fromIndex);
    m_items.insert(m_items.begin() + toIndex, item);

    int cur = m_currentIndex.load();
    if (cur == fromIndex) {
        m_currentIndex.store(toIndex);
    } else if (fromIndex < cur && toIndex >= cur) {
        m_currentIndex.store(cur - 1);
    } else if (fromIndex > cur && toIndex <= cur) {
        m_currentIndex.store(cur + 1);
    }
    notifyChange();
}

void MusicQueue::setCurrentIndex(int index) {
    m_currentIndex.store(index);
    notifyChange();
}

int MusicQueue::getCurrentIndex() const {
    return m_currentIndex.load();
}

QueueItem MusicQueue::getCurrentItem() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    int idx = getActualIndex(m_currentIndex.load());
    if (idx >= 0 && idx < static_cast<int>(m_items.size())) {
        return m_items[idx];
    }
    return QueueItem();
}

bool MusicQueue::hasNext() const {
    int cur = m_currentIndex.load();
    if (m_repeatMode == RepeatMode::ALL || m_repeatMode == RepeatMode::ONE) return true;
    return cur < static_cast<int>(m_items.size()) - 1;
}

bool MusicQueue::hasPrevious() const {
    int cur = m_currentIndex.load();
    if (m_repeatMode == RepeatMode::ALL) return true;
    return cur > 0;
}

int MusicQueue::next() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_items.empty()) return -1;

    int cur = m_currentIndex.load();

    if (m_repeatMode == RepeatMode::ONE) {
        // Stay on same track
        return getActualIndex(cur);
    }

    cur++;
    if (cur >= static_cast<int>(m_items.size())) {
        if (m_repeatMode == RepeatMode::ALL) {
            cur = 0;
            if (m_shuffleEnabled.load()) {
                generateShuffleOrder();
            }
        } else {
            return -1; // End of queue
        }
    }

    m_currentIndex.store(cur);
    return getActualIndex(cur);
}

int MusicQueue::previous() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_items.empty()) return -1;

    int cur = m_currentIndex.load();
    cur--;
    if (cur < 0) {
        if (m_repeatMode == RepeatMode::ALL) {
            cur = static_cast<int>(m_items.size()) - 1;
        } else {
            cur = 0;
        }
    }

    m_currentIndex.store(cur);
    return getActualIndex(cur);
}

std::vector<QueueItem> MusicQueue::getItems() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_items;
}

int MusicQueue::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_items.size());
}

bool MusicQueue::isEmpty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_items.empty();
}

void MusicQueue::setShuffle(bool enabled) {
    m_shuffleEnabled.store(enabled);
    if (enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        generateShuffleOrder();
    }
    notifyChange();
}

void MusicQueue::setRepeatMode(RepeatMode mode) {
    m_repeatMode = mode;
    notifyChange();
}

void MusicQueue::generateShuffleOrder() {
    // Fisher-Yates shuffle
    m_shuffleOrder.resize(m_items.size());
    for (size_t i = 0; i < m_shuffleOrder.size(); i++) {
        m_shuffleOrder[i] = static_cast<int>(i);
    }

    std::srand(static_cast<unsigned>(std::time(nullptr)));
    for (int i = static_cast<int>(m_shuffleOrder.size()) - 1; i > 0; i--) {
        int j = std::rand() % (i + 1);
        std::swap(m_shuffleOrder[i], m_shuffleOrder[j]);
    }

    // Move current track to front if playing
    int cur = m_currentIndex.load();
    if (cur >= 0 && cur < static_cast<int>(m_shuffleOrder.size())) {
        auto it = std::find(m_shuffleOrder.begin(), m_shuffleOrder.end(), cur);
        if (it != m_shuffleOrder.end()) {
            std::swap(*it, m_shuffleOrder[0]);
        }
    }
}

int MusicQueue::getActualIndex(int logicalIndex) const {
    if (logicalIndex < 0) return -1;
    if (m_shuffleEnabled.load() && !m_shuffleOrder.empty()) {
        if (logicalIndex < static_cast<int>(m_shuffleOrder.size())) {
            return m_shuffleOrder[logicalIndex];
        }
    }
    return logicalIndex;
}

void MusicQueue::notifyChange() {
    if (m_onChange) {
        brls::sync([this]() { m_onChange(); });
    }
}

void MusicQueue::saveState() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& app = App::instance();
    std::string path = app.getDataPath() + "/queue_state.txt";

    std::ofstream file(path);
    if (!file.is_open()) return;

    file << "index=" << m_currentIndex.load() << "\n";
    file << "shuffle=" << (m_shuffleEnabled.load() ? "1" : "0") << "\n";
    file << "repeat=" << static_cast<int>(m_repeatMode) << "\n";
    file << "count=" << m_items.size() << "\n";

    for (auto& item : m_items) {
        file << "item=" << item.queue_item_id << "|"
             << item.name << "|"
             << item.artist << "|"
             << item.uri << "|"
             << item.duration << "\n";
    }
    file.close();
}

void MusicQueue::loadState() {
    auto& app = App::instance();
    std::string path = app.getDataPath() + "/queue_state.txt";

    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "index") m_currentIndex.store(strtol(val.c_str(), nullptr, 10));
        else if (key == "shuffle") m_shuffleEnabled.store(val == "1");
        else if (key == "repeat") m_repeatMode = static_cast<RepeatMode>(strtol(val.c_str(), nullptr, 10));
        else if (key == "item") {
            QueueItem item;
            std::istringstream ss(val);
            std::string part;
            int idx = 0;
            while (std::getline(ss, part, '|')) {
                switch (idx) {
                    case 0: item.queue_item_id = part; break;
                    case 1: item.name = part; break;
                    case 2: item.artist = part; break;
                    case 3: item.uri = part; break;
                    case 4: item.duration = strtol(part.c_str(), nullptr, 10); break;
                }
                idx++;
            }
            m_items.push_back(item);
        }
    }
    file.close();
    brls::Logger::info("Queue: loaded {} items", m_items.size());
}

} // namespace vita_ma

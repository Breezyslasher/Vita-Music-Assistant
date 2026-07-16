/**
 * Vita Music Assistant - Music Tab implementation
 */

#include "view/music_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "app/music_queue.hpp"
#include "activity/player_activity.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"

namespace vita_ma {

MusicTab::MusicTab() {
    // Create alive flag for async callback safety
    m_alive = std::make_shared<bool>(true);

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // Scrollable main container
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

    m_mainContainer = new brls::Box();
    m_mainContainer->setAxis(brls::Axis::COLUMN);
    m_mainContainer->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_mainContainer->setAlignItems(brls::AlignItems::STRETCH);
    m_mainContainer->setPadding(20);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText("Music");
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(20);
    m_mainContainer->addView(m_titleLabel);

    // Sections row with horizontal scrolling
    m_sectionsScroll = new brls::HScrollingFrame();
    m_sectionsScroll->setHeight(50);
    m_sectionsScroll->setMarginBottom(20);

    m_sectionsBox = new brls::Box();
    m_sectionsBox->setAxis(brls::Axis::ROW);
    m_sectionsBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_sectionsBox->setAlignItems(brls::AlignItems::CENTER);

    m_sectionsScroll->setContentView(m_sectionsBox);
    m_mainContainer->addView(m_sectionsScroll);

    const auto& settings = Application::getInstance().getSettings();

    // Playlists row (hidden by default, shown when data loads)
    if (settings.showPlaylists) {
        m_playlistsRow = createHorizontalRow("Playlists");
        m_playlistsRow->setVisibility(brls::Visibility::GONE);
        m_mainContainer->addView(m_playlistsRow);
    }

    // Album categories (scrolling rows organized by type)
    m_albumCategoriesScroll = new brls::ScrollingFrame();
    m_albumCategoriesScroll->setGrow(1.0f);
    m_albumCategoriesScroll->setVisibility(brls::Visibility::GONE);

    m_albumCategoriesBox = new brls::Box();
    m_albumCategoriesBox->setAxis(brls::Axis::COLUMN);
    m_albumCategoriesBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_albumCategoriesBox->setAlignItems(brls::AlignItems::STRETCH);

    m_albumCategoriesScroll->setContentView(m_albumCategoriesBox);
    m_mainContainer->addView(m_albumCategoriesScroll);

    // "Artists" label
    auto* artistsLabel = new brls::Label();
    artistsLabel->setText("Artists");
    artistsLabel->setFontSize(22);
    artistsLabel->setMarginTop(10);
    artistsLabel->setMarginBottom(10);
    m_mainContainer->addView(artistsLabel);

    // Content grid
    m_contentGrid = new RecyclingGrid();
    m_contentGrid->setGrow(1.0f);
    m_contentGrid->setHeight(400);
    m_contentGrid->setOnItemSelected([this](const MusicItem& item) {
        onItemSelected(item);
    });
    m_contentGrid->setOnItemStartAction([this](const MusicItem& item) {
        showAlbumContextMenu(item);
    });
    m_mainContainer->addView(m_contentGrid);

    m_scrollView->setContentView(m_mainContainer);
    this->addView(m_scrollView);

    // Load sections immediately
    brls::Logger::debug("MusicTab: Loading sections...");
    loadSections();
}

MusicTab::~MusicTab() {
    // Mark as no longer alive to prevent async callbacks from updating destroyed UI
    if (m_alive) {
        *m_alive = false;
    }
    brls::Logger::debug("MusicTab: Destroyed");
}

brls::Box* MusicTab::createHorizontalRow(const std::string& title) {
    auto* rowBox = new brls::Box();
    rowBox->setAxis(brls::Axis::COLUMN);
    rowBox->setMarginBottom(15);

    auto* headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::ROW);
    headerBox->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setMarginBottom(10);

    auto* titleLabel = new brls::Label();
    titleLabel->setText(title);
    titleLabel->setFontSize(22);
    headerBox->addView(titleLabel);

    // Add "New Playlist" button for playlists row
    if (title == "Playlists") {
        auto* newBtn = new brls::Button();
        newBtn->setText("+ New");
        newBtn->setHeight(30);
        newBtn->setCornerRadius(16);
        newBtn->setHighlightCornerRadius(16);
        newBtn->setPadding(4, 14, 4, 14);
        newBtn->setBackgroundColor(nvgRGBA(50, 130, 80, 200));
        newBtn->registerClickAction([this](brls::View* view) {
            showCreatePlaylistDialog();
            return true;
        });
        headerBox->addView(newBtn);
    }

    rowBox->addView(headerBox);

    auto* scrollFrame = new brls::HScrollingFrame();
    scrollFrame->setHeight(120);

    auto* container = new brls::Box();
    container->setAxis(brls::Axis::ROW);
    container->setJustifyContent(brls::JustifyContent::FLEX_START);
    container->setAlignItems(brls::AlignItems::CENTER);

    scrollFrame->setContentView(container);
    rowBox->addView(scrollFrame);

    // Store container reference based on title
    if (title == "Playlists") {
        m_playlistsContainer = container;
    }

    return rowBox;
}

void MusicTab::styleButton(brls::Button* btn, bool active) {
    btn->setCornerRadius(16);
    btn->setHighlightCornerRadius(16);
    btn->setPadding(6, 16, 6, 16);
    if (active) {
        btn->setBackgroundColor(nvgRGBA(70, 90, 210, 220));
        btn->setBorderColor(nvgRGBA(120, 160, 255, 200));
        btn->setBorderThickness(1.5f);
    } else {
        btn->setBackgroundColor(nvgRGBA(60, 60, 70, 180));
        btn->setBorderColor(nvgRGBA(0, 0, 0, 0));
        btn->setBorderThickness(0);
    }
}

void MusicTab::updateSectionButtonStyles() {
    if (!m_sectionsBox) return;
    for (auto* child : m_sectionsBox->getChildren()) {
        auto* btn = dynamic_cast<brls::Button*>(child);
        if (btn) {
            styleButton(btn, btn == m_activeSectionBtn);
        }
    }
}

void MusicTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    ImageLoader::cancelAll();
    ImageLoader::clearCache();
}

void MusicTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    if (!m_loaded) {
        loadSections();
    }
}

void MusicTab::loadSections() {
    brls::Logger::debug("MusicTab::loadSections - TODO: implement with MA async API");
    // TODO: Use MAClient async API to fetch library data
    // MAClient::instance().getLibraryArtists([this](bool success, const Json& result) { ... });

    // Load playlists (audio playlists)
    const auto& settings = Application::getInstance().getSettings();
    if (settings.showPlaylists) {
        loadPlaylists();
    }

    m_loaded = true;
}

void MusicTab::loadContent(const std::string& sectionKey) {
    brls::Logger::debug("MusicTab::loadContent - section: {} - TODO: implement with MA async API", sectionKey);
    // TODO: Use MAClient async API to fetch library content
    // MAClient::instance().getLibraryArtists([this](bool success, const Json& result) { ... });

    // Also load albums organized by type
    loadAlbumsByType(sectionKey);
}

brls::Box* MusicTab::createAlbumScrollRow(const std::string& title, const std::vector<MusicItem>& items) {
    if (items.empty()) return nullptr;

    auto* rowBox = new brls::Box();
    rowBox->setAxis(brls::Axis::COLUMN);
    rowBox->setMarginBottom(15);

    auto* titleLabel = new brls::Label();
    titleLabel->setText(title + " (" + std::to_string(items.size()) + ")");
    titleLabel->setFontSize(20);
    titleLabel->setMarginBottom(10);
    rowBox->addView(titleLabel);

    auto* scrollFrame = new brls::HScrollingFrame();
    scrollFrame->setHeight(150);

    auto* container = new brls::Box();
    container->setAxis(brls::Axis::ROW);
    container->setJustifyContent(brls::JustifyContent::FLEX_START);
    container->setAlignItems(brls::AlignItems::CENTER);

    for (const auto& item : items) {
        auto* cell = new MediaItemCell();
        cell->setItem(item);
        cell->setMarginRight(10);

        MusicItem capturedItem = item;
        cell->registerClickAction([this, capturedItem](brls::View* view) {
            onItemSelected(capturedItem);
            return true;
        });
        cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

        cell->registerAction("Options", brls::ControllerButton::BUTTON_START, [this, capturedItem](brls::View* view) {
            showAlbumContextMenu(capturedItem);
            return true;
        });

        container->addView(cell);
    }

    scrollFrame->setContentView(container);
    rowBox->addView(scrollFrame);

    return rowBox;
}

void MusicTab::loadAlbumsByType(const std::string& sectionKey) {
    brls::Logger::debug("MusicTab::loadAlbumsByType - section: {} - TODO: implement with MA async API", sectionKey);
    // TODO: Use MAClient async API to fetch albums
    // MAClient::instance().getLibraryAlbums([this](bool success, const Json& result) { ... });
}

void MusicTab::loadPlaylists() {
    brls::Logger::debug("MusicTab::loadPlaylists - TODO: implement with MA async API");
    // TODO: Use MAClient async API to fetch playlists
    // MAClient::instance().getLibraryPlaylists([this](bool success, const Json& result) { ... });
}

void MusicTab::refreshPlaylists() {
    loadPlaylists();
}

void MusicTab::onItemSelected(const MusicItem& item) {
    // For tracks in a playlist, play the whole playlist with queue
    if (item.mediaType == MediaType::TRACK) {
        if (m_viewingPlaylist && !m_currentPlaylistId.empty()) {
            // Find the index of this track in the current items
            int startIndex = 0;
            for (size_t i = 0; i < m_items.size(); i++) {
                if (m_items[i].itemId == item.itemId) {
                    startIndex = (int)i;
                    break;
                }
            }
            playPlaylistWithQueue(m_currentPlaylistId, startIndex);
        } else {
            // Single track playback
            Application::getInstance().pushPlayerActivity(item.itemId);
        }
        return;
    }

    // Show media detail view for artists and albums
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

void MusicTab::onPlaylistSelected(const MusicItem& playlist) {
    brls::Logger::debug("MusicTab: Selected playlist: {}", playlist.name);

    std::weak_ptr<bool> aliveWeak = m_alive;
    MusicItem capturedPlaylist = playlist;

    MAClient::instance().getPlaylistTracks(playlist.itemId, [this, aliveWeak, capturedPlaylist](bool success, const Json& result) {
        auto alive = aliveWeak.lock();
        if (!alive || !*alive) return;

        if (!success) {
            brls::sync([]() {
                brls::Application::notify("Failed to load playlist tracks");
            });
            return;
        }

        std::vector<MusicItem> tracks;
        if (result.type() == Json::ARRAY) {
            for (size_t i = 0; i < result.size(); i++) {
                const Json& obj = result[i];
                MusicItem mi;
                mi.itemId     = obj.has("item_id")      ? obj["item_id"].str()     : "";
                mi.name       = obj.has("name")          ? obj["name"].str()        : "";
                mi.uri        = obj.has("uri")           ? obj["uri"].str()         : "";
                // Extract image URL: try image object, then metadata.images array
                if (obj.has("image") && obj["image"].type() == Json::OBJECT && obj["image"].has("path")) {
                    mi.imageUrl = MAClient::imageRefFromJson(obj["image"]);
                    if (obj["image"].has("provider")) mi.imageProvider = obj["image"]["provider"].str();
                } else if (obj.has("image") && obj["image"].type() == Json::STRING) {
                    mi.imageUrl = obj["image"].str();
                } else if (obj.has("metadata") && obj["metadata"].type() == Json::OBJECT) {
                    const Json& meta = obj["metadata"];
                    if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
                        const Json& img = meta["images"][static_cast<size_t>(0)];
                        mi.imageUrl = MAClient::imageRefFromJson(img);
                        if (img.has("provider")) mi.imageProvider = img["provider"].str();
                    }
                }
                mi.mediaType  = MediaType::TRACK;
                mi.duration   = obj.has("duration")      ? obj["duration"].intVal() : 0;
                mi.provider   = obj.has("provider")      ? obj["provider"].str()    : "";
                if (obj.has("artist_name"))  mi.artistName  = obj["artist_name"].str();
                if (obj.has("album_name"))   mi.albumName   = obj["album_name"].str();
                if (obj.has("track_number")) mi.trackNumber = obj["track_number"].intVal();
                tracks.push_back(mi);
            }
        }

        brls::sync([this, aliveWeak, tracks, capturedPlaylist]() {
            auto alive2 = aliveWeak.lock();
            if (!alive2 || !*alive2) return;

            brls::Logger::info("MusicTab: Loaded {} tracks from playlist '{}'",
                               tracks.size(), capturedPlaylist.name);
            m_items = tracks;
            m_currentPlaylistId = capturedPlaylist.itemId;
            m_viewingPlaylist = true;

            if (m_contentGrid) {
                m_contentGrid->setDataSource(tracks);
            }
        });
    }, 0, capturedPlaylist.provider);
}

void MusicTab::playPlaylistWithQueue(const std::string& playlistId, int startIndex) {
    brls::Logger::debug("MusicTab::playPlaylistWithQueue for playlist {}", playlistId);

    // If we already have the tracks loaded for this playlist, use them directly
    if (m_viewingPlaylist && m_currentPlaylistId == playlistId && !m_items.empty()) {
        auto* playerActivity = PlayerActivity::createWithQueue(m_items, startIndex);
        brls::Application::pushActivity(playerActivity);
        return;
    }

    // Otherwise fetch tracks first
    MAClient::instance().getPlaylistTracks(playlistId, [startIndex](bool success, const Json& result) {
        if (!success) {
            brls::sync([]() {
                brls::Application::notify("Failed to load playlist");
            });
            return;
        }

        std::vector<MusicItem> tracks;
        if (result.type() == Json::ARRAY) {
            for (size_t i = 0; i < result.size(); i++) {
                const Json& obj = result[i];
                MusicItem mi;
                mi.itemId     = obj.has("item_id")      ? obj["item_id"].str()     : "";
                mi.name       = obj.has("name")          ? obj["name"].str()        : "";
                mi.uri        = obj.has("uri")           ? obj["uri"].str()         : "";
                // Extract image URL: try image object, then metadata.images array
                if (obj.has("image") && obj["image"].type() == Json::OBJECT && obj["image"].has("path")) {
                    mi.imageUrl = MAClient::imageRefFromJson(obj["image"]);
                    if (obj["image"].has("provider")) mi.imageProvider = obj["image"]["provider"].str();
                } else if (obj.has("image") && obj["image"].type() == Json::STRING) {
                    mi.imageUrl = obj["image"].str();
                } else if (obj.has("metadata") && obj["metadata"].type() == Json::OBJECT) {
                    const Json& meta = obj["metadata"];
                    if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
                        const Json& img = meta["images"][static_cast<size_t>(0)];
                        mi.imageUrl = MAClient::imageRefFromJson(img);
                        if (img.has("provider")) mi.imageProvider = img["provider"].str();
                    }
                }
                mi.mediaType  = MediaType::TRACK;
                mi.duration   = obj.has("duration")      ? obj["duration"].intVal() : 0;
                mi.provider   = obj.has("provider")      ? obj["provider"].str()    : "";
                if (obj.has("artist_name"))  mi.artistName  = obj["artist_name"].str();
                if (obj.has("album_name"))   mi.albumName   = obj["album_name"].str();
                if (obj.has("track_number")) mi.trackNumber = obj["track_number"].intVal();
                tracks.push_back(mi);
            }
        }

        brls::sync([tracks, startIndex]() {
            if (!tracks.empty()) {
                auto* playerActivity = PlayerActivity::createWithQueue(tracks, startIndex);
                brls::Application::pushActivity(playerActivity);
            } else {
                brls::Application::notify("Playlist is empty");
            }
        });
    });
}

void MusicTab::showCreatePlaylistDialog() {
    // Use IME (on-screen keyboard) to get playlist name from user
    std::weak_ptr<bool> aliveWeak = m_alive;

    brls::Application::getImeManager()->openForText([this, aliveWeak](std::string playlistName) {
        if (playlistName.empty()) return;

        MAClient::instance().createPlaylist(playlistName, [this, aliveWeak](bool success, const Json& result) {
            brls::sync([this, aliveWeak, success]() {
                auto alive = aliveWeak.lock();
                if (!alive || !*alive) return;
                if (success) {
                    brls::Application::notify("Playlist created");
                    refreshPlaylists();
                } else {
                    brls::Application::notify("Failed to create playlist");
                }
            });
        });
    }, "New Playlist", "Enter playlist name", 128, "");
}

void MusicTab::showAlbumContextMenu(const MusicItem& album) {
    auto* dialog = new brls::Dialog(album.name);

    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    auto addDialogButton = [&optionsBox](const std::string& text, std::function<bool(brls::View*)> action) {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setHeight(44);
        btn->setMarginBottom(10);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        optionsBox->addView(btn);
    };

    MusicItem capturedAlbum = album;

    addDialogButton("Play Now (Clear Queue)", [capturedAlbum, dialog](brls::View*) {
        dialog->dismiss();
        brls::Logger::debug("MusicTab: Play Now - TODO: implement with MA async API");
        // TODO: Use MAClient async API to fetch album tracks and play
        // MAClient::instance().getAlbumTracks(capturedAlbum.itemId, [](bool success, const Json& result) { ... });
        return true;
    });

    addDialogButton("Play Next", [capturedAlbum, dialog](brls::View*) {
        dialog->dismiss();
        brls::Logger::debug("MusicTab: Play Next - TODO: implement with MA async API");
        // TODO: Use MAClient async API to fetch album tracks and queue next
        // MAClient::instance().getAlbumTracks(capturedAlbum.itemId, [](bool success, const Json& result) { ... });
        return true;
    });

    addDialogButton("Add to Bottom of Queue", [capturedAlbum, dialog](brls::View*) {
        dialog->dismiss();
        brls::Logger::debug("MusicTab: Add to Queue - TODO: implement with MA async API");
        // TODO: Use MAClient async API to fetch album tracks and add to queue
        // MAClient::instance().getAlbumTracks(capturedAlbum.itemId, [](bool success, const Json& result) { ... });
        return true;
    });

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });

    dialog->addView(optionsBox);
    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    brls::Application::pushActivity(new brls::Activity(dialog));
}

void MusicTab::showPlaylistOptionsDialog(const MusicItem& playlist) {
    auto* dialog = new brls::Dialog(playlist.name);

    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    auto* trackCount = new brls::Label();
    trackCount->setText("Tracks: " + std::to_string(playlist.itemCount));
    trackCount->setMarginBottom(10);
    optionsBox->addView(trackCount);

    if (!playlist.isEditable) {
        auto* smartLabel = new brls::Label();
        smartLabel->setText("(Smart Playlist - cannot be edited)");
        smartLabel->setFontSize(14);
        smartLabel->setTextColor(nvgRGBA(150, 150, 150, 255));
        smartLabel->setMarginBottom(10);
        optionsBox->addView(smartLabel);
    }

    auto addDialogButton = [&optionsBox](const std::string& text, std::function<bool(brls::View*)> action) {
        auto* btn = new brls::Button();
        btn->setText(text);
        btn->setHeight(44);
        btn->setMarginBottom(10);
        btn->registerClickAction(action);
        btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
        optionsBox->addView(btn);
    };

    MusicItem capturedPlaylist = playlist;

    addDialogButton("Play All", [this, capturedPlaylist, dialog](brls::View*) {
        dialog->dismiss();
        playPlaylistWithQueue(capturedPlaylist.itemId, 0);
        return true;
    });

    addDialogButton("Add to Queue", [capturedPlaylist, dialog](brls::View*) {
        dialog->dismiss();

        MAClient::instance().getPlaylistTracks(capturedPlaylist.itemId, [](bool success, const Json& result) {
            if (!success) {
                brls::sync([]() {
                    brls::Application::notify("Failed to load playlist tracks");
                });
                return;
            }

            std::vector<MusicItem> tracks;
            if (result.type() == Json::ARRAY) {
                for (size_t i = 0; i < result.size(); i++) {
                    const Json& obj = result[i];
                    MusicItem mi;
                    mi.itemId     = obj.has("item_id")      ? obj["item_id"].str()     : "";
                    mi.name       = obj.has("name")          ? obj["name"].str()        : "";
                    mi.uri        = obj.has("uri")           ? obj["uri"].str()         : "";
                    // MA returns image as an object with path field
                    if (obj.has("image") && obj["image"].type() == Json::OBJECT && obj["image"].has("path")) {
                        mi.imageUrl = MAClient::imageRefFromJson(obj["image"]);
                        if (obj["image"].has("provider")) mi.imageProvider = obj["image"]["provider"].str();
                    } else if (obj.has("image") && obj["image"].type() == Json::STRING) {
                        mi.imageUrl = obj["image"].str();
                    } else if (obj.has("image_url")) {
                        mi.imageUrl = obj["image_url"].str();
                    }
                    mi.mediaType  = MediaType::TRACK;
                    mi.duration   = obj.has("duration")      ? obj["duration"].intVal() : 0;
                    mi.provider   = obj.has("provider")      ? obj["provider"].str()    : "";
                    if (obj.has("artist_name"))  mi.artistName  = obj["artist_name"].str();
                    if (obj.has("album_name"))   mi.albumName   = obj["album_name"].str();
                    tracks.push_back(mi);
                }
            }

            brls::sync([tracks]() {
                if (!tracks.empty()) {
                    MusicQueue::getInstance().addTracks(tracks);
                    brls::Application::notify("Added " + std::to_string(tracks.size()) + " tracks to queue");
                }
            });
        }, 0, capturedPlaylist.provider);
        return true;
    });

    // Delete button (only for editable playlists)
    if (capturedPlaylist.isEditable) {
        addDialogButton("Delete", [this, capturedPlaylist, dialog](brls::View*) {
            dialog->dismiss();
            brls::Dialog* confirmDialog = new brls::Dialog("Delete this playlist?");
            confirmDialog->addButton("Yes, Delete", [this, capturedPlaylist]() {
                std::weak_ptr<bool> aliveWeak = m_alive;
                MAClient::instance().deletePlaylist(capturedPlaylist.itemId, [this, aliveWeak, capturedPlaylist](bool success, const Json& result) {
                    brls::sync([this, aliveWeak, success, capturedPlaylist]() {
                        auto alive = aliveWeak.lock();
                        if (!alive || !*alive) return;
                        if (success) {
                            brls::Application::notify("Playlist deleted");
                            refreshPlaylists();
                        } else {
                            brls::Application::notify("Failed to delete playlist");
                        }
                    });
                });
            });
            confirmDialog->addButton("Cancel", []() {});
            confirmDialog->open();
            return true;
        });
    }

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });

    dialog->addView(optionsBox);
    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    brls::Application::pushActivity(new brls::Activity(dialog));
}

} // namespace vita_ma

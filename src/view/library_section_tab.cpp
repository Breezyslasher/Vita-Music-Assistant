/**
 * Vita Music Assistant - Library Section Tab implementation
 * Uses Music Assistant types (MusicItem, MediaType) throughout.
 */

#include "view/library_section_tab.hpp"
#include "view/media_item_cell.hpp"
#include "view/media_detail_view.hpp"
#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"
#include "app/music_queue.hpp"

namespace vita_ma {

LibrarySectionTab::LibrarySectionTab(const std::string& sectionKey, const std::string& title, const std::string& sectionType)
    : m_sectionKey(sectionKey), m_title(title), m_sectionType(sectionType) {

    // Create alive flag for async callback safety
    m_alive = std::make_shared<bool>(true);

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText(title);
    m_titleLabel->setFontSize(28);
    m_titleLabel->setMarginBottom(15);
    this->addView(m_titleLabel);

    // View mode selector (All / Playlists)
    m_viewModeBox = new brls::Box();
    m_viewModeBox->setAxis(brls::Axis::ROW);
    m_viewModeBox->setJustifyContent(brls::JustifyContent::FLEX_START);
    m_viewModeBox->setAlignItems(brls::AlignItems::CENTER);
    m_viewModeBox->setMarginBottom(15);

    // All Items button
    m_allBtn = new brls::Button();
    m_allBtn->setText("All");
    m_allBtn->setMarginRight(10);
    styleButton(m_allBtn, true);  // Active by default
    m_allBtn->registerClickAction([this](brls::View* view) {
        showAllItems();
        return true;
    });
    m_viewModeBox->addView(m_allBtn);

    const auto& settings = Application::getInstance().getSettings();

    // Playlists button (only for music/artist sections)
    if (settings.showPlaylists && sectionType == "artist") {
        m_playlistsBtn = new brls::Button();
        m_playlistsBtn->setText("Playlists");
        m_playlistsBtn->setMarginRight(10);
        styleButton(m_playlistsBtn, false);
        m_playlistsBtn->registerClickAction([this](brls::View* view) {
            showPlaylists();
            return true;
        });
        m_viewModeBox->addView(m_playlistsBtn);
    }

    // Back button (hidden by default, shown in filtered view)
    m_backBtn = new brls::Button();
    m_backBtn->setText("< Back");
    m_backBtn->setVisibility(brls::Visibility::GONE);
    styleButton(m_backBtn, false);
    m_backBtn->setBackgroundColor(nvgRGBA(80, 60, 50, 200));
    m_backBtn->registerClickAction([this](brls::View* view) {
        showAllItems();
        return true;
    });
    m_viewModeBox->addView(m_backBtn);

    this->addView(m_viewModeBox);

    // Content grid
    m_contentGrid = new RecyclingGrid();
    m_contentGrid->setGrow(1.0f);
    m_contentGrid->setOnItemSelected([this](const MusicItem& item) {
        onItemSelected(item);
    });
    // Infinite scroll: load next page when user scrolls to bottom
    m_contentGrid->setOnLoadMore([this]() {
        loadNextPage();
    });
    m_contentGrid->setOnItemStartAction([this](const MusicItem& item) {
        if (item.mediaType == MediaType::PLAYLIST) {
            showPlaylistContextMenu(item);
        }
    });
    this->addView(m_contentGrid);

    // Track list view (for playlist contents - hidden by default)
    m_trackListScroll = new brls::ScrollingFrame();
    m_trackListScroll->setGrow(1.0f);
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_trackListBox = new brls::Box();
    m_trackListBox->setAxis(brls::Axis::COLUMN);
    m_trackListBox->setPadding(5);
    m_trackListScroll->setContentView(m_trackListBox);
    this->addView(m_trackListScroll);

    // Load content immediately
    brls::Logger::debug("LibrarySectionTab: Created for section {} ({}) type={}", m_sectionKey, m_title, m_sectionType);
    loadContent();
}

LibrarySectionTab::~LibrarySectionTab() {
    // Mark as no longer alive to prevent async callbacks from updating destroyed UI
    if (m_alive) {
        *m_alive = false;
    }
    brls::Logger::debug("LibrarySectionTab: Destroyed for section {}", m_sectionKey);
}

void LibrarySectionTab::willDisappear(bool resetState) {
    brls::Box::willDisappear(resetState);
    if (m_alive) *m_alive = false;
    ImageLoader::cancelAll();
    // Free image cache when leaving tab to reclaim memory
    ImageLoader::clearCache();
}

void LibrarySectionTab::onFocusGained() {
    brls::Box::onFocusGained();
    m_alive = std::make_shared<bool>(true);

    if (!m_loaded) {
        loadContent();
    }
}

void LibrarySectionTab::loadContent() {
    brls::Logger::debug("LibrarySectionTab::loadContent - section: {} type: {} (async)", m_sectionKey, m_sectionType);

    m_pageOffset = 0;
    m_totalItemCount = 0;
    m_items.clear();
    m_loaded = true;

    auto alive = m_alive;

    // TODO: Implement async loading via MAClient based on section type
    // Use MAClient::getLibraryArtists, getLibraryAlbums, getLibraryTracks, etc.
    // Example for artists:
    //   MAClient::instance().getLibraryArtists(
    //       [this, alive](bool success, const Json& result) {
    //           if (!alive || !*alive) return;
    //           if (!success) return;
    //           // Parse result into m_items, update m_totalItemCount
    //           brls::sync([this, alive]() {
    //               if (!alive || !*alive) return;
    //               m_contentGrid->setDataSource(m_items);
    //               m_contentGrid->setHasMore(m_pageOffset < (size_t)m_totalItemCount);
    //           });
    //       }, "", PAGE_SIZE, 0);
    brls::Logger::warning("LibrarySectionTab::loadContent - TODO: Implement async loading via MAClient");

    // Preload playlists if applicable
    const auto& settings = Application::getInstance().getSettings();
    if (settings.showPlaylists && m_sectionType == "artist") {
        loadPlaylists();
    }
}

void LibrarySectionTab::loadNextPage() {
    // TODO: Implement pagination via MAClient async API
    // Use the same MAClient call as loadContent() but with m_pageOffset
    // Example:
    //   auto alive = m_alive;
    //   MAClient::instance().getLibraryArtists(
    //       [this, alive](bool success, const Json& result) {
    //           if (!alive || !*alive) return;
    //           if (!success) return;
    //           // Append parsed items to m_items, advance m_pageOffset
    //           brls::sync([this, alive]() {
    //               if (!alive || !*alive) return;
    //               m_contentGrid->setDataSource(m_items);
    //               m_contentGrid->setHasMore(m_pageOffset < (size_t)m_totalItemCount);
    //           });
    //       }, "", PAGE_SIZE, m_pageOffset);
    brls::Logger::warning("LibrarySectionTab::loadNextPage - TODO: Implement async pagination via MAClient");
    m_contentGrid->setHasMore(false);
}

void LibrarySectionTab::showAllItems() {
    m_viewMode = LibraryViewMode::ALL_ITEMS;
    m_titleLabel->setText(m_title);
    // Free playlist tracks memory when leaving playlist view
    m_playlistTracks.clear();
    m_playlistTracks.shrink_to_fit();
    m_currentPlaylistId.clear();
    m_trackListRendered = 0;
    m_trackListLoadMoreBtn = nullptr;
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_contentGrid->setVisibility(brls::Visibility::VISIBLE);
    m_contentGrid->setDataSource(m_items);
    m_contentGrid->setHasMore(m_pageOffset < (size_t)m_totalItemCount);
    updateViewModeButtons();
}

void LibrarySectionTab::styleButton(brls::Button* btn, bool active) {
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

void LibrarySectionTab::updateViewModeButtons() {
    // Show/hide back button
    bool inFilteredView = (m_viewMode == LibraryViewMode::FILTERED);
    m_backBtn->setVisibility(inFilteredView ? brls::Visibility::VISIBLE : brls::Visibility::GONE);

    // Show/hide mode buttons
    bool showModeButtons = (m_viewMode != LibraryViewMode::FILTERED);
    m_allBtn->setVisibility(showModeButtons ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    if (m_playlistsBtn) {
        m_playlistsBtn->setVisibility(showModeButtons && !m_playlists.empty() ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }

    // Update active styling on mode buttons
    if (showModeButtons) {
        styleButton(m_allBtn, m_viewMode == LibraryViewMode::ALL_ITEMS);
        if (m_playlistsBtn) styleButton(m_playlistsBtn, m_viewMode == LibraryViewMode::PLAYLISTS);
    }
}

void LibrarySectionTab::onItemSelected(const MusicItem& item) {
    // Handle selection based on current view mode
    if (m_viewMode == LibraryViewMode::PLAYLISTS) {
        // Selected a playlist from the grid - show its contents
        onPlaylistSelected(item);
        return;
    }

    // For tracks, play directly
    if (item.mediaType == MediaType::TRACK) {
        Application::getInstance().pushPlayerActivity(item.itemId);
        return;
    }

    // Show media detail view for artists, albums, etc.
    auto* detailView = new MediaDetailView(item);
    brls::Application::pushActivity(new brls::Activity(detailView));
}

void LibrarySectionTab::loadPlaylists() {
    auto alive = m_alive;

    // TODO: Implement via MAClient::getLibraryPlaylists
    // MAClient::instance().getLibraryPlaylists(
    //     [this, alive](bool success, const Json& result) {
    //         if (!alive || !*alive) return;
    //         if (!success) {
    //             brls::Logger::error("LibrarySectionTab: Failed to load playlists");
    //             return;
    //         }
    //         // Parse result JSON into m_playlists (vector<MusicItem> with mediaType=PLAYLIST)
    //         // Each playlist MusicItem should have: itemId, name, imageUrl, itemCount, isEditable
    //         m_playlistsLoaded = true;
    //         brls::sync([this, alive]() {
    //             if (!alive || !*alive) return;
    //             updateViewModeButtons();
    //         });
    //     });
    brls::Logger::warning("LibrarySectionTab::loadPlaylists - TODO: Implement via MAClient::getLibraryPlaylists");
    m_playlistsLoaded = true;
}

void LibrarySectionTab::showPlaylists() {
    if (!m_playlistsLoaded) {
        brls::Application::notify("Loading playlists...");
        return;
    }

    if (m_playlists.empty()) {
        brls::Application::notify("No playlists available");
        return;
    }

    m_viewMode = LibraryViewMode::PLAYLISTS;
    m_titleLabel->setText(m_title + " - Playlists");
    m_trackListScroll->setVisibility(brls::Visibility::GONE);
    m_contentGrid->setVisibility(brls::Visibility::VISIBLE);
    m_contentGrid->setDataSource(m_playlists);
    m_contentGrid->setHasMore(false);
    updateViewModeButtons();
}

void LibrarySectionTab::onPlaylistSelected(const MusicItem& playlist) {
    brls::Logger::debug("LibrarySectionTab: Selected playlist '{}' (id={})", playlist.name, playlist.itemId);

    auto alive = m_alive;
    std::string playlistId = playlist.itemId;
    std::string playlistName = playlist.name;

    std::string provider = playlist.provider;

    MAClient::instance().getPlaylistTracks(playlistId, [this, alive, playlistId, playlistName](bool success, const Json& result) {
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
                mi.imageUrl   = obj.has("image")         ? obj["image"].str()       :
                                obj.has("image_url")     ? obj["image_url"].str()   : "";
                mi.mediaType  = MediaType::TRACK;
                mi.duration   = obj.has("duration")      ? obj["duration"].intVal() : 0;
                mi.provider   = obj.has("provider")      ? obj["provider"].str()    : "";
                if (obj.has("artist_name"))  mi.artistName  = obj["artist_name"].str();
                if (obj.has("album_name"))   mi.albumName   = obj["album_name"].str();
                if (obj.has("track_number")) mi.trackNumber = obj["track_number"].intVal();
                tracks.push_back(mi);
            }
        }

        brls::sync([this, alive, tracks = std::move(tracks), playlistName, playlistId]() mutable {
            if (!alive || !*alive) return;
            showPlaylistTrackList(std::move(tracks), playlistName, playlistId);
        });
    }, 0, provider);
}

void LibrarySectionTab::showPlaylistTrackList(std::vector<MusicItem>&& tracks,
                                                const std::string& playlistTitle,
                                                const std::string& playlistId) {
    m_viewMode = LibraryViewMode::FILTERED;
    m_titleLabel->setText(m_title + " - " + playlistTitle + " (" + std::to_string(tracks.size()) + " tracks)");
    updateViewModeButtons();

    // Store tracks as member to avoid per-row copies (was causing OOM with large playlists)
    m_playlistTracks = std::move(tracks);
    m_currentPlaylistId = playlistId;
    m_trackListRendered = 0;
    m_trackListLoadMoreBtn = nullptr;

    // Hide grid, show track list
    m_contentGrid->setVisibility(brls::Visibility::GONE);
    m_trackListScroll->setVisibility(brls::Visibility::VISIBLE);
    m_trackListBox->clearViews();

    // Render first page of tracks
    appendTrackListPage();
}

void LibrarySectionTab::appendTrackListPage() {
    // Remove existing "Load More" button
    if (m_trackListLoadMoreBtn) {
        m_trackListBox->removeView(m_trackListLoadMoreBtn);
        m_trackListLoadMoreBtn = nullptr;
    }

    size_t end = std::min(m_trackListRendered + TRACK_LIST_PAGE_SIZE, m_playlistTracks.size());

    for (size_t i = m_trackListRendered; i < end; i++) {
        const auto& track = m_playlistTracks[i];

        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(48);
        row->setPadding(8, 12, 8, 12);
        row->setMarginBottom(3);
        row->setCornerRadius(6);
        row->setBackgroundColor(nvgRGBA(50, 50, 60, 200));
        row->setFocusable(true);

        // Left side: track number + artist + title
        auto* leftBox = new brls::Box();
        leftBox->setAxis(brls::Axis::ROW);
        leftBox->setAlignItems(brls::AlignItems::CENTER);
        leftBox->setGrow(1.0f);

        auto* trackNum = new brls::Label();
        trackNum->setFontSize(13);
        trackNum->setMarginRight(10);
        trackNum->setTextColor(nvgRGBA(150, 150, 150, 255));
        trackNum->setText(std::to_string(i + 1));
        leftBox->addView(trackNum);

        auto* titleLabel = new brls::Label();
        titleLabel->setFontSize(13);
        std::string displayTitle = track.name;
        if (!track.artistName.empty()) {
            displayTitle = track.artistName + " - " + displayTitle;
        }
        // Truncate for Vita screen
        if (displayTitle.length() > 55) {
            displayTitle = displayTitle.substr(0, 52) + "...";
        }
        titleLabel->setText(displayTitle);
        leftBox->addView(titleLabel);

        row->addView(leftBox);

        // Right side: duration (already in seconds)
        if (track.duration > 0) {
            auto* durLabel = new brls::Label();
            durLabel->setFontSize(12);
            durLabel->setTextColor(nvgRGBA(150, 150, 150, 255));
            int min = track.duration / 60;
            int sec = track.duration % 60;
            char durStr[16];
            snprintf(durStr, sizeof(durStr), "%d:%02d", min, sec);
            durLabel->setText(durStr);
            row->addView(durLabel);
        }

        // Click to play track - only capture the index, reference m_playlistTracks via 'this'
        size_t idx = i;
        row->registerClickAction([this, idx](brls::View*) {
            performPlaylistTrackAction(idx);
            return true;
        });
        row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

        m_trackListBox->addView(row);
    }

    m_trackListRendered = end;

    // Add "Load More" button if there are remaining tracks
    if (m_trackListRendered < m_playlistTracks.size()) {
        size_t remaining = m_playlistTracks.size() - m_trackListRendered;
        m_trackListLoadMoreBtn = new brls::Button();
        auto* label = new brls::Label();
        label->setText("Load More (" + std::to_string(remaining) + " remaining)");
        label->setFontSize(16);
        label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        m_trackListLoadMoreBtn->addView(label);
        m_trackListLoadMoreBtn->setMarginTop(10);
        m_trackListLoadMoreBtn->setHeight(44);
        m_trackListLoadMoreBtn->registerClickAction([this](brls::View*) {
            appendTrackListPage();
            return true;
        });
        m_trackListLoadMoreBtn->addGestureRecognizer(new brls::TapGestureRecognizer(m_trackListLoadMoreBtn));
        m_trackListBox->addView(m_trackListLoadMoreBtn);
    }
}

void LibrarySectionTab::performPlaylistTrackAction(size_t trackIndex) {
    if (trackIndex >= m_playlistTracks.size()) return;

    const MusicItem& track = m_playlistTracks[trackIndex];
    TrackDefaultAction action = Application::getInstance().getSettings().trackDefaultAction;
    MusicQueue& queue = MusicQueue::getInstance();

    auto executeAction = [this, &track, trackIndex, &queue](TrackDefaultAction act) {
        switch (act) {
            case TrackDefaultAction::PLAY_NOW_CLEAR:
            default:
                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                break;

            case TrackDefaultAction::PLAY_NEXT:
                if (queue.isEmpty()) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else {
                    queue.insertTrackAfterCurrent(track);
                    brls::Application::notify("Playing next: " + track.name);
                }
                break;

            case TrackDefaultAction::ADD_TO_BOTTOM:
                if (queue.isEmpty()) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else {
                    queue.addTrack(track);
                    brls::Application::notify("Added to queue: " + track.name);
                }
                break;

            case TrackDefaultAction::PLAY_NOW_REPLACE:
                if (queue.isEmpty()) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else {
                    queue.insertTrackAfterCurrent(track);
                    if (queue.playNext()) {
                        brls::Application::notify("Now playing: " + track.name);
                    }
                }
                break;

            case TrackDefaultAction::ASK_EACH_TIME:
                break;  // Handled below
        }
    };

    if (action == TrackDefaultAction::ASK_EACH_TIME) {
        auto* dialog = new brls::Dialog("Choose Action");
        auto* optionsBox = new brls::Box();
        optionsBox->setAxis(brls::Axis::COLUMN);
        optionsBox->setPadding(20);

        auto addBtn = [this, trackIndex, dialog, &optionsBox](const std::string& text, TrackDefaultAction act) {
            auto* btn = new brls::Button();
            btn->setText(text);
            btn->setHeight(44);
            btn->setMarginBottom(10);
            btn->registerClickAction([this, dialog, trackIndex, act](brls::View*) {
                dialog->dismiss();
                if (trackIndex >= m_playlistTracks.size()) return true;
                const MusicItem& t = m_playlistTracks[trackIndex];
                if (act == TrackDefaultAction::PLAY_NOW_CLEAR) {
                    brls::Application::pushActivity(
                        PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                } else if (act == TrackDefaultAction::PLAY_NEXT) {
                    MusicQueue& q = MusicQueue::getInstance();
                    if (q.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                    } else {
                        q.insertTrackAfterCurrent(t);
                        brls::Application::notify("Playing next: " + t.name);
                    }
                } else if (act == TrackDefaultAction::ADD_TO_BOTTOM) {
                    MusicQueue& q = MusicQueue::getInstance();
                    if (q.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                    } else {
                        q.addTrack(t);
                        brls::Application::notify("Added to queue: " + t.name);
                    }
                } else if (act == TrackDefaultAction::PLAY_NOW_REPLACE) {
                    MusicQueue& q = MusicQueue::getInstance();
                    if (q.isEmpty()) {
                        brls::Application::pushActivity(
                            PlayerActivity::createWithQueue(m_playlistTracks, trackIndex));
                    } else {
                        q.insertTrackAfterCurrent(t);
                        if (q.playNext()) {
                            brls::Application::notify("Now playing: " + t.name);
                        }
                    }
                }
                return true;
            });
            btn->addGestureRecognizer(new brls::TapGestureRecognizer(btn));
            optionsBox->addView(btn);
        };

        addBtn("Play Playlist from Here", TrackDefaultAction::PLAY_NOW_CLEAR);
        addBtn("Play Next", TrackDefaultAction::PLAY_NEXT);
        addBtn("Add to Bottom of Queue", TrackDefaultAction::ADD_TO_BOTTOM);
        addBtn("Play Now (Replace Current)", TrackDefaultAction::PLAY_NOW_REPLACE);

        dialog->addView(optionsBox);
        dialog->open();
    } else {
        executeAction(action);
    }
}

void LibrarySectionTab::showPlaylistContextMenu(const MusicItem& playlist) {
    brls::Logger::debug("LibrarySectionTab: Context menu for playlist '{}' (id={})", playlist.name, playlist.itemId);

    auto* dialog = new brls::Dialog(playlist.name);
    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    // Show track count if available
    if (playlist.itemCount > 0) {
        auto* infoLabel = new brls::Label();
        infoLabel->setText(std::to_string(playlist.itemCount) + " tracks");
        infoLabel->setFontSize(14);
        infoLabel->setTextColor(nvgRGBA(180, 180, 180, 255));
        infoLabel->setMarginBottom(15);
        optionsBox->addView(infoLabel);
    }

    // "View Tracks" button
    std::string playlistId = playlist.itemId;
    std::string playlistName = playlist.name;
    auto* viewBtn = new brls::Button();
    viewBtn->setText("View Tracks");
    viewBtn->setHeight(44);
    viewBtn->setMarginBottom(10);
    viewBtn->registerClickAction([this, dialog, playlistId, playlistName](brls::View*) {
        dialog->dismiss();
        MusicItem pl;
        pl.itemId = playlistId;
        pl.name = playlistName;
        pl.mediaType = MediaType::PLAYLIST;
        onPlaylistSelected(pl);
        return true;
    });
    viewBtn->addGestureRecognizer(new brls::TapGestureRecognizer(viewBtn));
    optionsBox->addView(viewBtn);

    // "Play All" button
    auto* playBtn = new brls::Button();
    playBtn->setText("Play All");
    playBtn->setHeight(44);
    playBtn->setMarginBottom(10);
    playBtn->registerClickAction([this, dialog, playlistId](brls::View*) {
        dialog->dismiss();
        playPlaylistWithQueue(playlistId, 0);
        return true;
    });
    playBtn->addGestureRecognizer(new brls::TapGestureRecognizer(playBtn));
    optionsBox->addView(playBtn);

    // "Delete" button (only for editable playlists)
    if (playlist.isEditable) {
        auto* deleteBtn = new brls::Button();
        deleteBtn->setText("Delete Playlist");
        deleteBtn->setHeight(44);
        deleteBtn->setMarginBottom(10);
        deleteBtn->registerClickAction([this, dialog, playlistId, playlistName](brls::View*) {
            dialog->dismiss();
            showPlaylistOptionsDialog(MusicItem{playlistId, playlistName, "", "", "", MediaType::PLAYLIST});
            return true;
        });
        deleteBtn->addGestureRecognizer(new brls::TapGestureRecognizer(deleteBtn));
        optionsBox->addView(deleteBtn);
    }

    dialog->addView(optionsBox);
    dialog->open();
}

void LibrarySectionTab::showPlaylistOptionsDialog(const MusicItem& playlist) {
    auto* dialog = new brls::Dialog("Delete \"" + playlist.name + "\"?");
    auto* optionsBox = new brls::Box();
    optionsBox->setAxis(brls::Axis::COLUMN);
    optionsBox->setPadding(20);

    auto* infoLabel = new brls::Label();
    infoLabel->setText("This action cannot be undone.");
    infoLabel->setFontSize(14);
    infoLabel->setTextColor(nvgRGBA(255, 150, 150, 255));
    infoLabel->setMarginBottom(15);
    optionsBox->addView(infoLabel);

    std::string playlistId = playlist.itemId;
    auto alive = m_alive;

    auto* confirmBtn = new brls::Button();
    confirmBtn->setText("Delete");
    confirmBtn->setHeight(44);
    confirmBtn->setMarginBottom(10);
    confirmBtn->registerClickAction([this, dialog, playlistId, alive](brls::View*) {
        dialog->dismiss();
        MAClient::instance().deletePlaylist(playlistId,
            [this, alive, playlistId](bool success, const Json& result) {
                if (!alive || !*alive) return;
                brls::sync([this, alive, success, playlistId]() {
                    if (!alive || !*alive) return;
                    if (success) {
                        // Remove from local list
                        m_playlists.erase(
                            std::remove_if(m_playlists.begin(), m_playlists.end(),
                                [&playlistId](const MusicItem& p) { return p.itemId == playlistId; }),
                            m_playlists.end());
                        brls::Application::notify("Playlist deleted");
                        // Refresh view if we're showing playlists
                        if (m_viewMode == LibraryViewMode::PLAYLISTS) {
                            showPlaylists();
                        }
                    } else {
                        brls::Application::notify("Failed to delete playlist");
                    }
                });
            });
        return true;
    });
    confirmBtn->addGestureRecognizer(new brls::TapGestureRecognizer(confirmBtn));
    optionsBox->addView(confirmBtn);

    auto* cancelBtn = new brls::Button();
    cancelBtn->setText("Cancel");
    cancelBtn->setHeight(44);
    cancelBtn->registerClickAction([dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    cancelBtn->addGestureRecognizer(new brls::TapGestureRecognizer(cancelBtn));
    optionsBox->addView(cancelBtn);

    dialog->addView(optionsBox);
    dialog->open();
}

void LibrarySectionTab::playPlaylistWithQueue(const std::string& playlistId, int startIndex) {
    auto alive = m_alive;

    // If we already have the playlist tracks loaded, use them directly
    if (!m_playlistTracks.empty() && m_currentPlaylistId == playlistId) {
        auto* playerActivity = PlayerActivity::createWithQueue(m_playlistTracks, startIndex);
        brls::Application::pushActivity(playerActivity);
        return;
    }

    MAClient::instance().getPlaylistTracks(playlistId, [alive, startIndex](bool success, const Json& result) {
        if (!alive || !*alive) return;
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
                mi.imageUrl   = obj.has("image")         ? obj["image"].str()       :
                                obj.has("image_url")     ? obj["image_url"].str()   : "";
                mi.mediaType  = MediaType::TRACK;
                mi.duration   = obj.has("duration")      ? obj["duration"].intVal() : 0;
                mi.provider   = obj.has("provider")      ? obj["provider"].str()    : "";
                if (obj.has("artist_name"))  mi.artistName  = obj["artist_name"].str();
                if (obj.has("album_name"))   mi.albumName   = obj["album_name"].str();
                if (obj.has("track_number")) mi.trackNumber = obj["track_number"].intVal();
                tracks.push_back(mi);
            }
        }

        brls::sync([tracks = std::move(tracks), startIndex]() {
            if (!tracks.empty()) {
                brls::Application::pushActivity(
                    PlayerActivity::createWithQueue(tracks, startIndex));
            } else {
                brls::Application::notify("Playlist is empty");
            }
        });
    });
}

} // namespace vita_ma

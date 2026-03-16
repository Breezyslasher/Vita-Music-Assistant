/**
 * Vita Music Assistant - Media Detail View implementation
 * Music-only: supports ARTIST, ALBUM, TRACK, PLAYLIST, RADIO types.
 */

#include "view/media_detail_view.hpp"
#include "view/media_item_cell.hpp"
#include "view/progress_dialog.hpp"
#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "app/ma_client.hpp"
#include "app/music_queue.hpp"
#include "utils/image_loader.hpp"
#include "utils/async.hpp"
#include <thread>

#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

namespace vita_ma {

MediaDetailView::MediaDetailView(const MusicItem& item)
    : m_item(item), m_alive(std::make_shared<std::atomic<bool>>(true)) {

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setGrow(1.0f);

    // Register back button (B/Circle) to pop this activity
    this->registerAction("Back", brls::ControllerButton::BUTTON_B, [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    }, false, false, brls::Sound::SOUND_BACK);

    // Create scrollable content
    m_scrollView = new brls::ScrollingFrame();
    m_scrollView->setGrow(1.0f);

    m_mainContent = new brls::Box();
    m_mainContent->setAxis(brls::Axis::COLUMN);
    m_mainContent->setPadding(30);

    // Top row - poster and info
    auto* topRow = new brls::Box();
    topRow->setAxis(brls::Axis::ROW);
    topRow->setJustifyContent(brls::JustifyContent::FLEX_START);
    topRow->setAlignItems(brls::AlignItems::FLEX_START);
    topRow->setMarginBottom(20);

    // Left side - poster (square album art for all music types)
    auto* leftBox = new brls::Box();
    leftBox->setAxis(brls::Axis::COLUMN);
    leftBox->setWidth(200);
    leftBox->setMarginRight(30);

    auto* posterContainer = new brls::Box();
    posterContainer->setAxis(brls::Axis::COLUMN);
    posterContainer->setWidth(200);

    m_posterImage = new brls::Image();
    m_posterImage->setWidth(200);
    m_posterImage->setHeight(200);
    posterContainer->setHeight(200);
    m_posterImage->setScalingType(brls::ImageScalingType::FIT);
    m_posterImage->setVisibility(brls::Visibility::INVISIBLE);
    posterContainer->addView(m_posterImage);

    leftBox->addView(posterContainer);
    topRow->addView(leftBox);

    // Right side - details
    auto* rightBox = new brls::Box();
    rightBox->setAxis(brls::Axis::COLUMN);
    rightBox->setGrow(1.0f);

    // Title
    m_titleLabel = new brls::Label();
    m_titleLabel->setText(m_item.name);
    m_titleLabel->setFontSize(26);
    m_titleLabel->setMarginBottom(10);
    rightBox->addView(m_titleLabel);

    // Metadata row
    auto* metaBox = new brls::Box();
    metaBox->setAxis(brls::Axis::ROW);
    metaBox->setMarginBottom(15);

    if (m_item.year > 0) {
        m_yearLabel = new brls::Label();
        m_yearLabel->setText(std::to_string(m_item.year));
        m_yearLabel->setFontSize(16);
        m_yearLabel->setMarginRight(15);
        metaBox->addView(m_yearLabel);
    }

    if (m_item.duration > 0) {
        m_durationLabel = new brls::Label();
        int minutes = m_item.duration / 60;
        m_durationLabel->setText(std::to_string(minutes) + " min");
        m_durationLabel->setFontSize(16);
        metaBox->addView(m_durationLabel);
    }

    rightBox->addView(metaBox);

    // Description/biography - shown for artists (biography) or general info
    if (m_item.mediaType == MediaType::ARTIST) {
        m_fullDescription = m_item.biography;
    }

    if (!m_fullDescription.empty()) {
        m_summaryScroll = new brls::ScrollingFrame();
        m_summaryScroll->setHeight(200);
        m_summaryScroll->setMarginBottom(20);

        m_summaryLabel = new brls::Label();
        m_summaryLabel->setFontSize(16);
        m_summaryLabel->setText(m_fullDescription);
        m_summaryLabel->setFocusable(true);

        m_summaryScroll->setContentView(m_summaryLabel);
        rightBox->addView(m_summaryScroll);
    }

    topRow->addView(rightBox);
    m_mainContent->addView(topRow);

    // Track list for albums (vertical list with nested scrolling)
    if (m_item.mediaType == MediaType::ALBUM) {
        auto* tracksLabel = new brls::Label();
        tracksLabel->setText("Tracks");
        tracksLabel->setFontSize(20);
        tracksLabel->setMarginBottom(10);
        m_mainContent->addView(tracksLabel);

        // Wrap track list in its own ScrollingFrame so only tracks scroll
        auto* trackScroll = new brls::ScrollingFrame();
        trackScroll->setGrow(1.0f);
        trackScroll->setMinHeight(200);

        m_trackListBox = new brls::Box();
        m_trackListBox->setAxis(brls::Axis::COLUMN);
        m_trackListBox->setJustifyContent(brls::JustifyContent::FLEX_START);
        m_trackListBox->setAlignItems(brls::AlignItems::STRETCH);

        trackScroll->setContentView(m_trackListBox);
        m_mainContent->addView(trackScroll);
    }

    // Music categories container for artists - scrollable below fixed header
    if (m_item.mediaType == MediaType::ARTIST) {
        auto* categoriesScroll = new brls::ScrollingFrame();
        categoriesScroll->setGrow(1.0f);

        m_musicCategoriesBox = new brls::Box();
        m_musicCategoriesBox->setAxis(brls::Axis::COLUMN);

        categoriesScroll->setContentView(m_musicCategoriesBox);
        m_mainContent->addView(categoriesScroll);
    }

    if (m_item.mediaType == MediaType::ALBUM ||
        m_item.mediaType == MediaType::ARTIST) {
        // Top info is fixed, only media content below scrolls in its own container
        m_mainContent->setGrow(1.0f);
        this->addView(m_mainContent);
    } else {
        m_scrollView->setContentView(m_mainContent);
        this->addView(m_scrollView);
    }

    // Load full details
    loadDetails();
}

MediaDetailView::~MediaDetailView() {
    if (m_alive) {
        m_alive->store(false);
    }
    brls::Logger::debug("MediaDetailView: Destroyed");
}

brls::HScrollingFrame* MediaDetailView::createMediaRow(const std::string& title, brls::Box** contentOut) {
    auto* label = new brls::Label();
    label->setText(title);
    label->setFontSize(20);
    label->setMarginBottom(10);
    label->setMarginTop(15);
    m_musicCategoriesBox->addView(label);

    auto* scrollFrame = new brls::HScrollingFrame();
    scrollFrame->setHeight(150);
    scrollFrame->setMarginBottom(10);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::ROW);
    content->setJustifyContent(brls::JustifyContent::FLEX_START);

    scrollFrame->setContentView(content);
    m_musicCategoriesBox->addView(scrollFrame);

    if (contentOut) {
        *contentOut = content;
    }

    return scrollFrame;
}

brls::View* MediaDetailView::create() {
    return nullptr; // Factory not used
}

void MediaDetailView::loadDetails() {
    std::string itemId = m_item.itemId;
    std::string imageUrl = m_item.imageUrl;
    MediaType mediaType = m_item.mediaType;

    // Load thumbnail
    if (m_posterImage && !imageUrl.empty()) {
        MAClient& client = MAClient::instance();
        std::string url = client.getThumbnailUrl(imageUrl, 400, 400);
        ImageLoader::loadAsync(url, [](brls::Image* image) {
            image->setVisibility(brls::Visibility::VISIBLE);
        }, m_posterImage, m_alive);
    }

    // TODO: fetchMediaDetails does not exist yet in new MA API.
    // For now we use the item data we already have from the list view.
    // When the MA API adds a getItem(itemId) -> full details method, call it here.

    // Load children if applicable
    if (m_item.mediaType == MediaType::ARTIST) {
        loadMusicCategories();
    } else if (m_item.mediaType == MediaType::ALBUM) {
        loadTrackList();
    }
}

void MediaDetailView::loadChildren() {
    // TODO: fetchChildren does not exist in new MA API.
    // Use getAlbumTracks / getArtistAlbums etc. via MAClient when needed.
}

void MediaDetailView::loadMusicCategories() {
    if (!m_musicCategoriesBox) return;

    std::string itemId = m_item.itemId;
    std::string provider = m_item.provider;
    std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;

    asyncRun([this, itemId, provider, aliveWeak]() {
        MAClient& client = MAClient::instance();

        // Fetch artist's albums via the new MA API
        bool done = false;
        std::vector<MusicItem> allAlbumItems;

        client.getArtistAlbums(itemId, [&done, &allAlbumItems](bool success, const Json& result) {
            if (success && result.type() == Json::ARRAY) {
                for (size_t i = 0; i < result.size(); i++) {
                    const Json& item = result[i];
                    MusicItem mi;
                    mi.itemId = item.has("item_id") ? item["item_id"].str() : "";
                    mi.name = item.has("name") ? item["name"].str() : "";
                    // Extract image URL: try image object, then metadata.images array
                    if (item.has("image") && item["image"].type() == Json::OBJECT && item["image"].has("path")) {
                        mi.imageUrl = item["image"]["path"].str();
                    } else if (item.has("image") && item["image"].type() == Json::STRING) {
                        mi.imageUrl = item["image"].str();
                    } else if (item.has("metadata") && item["metadata"].type() == Json::OBJECT) {
                        const Json& meta = item["metadata"];
                        if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
                            const Json& img = meta["images"][static_cast<size_t>(0)];
                            if (img.has("path")) mi.imageUrl = img["path"].str();
                        }
                    }
                    mi.mediaType = MediaType::ALBUM;
                    mi.year = item.has("year") ? item["year"].intVal() : 0;
                    mi.subtype = item.has("album_type") ? item["album_type"].str() :
                                 (item.has("subtype") ? item["subtype"].str() : "");
                    mi.artistName = item.has("artist") ? item["artist"].str() :
                                    (item.has("artists") && item["artists"].size() > 0 ?
                                     item["artists"][static_cast<size_t>(0)].has("name") ?
                                     item["artists"][static_cast<size_t>(0)]["name"].str() : "" : "");
                    mi.provider = item.has("provider") ? item["provider"].str() : "";
                    mi.uri = item.has("uri") ? item["uri"].str() : "";
                    allAlbumItems.push_back(mi);
                }
            }
            done = true;
        }, provider);

        // Wait for async response (simplified blocking wait for Vita)
        // TODO: Refactor to fully async callback chain
        int waitMs = 0;
        while (!done && waitMs < 10000) {
#ifdef __vita__
            sceKernelDelayThread(50 * 1000);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
            waitMs += 50;
        }

        brls::sync([this, allAlbumItems, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;

            m_musicCategoriesBox->clearViews();

            // Group albums by subtype
            std::vector<MusicItem> albums, singles, eps, compilations, soundtracks, other;
            for (const auto& item : allAlbumItems) {
                std::string subtype = item.subtype;
                for (char& c : subtype) c = tolower(c);

                if (subtype == "single") {
                    singles.push_back(item);
                } else if (subtype == "ep") {
                    eps.push_back(item);
                } else if (subtype == "compilation") {
                    compilations.push_back(item);
                } else if (subtype == "soundtrack") {
                    soundtracks.push_back(item);
                } else if (subtype == "album" || subtype.empty()) {
                    albums.push_back(item);
                } else {
                    other.push_back(item);
                }
            }

            auto addCategory = [this](const std::string& title, const std::vector<MusicItem>& items) {
                if (items.empty()) return;

                brls::Box* content = nullptr;
                createMediaRow(title + " (" + std::to_string(items.size()) + ")", &content);

                for (const auto& item : items) {
                    auto* cell = new MediaItemCell();
                    cell->setItem(item);
                    cell->setMarginRight(10);

                    MusicItem capturedItem = item;
                    cell->registerClickAction([this, capturedItem](brls::View* view) {
                        auto* detailView = new MediaDetailView(capturedItem);
                        brls::Application::pushActivity(new brls::Activity(detailView));
                        return true;
                    });
                    cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));

                    cell->registerAction("Options", brls::ControllerButton::BUTTON_START, [this, capturedItem](brls::View* view) {
                        showAlbumContextMenu(capturedItem);
                        return true;
                    });

                    content->addView(cell);
                }
            };

            addCategory("Albums", albums);
            addCategory("Singles", singles);
            addCategory("EPs", eps);
            addCategory("Compilations", compilations);
            addCategory("Soundtracks", soundtracks);
            addCategory("Other", other);

            // Focus setup: if no description, transfer focus to first category item
            if (m_fullDescription.empty() && m_musicCategoriesBox &&
                !m_musicCategoriesBox->getChildren().empty()) {
                for (auto* child : m_musicCategoriesBox->getChildren()) {
                    auto* hScroll = dynamic_cast<brls::HScrollingFrame*>(child);
                    if (hScroll) {
                        brls::Application::giveFocus(hScroll);
                        break;
                    }
                }
            }
        });
    });
}

void MediaDetailView::onPlay(bool resume) {
    if (m_item.mediaType == MediaType::TRACK) {
        Application::getInstance().pushPlayerActivity(m_item.itemId);
    }
    // For albums, play the first child item
    else if (m_item.mediaType == MediaType::ALBUM) {
        if (!m_children.empty()) {
            // First child is directly playable
            std::vector<MusicItem> tracks = m_children;
            auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
            brls::Application::pushActivity(playerActivity);
        }
    }
}

void MediaDetailView::loadTrackList() {
    if (!m_trackListBox) return;

    std::string itemId = m_item.itemId;
    std::string provider = m_item.provider;
    std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;

    asyncRun([this, itemId, provider, aliveWeak]() {
        MAClient& client = MAClient::instance();

        bool done = false;
        std::vector<MusicItem> tracks;

        client.getAlbumTracks(itemId, [&done, &tracks](bool success, const Json& result) {
            if (success && result.type() == Json::ARRAY) {
                for (size_t i = 0; i < result.size(); i++) {
                    const Json& item = result[i];
                    MusicItem mi;
                    mi.itemId = item.has("item_id") ? item["item_id"].str() : "";
                    mi.name = item.has("name") ? item["name"].str() : "";
                    // Extract image URL: try image object, then metadata.images array
                    if (item.has("image") && item["image"].type() == Json::OBJECT && item["image"].has("path")) {
                        mi.imageUrl = item["image"]["path"].str();
                    } else if (item.has("image") && item["image"].type() == Json::STRING) {
                        mi.imageUrl = item["image"].str();
                    } else if (item.has("metadata") && item["metadata"].type() == Json::OBJECT) {
                        const Json& meta = item["metadata"];
                        if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
                            const Json& img = meta["images"][static_cast<size_t>(0)];
                            if (img.has("path")) mi.imageUrl = img["path"].str();
                        }
                    }
                    mi.mediaType = MediaType::TRACK;
                    mi.trackNumber = item.has("track_number") ? item["track_number"].intVal() : 0;
                    mi.discNumber = item.has("disc_number") ? item["disc_number"].intVal() : 0;
                    mi.duration = item.has("duration") ? item["duration"].intVal() : 0;
                    mi.artistName = item.has("artist") ? item["artist"].str() :
                                    (item.has("artists") && item["artists"].size() > 0 ?
                                     item["artists"][static_cast<size_t>(0)].has("name") ?
                                     item["artists"][static_cast<size_t>(0)]["name"].str() : "" : "");
                    mi.albumName = item.has("album") ? item["album"].str() : "";
                    mi.uri = item.has("uri") ? item["uri"].str() : "";
                    mi.provider = item.has("provider") ? item["provider"].str() : "";
                    tracks.push_back(mi);
                }
            }
            done = true;
        }, provider);

        // Wait for async response
        // TODO: Refactor to fully async callback chain
        int waitMs = 0;
        while (!done && waitMs < 10000) {
#ifdef __vita__
            sceKernelDelayThread(50 * 1000);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
            waitMs += 50;
        }

        brls::sync([this, tracks, aliveWeak]() {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;

            m_trackListBox->clearViews();
            m_children = tracks;

            for (size_t i = 0; i < tracks.size(); i++) {
                const auto& track = tracks[i];

                // Create a row for each track
                auto* row = new brls::Box();
                row->setAxis(brls::Axis::ROW);
                row->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
                row->setAlignItems(brls::AlignItems::CENTER);
                row->setHeight(56);
                row->setPadding(10, 16, 10, 16);
                row->setMarginBottom(4);
                row->setCornerRadius(8);
                row->setBackgroundColor(nvgRGBA(50, 50, 60, 200));
                row->setFocusable(true);

                // Left side: track number + title
                auto* leftBox = new brls::Box();
                leftBox->setAxis(brls::Axis::ROW);
                leftBox->setAlignItems(brls::AlignItems::CENTER);
                leftBox->setGrow(1.0f);

                auto* trackNum = new brls::Label();
                trackNum->setFontSize(14);
                trackNum->setMarginRight(12);
                trackNum->setTextColor(nvgRGBA(150, 150, 150, 255));
                if (track.trackNumber > 0) {
                    trackNum->setText(std::to_string(track.trackNumber));
                } else {
                    trackNum->setText(std::to_string(i + 1));
                }
                leftBox->addView(trackNum);

                auto* titleLabel = new brls::Label();
                titleLabel->setFontSize(14);
                titleLabel->setText(track.name);
                leftBox->addView(titleLabel);

                row->addView(leftBox);

                // Right side: button hint + duration
                auto* rightSide = new brls::Box();
                rightSide->setAxis(brls::Axis::ROW);
                rightSide->setAlignItems(brls::AlignItems::CENTER);

                auto* hintIcon = new brls::Image();
                hintIcon->setImageFromRes("images/square_button.png");
                hintIcon->setWidth(16);
                hintIcon->setHeight(16);
                hintIcon->setMarginRight(2);
                hintIcon->setVisibility(brls::Visibility::INVISIBLE);
                rightSide->addView(hintIcon);

                auto* hintLabel = new brls::Label();
                hintLabel->setFontSize(10);
                hintLabel->setTextColor(nvgRGBA(150, 150, 180, 180));
                hintLabel->setText("Options");
                hintLabel->setMarginRight(10);
                hintLabel->setVisibility(brls::Visibility::INVISIBLE);
                rightSide->addView(hintLabel);

                // Show hint on focus, hide previous
                brls::Image* capturedHintIcon = hintIcon;
                brls::Label* capturedHintLabel = hintLabel;
                row->getFocusEvent()->subscribe([this, capturedHintIcon, capturedHintLabel](brls::View*) {
                    // Hide previously focused hint
                    if (m_currentFocusedHint && m_currentFocusedHint != capturedHintIcon) {
                        m_currentFocusedHint->setVisibility(brls::Visibility::INVISIBLE);
                    }
                    if (m_currentFocusedHintLabel && m_currentFocusedHintLabel != capturedHintLabel) {
                        m_currentFocusedHintLabel->setVisibility(brls::Visibility::INVISIBLE);
                    }
                    // Show current hint
                    capturedHintIcon->setVisibility(brls::Visibility::VISIBLE);
                    capturedHintLabel->setVisibility(brls::Visibility::VISIBLE);
                    m_currentFocusedHint = capturedHintIcon;
                    m_currentFocusedHintLabel = capturedHintLabel;
                });

                if (track.duration > 0) {
                    auto* durLabel = new brls::Label();
                    durLabel->setFontSize(12);
                    durLabel->setTextColor(nvgRGBA(150, 150, 150, 255));
                    int min = track.duration / 60;
                    int sec = track.duration % 60;
                    char durStr[16];
                    snprintf(durStr, sizeof(durStr), "%d:%02d", min, sec);
                    durLabel->setText(durStr);
                    rightSide->addView(durLabel);
                }

                row->addView(rightSide);

                // Click to perform default track action
                MusicItem capturedTrack = track;
                row->registerClickAction([this, capturedTrack, i](brls::View* view) {
                    performTrackAction(capturedTrack, i);
                    return true;
                });
                row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

                // START button always shows track action dialog
                row->registerAction("Options", brls::ControllerButton::BUTTON_START, [this, capturedTrack, i](brls::View* view) {
                    showTrackActionDialog(capturedTrack, i);
                    return true;
                });

                m_trackListBox->addView(row);
            }

            // Set up focus transfer for album track list
            if (!m_trackListBox->getChildren().empty()) {
                brls::View* firstTrack = m_trackListBox->getChildren().front();

                if (m_summaryLabel && !m_fullDescription.empty()) {
                    // Description exists: DOWN from description goes to first track
                    m_summaryLabel->setCustomNavigationRoute(brls::FocusDirection::DOWN, firstTrack);
                    // UP from tracks goes to description
                    for (auto* child : m_trackListBox->getChildren()) {
                        child->setCustomNavigationRoute(brls::FocusDirection::UP, m_summaryLabel);
                    }
                } else {
                    // No description: transfer focus to first track to avoid focus errors
                    brls::Application::giveFocus(firstTrack);
                }
            }
        });
    });
}

void MediaDetailView::performTrackAction(const MusicItem& track, size_t trackIndex) {
    TrackDefaultAction action = Application::getInstance().getSettings().trackDefaultAction;

    if (action == TrackDefaultAction::ASK_EACH_TIME) {
        showTrackActionDialog(track, trackIndex);
        return;
    }

    MusicQueue& queue = MusicQueue::getInstance();

    switch (action) {
        case TrackDefaultAction::PLAY_NEXT:
            // Add after current track in queue
            if (queue.isEmpty()) {
                std::vector<MusicItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(track);
                brls::Application::notify("Playing next: " + track.name);
            }
            break;

        case TrackDefaultAction::PLAY_NOW_REPLACE:
            // Replace current track and play
            if (queue.isEmpty()) {
                std::vector<MusicItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(track);
                if (queue.playNext()) {
                    brls::Application::notify("Now playing: " + track.name);
                }
            }
            break;

        case TrackDefaultAction::ADD_TO_BOTTOM:
            // Add to end of queue
            if (queue.isEmpty()) {
                std::vector<MusicItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.addTrack(track);
                brls::Application::notify("Added to queue: " + track.name);
            }
            break;

        case TrackDefaultAction::PLAY_NOW_CLEAR:
        default:
            // Clear queue and play just this track
            {
                std::vector<MusicItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            }
            break;
    }
}

void MediaDetailView::showTrackActionDialog(const MusicItem& track, size_t trackIndex) {
    auto* dialog = new brls::Dialog("Choose Action");

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

    MusicItem capturedTrack = track;
    size_t capturedIndex = trackIndex;

    addDialogButton("Play Now (Clear Queue)", [this, capturedTrack, dialog](brls::View*) {
        dialog->dismiss();
        std::vector<MusicItem> single = {capturedTrack};
        auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
        brls::Application::pushActivity(playerActivity);
        return true;
    });

    addDialogButton("Play Next", [this, capturedTrack, dialog](brls::View*) {
        dialog->dismiss();
        MusicQueue& queue = MusicQueue::getInstance();
        if (queue.isEmpty()) {
            std::vector<MusicItem> single = {capturedTrack};
            auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
            brls::Application::pushActivity(playerActivity);
        } else {
            queue.insertTrackAfterCurrent(capturedTrack);
            brls::Application::notify("Playing next: " + capturedTrack.name);
        }
        return true;
    });

    addDialogButton("Add to Bottom of Queue", [this, capturedTrack, dialog](brls::View*) {
        dialog->dismiss();
        MusicQueue& queue = MusicQueue::getInstance();
        if (queue.isEmpty()) {
            std::vector<MusicItem> single = {capturedTrack};
            auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
            brls::Application::pushActivity(playerActivity);
        } else {
            queue.addTrack(capturedTrack);
            brls::Application::notify("Added to queue: " + capturedTrack.name);
        }
        return true;
    });

    addDialogButton("Add to Playlist", [this, capturedTrack, dialog](brls::View*) {
        dialog->dismiss();
        // TODO: Implement playlist picker using MAClient::getLibraryPlaylists()
        // and MAClient::playMedia() or a future addToPlaylist API
        brls::Application::notify("Add to playlist: not yet implemented");
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

void MediaDetailView::showAlbumContextMenu(const MusicItem& album) {
    showAlbumContextMenuStatic(album);
}

void MediaDetailView::showArtistContextMenuStatic(const MusicItem& artist) {
    auto* dialog = new brls::Dialog(artist.name);

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

    MusicItem capturedArtist = artist;

    // Shuffle Artist - fetch all tracks and shuffle
    addDialogButton("Shuffle Artist", [capturedArtist, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedArtist]() {
            MAClient& client = MAClient::instance();

            bool done = false;
            std::vector<MusicItem> allTracks;

            client.getArtistTracks(capturedArtist.itemId, [&done, &allTracks, &capturedArtist](bool success, const Json& result) {
                if (success && result.type() == Json::ARRAY) {
                    for (size_t i = 0; i < result.size(); i++) {
                        const Json& item = result[i];
                        MusicItem mi;
                        mi.itemId = item.has("item_id") ? item["item_id"].str() : "";
                        mi.name = item.has("name") ? item["name"].str() : "";
                        // Extract image URL: try image object, then metadata.images array
                        if (item.has("image") && item["image"].type() == Json::OBJECT && item["image"].has("path")) {
                            mi.imageUrl = item["image"]["path"].str();
                        } else if (item.has("image") && item["image"].type() == Json::STRING) {
                            mi.imageUrl = item["image"].str();
                        } else if (item.has("metadata") && item["metadata"].type() == Json::OBJECT) {
                            const Json& meta = item["metadata"];
                            if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
                                const Json& img = meta["images"][static_cast<size_t>(0)];
                                if (img.has("path")) mi.imageUrl = img["path"].str();
                            }
                        }
                        mi.mediaType = MediaType::TRACK;
                        mi.duration = item.has("duration") ? item["duration"].intVal() : 0;
                        mi.uri = item.has("uri") ? item["uri"].str() : "";
                        mi.artistName = capturedArtist.name;
                        allTracks.push_back(mi);
                    }
                }
                done = true;
            }, capturedArtist.provider);

            int waitMs = 0;
            while (!done && waitMs < 10000) {
#ifdef __vita__
                sceKernelDelayThread(50 * 1000);
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
                waitMs += 50;
            }

            if (allTracks.empty()) {
                brls::sync([]() { brls::Application::notify("No tracks found"); });
                return;
            }

            // Shuffle
            srand((unsigned)time(nullptr));
            for (size_t i = allTracks.size() - 1; i > 0; i--) {
                size_t j = rand() % (i + 1);
                std::swap(allTracks[i], allTracks[j]);
            }

            brls::sync([allTracks]() {
                auto* playerActivity = PlayerActivity::createWithQueue(allTracks, 0);
                brls::Application::pushActivity(playerActivity);
            });
        });
        return true;
    });

    // Play All (in order)
    addDialogButton("Play All", [capturedArtist, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedArtist]() {
            MAClient& client = MAClient::instance();

            bool done = false;
            std::vector<MusicItem> allTracks;

            client.getArtistTracks(capturedArtist.itemId, [&done, &allTracks, &capturedArtist](bool success, const Json& result) {
                if (success && result.type() == Json::ARRAY) {
                    for (size_t i = 0; i < result.size(); i++) {
                        const Json& item = result[i];
                        MusicItem mi;
                        mi.itemId = item.has("item_id") ? item["item_id"].str() : "";
                        mi.name = item.has("name") ? item["name"].str() : "";
                        // Extract image URL: try image object, then metadata.images array
                        if (item.has("image") && item["image"].type() == Json::OBJECT && item["image"].has("path")) {
                            mi.imageUrl = item["image"]["path"].str();
                        } else if (item.has("image") && item["image"].type() == Json::STRING) {
                            mi.imageUrl = item["image"].str();
                        } else if (item.has("metadata") && item["metadata"].type() == Json::OBJECT) {
                            const Json& meta = item["metadata"];
                            if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
                                const Json& img = meta["images"][static_cast<size_t>(0)];
                                if (img.has("path")) mi.imageUrl = img["path"].str();
                            }
                        }
                        mi.mediaType = MediaType::TRACK;
                        mi.duration = item.has("duration") ? item["duration"].intVal() : 0;
                        mi.uri = item.has("uri") ? item["uri"].str() : "";
                        mi.artistName = capturedArtist.name;
                        allTracks.push_back(mi);
                    }
                }
                done = true;
            }, capturedArtist.provider);

            int waitMs = 0;
            while (!done && waitMs < 10000) {
#ifdef __vita__
                sceKernelDelayThread(50 * 1000);
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
                waitMs += 50;
            }

            if (allTracks.empty()) {
                brls::sync([]() { brls::Application::notify("No tracks found"); });
                return;
            }

            brls::sync([allTracks]() {
                auto* playerActivity = PlayerActivity::createWithQueue(allTracks, 0);
                brls::Application::pushActivity(playerActivity);
            });
        });
        return true;
    });

    addDialogButton("Cancel", [dialog](brls::View*) {
        dialog->dismiss(); return true;
    });

    dialog->addView(optionsBox);
    dialog->registerAction("Back", brls::ControllerButton::BUTTON_B, [dialog](brls::View*) {
        dialog->dismiss();
        return true;
    });
    brls::Application::pushActivity(new brls::Activity(dialog));
}

void MediaDetailView::showAlbumContextMenuStatic(const MusicItem& album) {
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
        asyncRun([capturedAlbum]() {
            MAClient& client = MAClient::instance();

            bool done = false;
            std::vector<MusicItem> tracks;

            client.getAlbumTracks(capturedAlbum.itemId, [&done, &tracks](bool success, const Json& result) {
                if (success && result.type() == Json::ARRAY) {
                    for (size_t i = 0; i < result.size(); i++) {
                        const Json& item = result[i];
                        MusicItem mi;
                        mi.itemId = item.has("item_id") ? item["item_id"].str() : "";
                        mi.name = item.has("name") ? item["name"].str() : "";
                        // Extract image URL: try image object, then metadata.images array
                        if (item.has("image") && item["image"].type() == Json::OBJECT && item["image"].has("path")) {
                            mi.imageUrl = item["image"]["path"].str();
                        } else if (item.has("image") && item["image"].type() == Json::STRING) {
                            mi.imageUrl = item["image"].str();
                        } else if (item.has("metadata") && item["metadata"].type() == Json::OBJECT) {
                            const Json& meta = item["metadata"];
                            if (meta.has("images") && meta["images"].type() == Json::ARRAY && meta["images"].size() > 0) {
                                const Json& img = meta["images"][static_cast<size_t>(0)];
                                if (img.has("path")) mi.imageUrl = img["path"].str();
                            }
                        }
                        mi.mediaType = MediaType::TRACK;
                        mi.duration = item.has("duration") ? item["duration"].intVal() : 0;
                        mi.uri = item.has("uri") ? item["uri"].str() : "";
                        tracks.push_back(mi);
                    }
                }
                done = true;
            }, capturedAlbum.provider);

            int waitMs = 0;
            while (!done && waitMs < 10000) {
#ifdef __vita__
                sceKernelDelayThread(50 * 1000);
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
                waitMs += 50;
            }

            if (!tracks.empty()) {
                brls::sync([tracks]() {
                    auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                    brls::Application::pushActivity(playerActivity);
                });
            }
        });
        return true;
    });

    addDialogButton("Play Next", [capturedAlbum, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedAlbum]() {
            MAClient& client = MAClient::instance();

            bool done = false;
            std::vector<MusicItem> tracks;

            client.getAlbumTracks(capturedAlbum.itemId, [&done, &tracks](bool success, const Json& result) {
                if (success && result.type() == Json::ARRAY) {
                    for (size_t i = 0; i < result.size(); i++) {
                        const Json& item = result[i];
                        MusicItem mi;
                        mi.itemId = item.has("item_id") ? item["item_id"].str() : "";
                        mi.name = item.has("name") ? item["name"].str() : "";
                        mi.mediaType = MediaType::TRACK;
                        mi.duration = item.has("duration") ? item["duration"].intVal() : 0;
                        mi.uri = item.has("uri") ? item["uri"].str() : "";
                        tracks.push_back(mi);
                    }
                }
                done = true;
            });

            int waitMs = 0;
            while (!done && waitMs < 10000) {
#ifdef __vita__
                sceKernelDelayThread(50 * 1000);
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
                waitMs += 50;
            }

            if (!tracks.empty()) {
                brls::sync([tracks]() {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (queue.isEmpty()) {
                        auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                        brls::Application::pushActivity(playerActivity);
                    } else {
                        for (int i = (int)tracks.size() - 1; i >= 0; i--) {
                            queue.insertTrackAfterCurrent(tracks[i]);
                        }
                        brls::Application::notify("Album queued next");
                    }
                });
            }
        });
        return true;
    });

    addDialogButton("Add to Bottom of Queue", [capturedAlbum, dialog](brls::View*) {
        dialog->dismiss();
        asyncRun([capturedAlbum]() {
            MAClient& client = MAClient::instance();

            bool done = false;
            std::vector<MusicItem> tracks;

            client.getAlbumTracks(capturedAlbum.itemId, [&done, &tracks](bool success, const Json& result) {
                if (success && result.type() == Json::ARRAY) {
                    for (size_t i = 0; i < result.size(); i++) {
                        const Json& item = result[i];
                        MusicItem mi;
                        mi.itemId = item.has("item_id") ? item["item_id"].str() : "";
                        mi.name = item.has("name") ? item["name"].str() : "";
                        mi.mediaType = MediaType::TRACK;
                        mi.duration = item.has("duration") ? item["duration"].intVal() : 0;
                        mi.uri = item.has("uri") ? item["uri"].str() : "";
                        tracks.push_back(mi);
                    }
                }
                done = true;
            });

            int waitMs = 0;
            while (!done && waitMs < 10000) {
#ifdef __vita__
                sceKernelDelayThread(50 * 1000);
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
                waitMs += 50;
            }

            if (!tracks.empty()) {
                brls::sync([tracks]() {
                    MusicQueue& queue = MusicQueue::getInstance();
                    if (queue.isEmpty()) {
                        auto* playerActivity = PlayerActivity::createWithQueue(tracks, 0);
                        brls::Application::pushActivity(playerActivity);
                    } else {
                        queue.addTracks(tracks);
                        brls::Application::notify("Album added to queue");
                    }
                });
            }
        });
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

void MediaDetailView::performTrackActionStatic(const MusicItem& track) {
    TrackDefaultAction action = Application::getInstance().getSettings().trackDefaultAction;

    if (action == TrackDefaultAction::ASK_EACH_TIME) {
        // Show a simplified action dialog (no playlist option without MediaDetailView context)
        auto* dialog = new brls::Dialog("Choose Action");

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

        MusicItem capturedTrack = track;

        addDialogButton("Play Now (Clear Queue)", [capturedTrack, dialog](brls::View*) {
            dialog->dismiss();
            std::vector<MusicItem> single = {capturedTrack};
            auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
            brls::Application::pushActivity(playerActivity);
            return true;
        });

        addDialogButton("Play Next", [capturedTrack, dialog](brls::View*) {
            dialog->dismiss();
            MusicQueue& queue = MusicQueue::getInstance();
            if (queue.isEmpty()) {
                std::vector<MusicItem> single = {capturedTrack};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(capturedTrack);
                brls::Application::notify("Playing next: " + capturedTrack.name);
            }
            return true;
        });

        addDialogButton("Add to Bottom of Queue", [capturedTrack, dialog](brls::View*) {
            dialog->dismiss();
            MusicQueue& queue = MusicQueue::getInstance();
            if (queue.isEmpty()) {
                std::vector<MusicItem> single = {capturedTrack};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.addTrack(capturedTrack);
                brls::Application::notify("Added to queue: " + capturedTrack.name);
            }
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
        return;
    }

    MusicQueue& queue = MusicQueue::getInstance();

    switch (action) {
        case TrackDefaultAction::PLAY_NEXT:
            if (queue.isEmpty()) {
                std::vector<MusicItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(track);
                brls::Application::notify("Playing next: " + track.name);
            }
            break;

        case TrackDefaultAction::PLAY_NOW_REPLACE:
            if (queue.isEmpty()) {
                std::vector<MusicItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.insertTrackAfterCurrent(track);
                if (queue.playNext()) {
                    brls::Application::notify("Now playing: " + track.name);
                }
            }
            break;

        case TrackDefaultAction::ADD_TO_BOTTOM:
            if (queue.isEmpty()) {
                std::vector<MusicItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            } else {
                queue.addTrack(track);
                brls::Application::notify("Added to queue: " + track.name);
            }
            break;

        case TrackDefaultAction::PLAY_NOW_CLEAR:
        default:
            {
                std::vector<MusicItem> single = {track};
                auto* playerActivity = PlayerActivity::createWithQueue(single, 0);
                brls::Application::pushActivity(playerActivity);
            }
            break;
    }
}

void MediaDetailView::setupChildrenFocusTransfer() {
    // Simplified for music-only: focus navigation for children containers
    // is now handled inline in loadTrackList() and loadMusicCategories().
}

} // namespace vita_ma

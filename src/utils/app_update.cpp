/**
 * Vita Music Assistant - In-app updates implementation
 *
 * Modelled on VitaPlex's utils/app_update (Vita path): see
 * utils/app_update.hpp for the flow.
 */

#include "utils/app_update.hpp"

#include "app.h"
#include "app/application.hpp"
#include "app/shell_companion_bridge.hpp"
#include "utils/http_client.hpp"
#include "view/progress_dialog.hpp"

#include "shell_companion.h"
#include "sc_protocol.h"

#include <borealis.hpp>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

namespace vita_ma {
namespace app_update {

namespace {

constexpr const char* kRepo = "Breezyslasher/Vita-Music-Assistant";
constexpr const char* kUpdateVpk = "ux0:data/VitaMA/update.vpk";

// True while a check or an install runs, so Settings and the start-up check
// can't race each other.
std::atomic<bool> s_busy{false};
// Set by the progress dialog's Cancel; the download's write callback aborts.
std::atomic<bool> s_cancel{false};

struct ReleaseInfo {
    std::string tag;
    std::string assetUrl;
    int64_t assetSize = 0;
};

// ── Small JSON pulls ─────────────────────────────────────────────────────
// The feed is a top-level array of releases, each with an assets array.
// String-aware object walking, so a brace in release notes doesn't end it.

size_t objectEnd(const std::string& body, size_t objStart) {
    int depth = 0;
    bool inString = false;
    for (size_t pos = objStart; pos < body.size(); pos++) {
        const char c = body[pos];
        if (inString) {
            if (c == '\\') { pos++; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') depth++;
        else if (c == '}' && --depth == 0) return pos + 1;
    }
    return body.size();
}

// Calls fn on each object of the first array at or after `from`; fn returns
// false to stop.
template <typename Fn>
void forEachObjectInArray(const std::string& body, size_t from, Fn fn) {
    size_t pos = body.find('[', from);
    if (pos == std::string::npos) return;
    pos++;
    while (pos < body.size()) {
        while (pos < body.size() && body[pos] != '{' && body[pos] != ']') pos++;
        if (pos >= body.size() || body[pos] == ']') break;
        size_t end = objectEnd(body, pos);
        if (!fn(body.substr(pos, end - pos))) return;
        pos = end;
    }
}

// The value of a top-level key of obj: keys inside nested objects (an
// asset's "name" inside a release) are skipped.
size_t topLevelKey(const std::string& obj, const std::string& key) {
    const std::string quoted = "\"" + key + "\"";
    int depth = 0;
    bool inString = false;
    for (size_t pos = 0; pos < obj.size(); pos++) {
        const char c = obj[pos];
        if (inString) {
            if (c == '\\') { pos++; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '{' || c == '[') { depth++; continue; }
        if (c == '}' || c == ']') { depth--; continue; }
        if (c == '"') {
            if (depth == 1 && obj.compare(pos, quoted.size(), quoted) == 0) {
                size_t colon = obj.find_first_not_of(" \t\r\n", pos + quoted.size());
                if (colon != std::string::npos && obj[colon] == ':') return colon + 1;
            }
            inString = true;
        }
    }
    return std::string::npos;
}

std::string jsonString(const std::string& obj, const std::string& key) {
    size_t pos = topLevelKey(obj, key);
    if (pos == std::string::npos) return {};
    pos = obj.find_first_not_of(" \t\n\r", pos);
    if (pos == std::string::npos || obj[pos] != '"') return {};
    std::string out;
    for (size_t i = pos + 1; i < obj.size(); i++) {
        char c = obj[i];
        if (c == '\\' && i + 1 < obj.size()) {
            out += obj[++i];
            continue;
        }
        if (c == '"') break;
        out += c;
    }
    return out;
}

int64_t jsonInt(const std::string& obj, const std::string& key) {
    size_t pos = topLevelKey(obj, key);
    return pos == std::string::npos ? 0 : strtoll(obj.c_str() + pos, nullptr, 10);
}

bool jsonBool(const std::string& obj, const std::string& key) {
    size_t pos = topLevelKey(obj, key);
    if (pos == std::string::npos) return false;
    pos = obj.find_first_not_of(" \t\n\r", pos);
    return pos != std::string::npos && obj.compare(pos, 4, "true") == 0;
}

// ── Versions ─────────────────────────────────────────────────────────────
// Tags read "Beta-0.2.5", VMA_VERSION "0.2.5": compare the first three
// numeric runs of each.

bool parseVersion(const std::string& s, int out[3]) {
    int n = 0;
    size_t i = 0;
    while (i < s.size() && n < 3) {
        if (isdigit((unsigned char)s[i])) {
            out[n++] = atoi(s.c_str() + i);
            while (i < s.size() && isdigit((unsigned char)s[i])) i++;
        } else {
            i++;
        }
    }
    for (int k = n; k < 3; k++) out[k] = 0;
    return n > 0;
}

bool isNewer(const std::string& tag, const std::string& current) {
    int a[3], b[3];
    if (!parseVersion(tag, a) || !parseVersion(current, b)) return false;
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

std::string mbLabel(int64_t bytes) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return buf;
}

void showMessage(const std::string& text) {
    auto* d = new brls::Dialog(text);
    d->addButton("OK", []() {});
    d->open();
}

// ── Install ──────────────────────────────────────────────────────────────

// UI thread: the VPK is on the card; hand it to the plugin and quit, or
// leave it for VitaShell.
void handOver(const ReleaseInfo& rel) {
    auto& bridge = ShellCompanionBridge::instance();
    if (bridge.hasCap("update.vpk") && bridge.installUpdateVpk(kUpdateVpk)) {
        brls::Logger::info("app_update: plugin installs {} once the app has quit", rel.tag);
        brls::Application::quit();
        return;
    }
    brls::Logger::warning("app_update: no shell companion plugin with update.vpk "
                          "(open {}), {} left for VitaShell",
                          bridge.isOpen(), kUpdateVpk);
    showMessage("Update " + rel.tag + " downloaded.\n\n"
                "The shell companion plugin, which installs it, is not loaded or is too old "
                "(restarting the Vita loads this app's own). Open VitaShell and install this "
                "file to finish:\n\n" + std::string(kUpdateVpk));
}

void startInstall(const ReleaseInfo rel) {
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) return;
    s_cancel = false;
    auto* dialog = new ProgressDialog("Updating to " + rel.tag);
    dialog->setStatus("Downloading...");
    dialog->setProgress(0.0f);
    // The dialog is deleted once it is popped: the worker checks `alive`
    // before every UI update, and the dialog clears it on every close path.
    auto alive = std::make_shared<std::atomic<bool>>(true);
    dialog->setCancelCallback([alive]() {
        *alive = false;
        s_cancel = true;
    });
    dialog->show();

    std::thread([rel, dialog, alive]() {
        HttpClient client;
        int64_t total = rel.assetSize;
        int64_t got = 0;
        bool ok = false;

        // One retry: a fresh connection clears most transient failures.
        for (int attempt = 0; attempt < 2 && !s_cancel.load(); attempt++) {
            if (attempt > 0) {
                brls::Logger::warning("app_update: download failed at {} bytes, retrying", got);
                brls::sync([dialog, alive]() {
                    if (*alive) dialog->setStatus("Retrying...");
                });
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            SceUID f = sceIoOpen(kUpdateVpk, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (f < 0) break;
            got = 0;
            int lastPct = -1;
            ok = client.downloadFile(
                rel.assetUrl,
                [&](const char* data, size_t size) -> bool {
                    if (s_cancel.load()) return false;
                    if (sceIoWrite(f, data, size) != (int)size) return false;
                    got += (int64_t)size;
                    int pct = total > 0 ? (int)(got * 100 / total) : -1;
                    if (pct != lastPct) {
                        lastPct = pct;
                        int64_t g = got, t = total;
                        brls::sync([dialog, alive, g, t]() {
                            if (*alive) dialog->updateDownloadProgress(g, t);
                        });
                    }
                    return true;
                },
                [&](int64_t sz) { if (total <= 0) total = sz; });
            sceIoClose(f);
            if (ok && (rel.assetSize <= 0 || got == rel.assetSize)) break;
            ok = false;
        }

        if (s_cancel.load() || !ok) {
            sceIoRemove(kUpdateVpk);
            const bool cancelled = s_cancel.load();
            const int64_t g = got;
            brls::sync([dialog, alive, cancelled, g, rel]() {
                if (*alive) {
                    *alive = false;
                    dialog->dismiss();
                }
                if (!cancelled)
                    showMessage("Update failed: incomplete download (" + std::to_string(g) + "/" +
                                std::to_string(rel.assetSize) + " bytes).");
            });
            brls::Logger::info("app_update: download {} ({} bytes)",
                               cancelled ? "cancelled" : "failed", got);
            s_busy = false;
            return;
        }

        brls::Logger::info("app_update: downloaded {} ({} bytes)", rel.tag, got);
        brls::sync([dialog, alive]() {
            if (*alive) {
                dialog->setProgress(1.0f);
                dialog->setStatus("Closing to install, reopens by itself");
            }
        });
        // Long enough to read the line before the app closes.
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        brls::sync([dialog, alive, rel]() {
            if (!*alive) {
                // Cancelled at the last moment: the VPK stays for later.
                brls::Application::notify("Update kept at " + std::string(kUpdateVpk));
                return;
            }
            *alive = false;
            dialog->dismiss();
            handOver(rel);
        });
        s_busy = false;
    }).detach();
}

void offerUpdate(const ReleaseInfo rel) {
    std::string text = "Update available\n\n" + std::string(VMA_VERSION) + "  \xE2\x86\x92  " +
                       rel.tag;
    if (rel.assetSize > 0) text += "\n" + mbLabel(rel.assetSize) + " download";
    text += "\nInstalls in place and reopens by itself.";
    auto* d = new brls::Dialog(text);
    d->addButton("Later", [rel]() {
        Application& app = Application::getInstance();
        app.getSettings().skippedUpdateVersion = rel.tag;
        app.saveSettings();
    });
    d->addButton("Update", [rel]() { startInstall(rel); });
    // B closes it without skipping the release.
    d->open();
}

}  // namespace

void checkForUpdates(bool manual) {
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) return;
    if (manual) brls::Application::notify("Checking for updates...");

    std::thread([manual]() {
        // At start-up, let login and the first fetches go first.
        if (!manual) std::this_thread::sleep_for(std::chrono::seconds(5));

        HttpClient client;
        HttpRequest req;
        req.url = "https://api.github.com/repos/" + std::string(kRepo) + "/releases?per_page=10";
        req.headers["Accept"] = "application/vnd.github+json";
        req.headers["User-Agent"] = "VitaMusicAssistant/" VMA_VERSION;
        req.timeout = 15;
        HttpResponse resp = client.request(req);
        if (resp.statusCode != 200 || resp.body.empty()) {
            brls::Logger::error("app_update: releases feed HTTP {} {}", resp.statusCode,
                                resp.error);
            if (manual)
                brls::sync([]() { showMessage("Could not reach GitHub to check for updates."); });
            s_busy = false;
            return;
        }

        ReleaseInfo rel;
        forEachObjectInArray(resp.body, 0, [&rel](const std::string& obj) {
            if (jsonBool(obj, "draft")) return true;
            rel.tag = jsonString(obj, "tag_name");
            size_t assetsAt = topLevelKey(obj, "assets");
            if (assetsAt != std::string::npos) {
                forEachObjectInArray(obj, assetsAt, [&rel](const std::string& a) {
                    std::string name = jsonString(a, "name");
                    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".vpk") == 0) {
                        rel.assetUrl = jsonString(a, "browser_download_url");
                        rel.assetSize = jsonInt(a, "size");
                        return false;
                    }
                    return true;
                });
            }
            return false;  // newest non-draft found
        });

        const std::string current = VMA_VERSION;
        if (rel.tag.empty() || rel.assetUrl.empty()) {
            brls::Logger::info("app_update: no release with a .vpk (newest '{}')", rel.tag);
            if (manual) brls::sync([]() { showMessage("No update package found on GitHub."); });
            s_busy = false;
            return;
        }
        if (!isNewer(rel.tag, current)) {
            brls::Logger::info("app_update: up to date ({} vs {})", current, rel.tag);
            if (manual)
                brls::sync([current]() {
                    showMessage("Vita Music Assistant is up to date (" + current + ").");
                });
            s_busy = false;
            return;
        }
        if (!manual && rel.tag == Application::getInstance().getSettings().skippedUpdateVersion) {
            brls::Logger::info("app_update: {} available but skipped", rel.tag);
            s_busy = false;
            return;
        }

        brls::Logger::info("app_update: update available {} (current {}), {} bytes", rel.tag,
                           current, rel.assetSize);
        // The offer holds nothing: closing it in any way leaves checks free.
        s_busy = false;
        brls::sync([rel]() { offerUpdate(rel); });
    }).detach();
}

void afterStart() {
    int result = 0;
    char why[192] = {0};
    if (scReadUpdateResult(&result, why, sizeof(why)) != 0) return;  // no update ran

    char file[256];
    if (scPath(SC_UPDATE_RESULT, file, sizeof(file)) == 0) sceIoRemove(file);
    if (result == 0) {
        brls::Logger::info("app_update: the shell companion plugin installed the update");
        sceIoRemove(kUpdateVpk);
        return;
    }
    brls::Logger::error("app_update: the shell companion plugin did not install the update "
                        "(0x{:08X}): {}", (unsigned)result, why);
    std::string reason = why;
    brls::sync([reason]() {
        showMessage("The update could not be installed" +
                    (reason.empty() ? std::string() : ": " + reason) + ".\n\nIt is kept at " +
                    std::string(kUpdateVpk) +
                    ": install it with VitaShell, or check for updates again in Settings.");
    });
}

}  // namespace app_update
}  // namespace vita_ma

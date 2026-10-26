/**
 * Vita Music Assistant - In-app updates
 *
 * Checks this repository's GitHub releases and installs the newest one's
 * .vpk in place, as VitaPlex does (its utils/app_update):
 *
 *   1. The releases feed is read (/releases, not /releases/latest, which
 *      skips the pre-releases VMA publishes) and the newest non-draft
 *      release compared with VMA_VERSION.
 *   2. Offered in a dialog; "Later" skips that release at start-up.
 *   3. The .vpk is downloaded to ux0:data/VitaMA/update.vpk.
 *   4. A title can't install over itself while it runs, so the shell
 *      companion plugin (0.3.0 on, caps "update.vpk") is handed the VPK
 *      (scUpdateInstallVpk) and the app quits: the plugin unpacks, checks and
 *      installs it once the app has gone, then relaunches it. Without that
 *      plugin the VPK is kept for a VitaShell install.
 *   5. On the next start, afterStart() reads what the plugin made of it.
 */

#pragma once

namespace vita_ma {
namespace app_update {

// Check GitHub for a newer release. manual (Settings) reports "up to date"
// and offers a release the user skipped; the start-up check is silent in
// both cases. Network and parsing run on a worker thread.
void checkForUpdates(bool manual);

// Once per start, after the shell companion is open: the result of an
// update the plugin installed as the app quit (removes the VPK on success,
// tells the user on failure).
void afterStart();

}  // namespace app_update
}  // namespace vita_ma

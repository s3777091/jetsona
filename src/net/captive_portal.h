#pragma once

#include <string>

namespace jetson {

/* Result of an HTTP connectivity check made after a Wi-Fi link is up.
 *
 * Captive portals deliberately intercept a plain-HTTP request.  A normal
 * Internet connection returns HTTP 204 from the probe endpoint; a redirect,
 * an HTML response, or HTTP 511 means the user still has to authenticate. */
enum class InternetAccessState {
    Unknown = -1,
    Offline = 0,
    Online = 1,
    CaptivePortal = 2,
};

struct CaptivePortalResult {
    InternetAccessState state = InternetAccessState::Unknown;
    // Redirect target when the portal supplied a safe HTTP(S) Location.
    // Otherwise this is the plain-HTTP probe URL, which the portal will
    // intercept again when Chromium opens it.
    std::string login_url;
    long http_status = 0;
    std::string error;
};

// Blocking, bounded connectivity probe. Call from a worker thread, never from
// the LVGL handler thread. The URL can be overridden for deployments with
// JETSON_CAPTIVE_PORTAL_PROBE_URL=http://host/path.
CaptivePortalResult ProbeCaptivePortal();

// Returns the validated probe URL used by ProbeCaptivePortal().
std::string CaptivePortalProbeUrl();

} // namespace jetson

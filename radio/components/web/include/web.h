// HTTP server on port 80: the web page, the JSON API, the live event stream and firmware upload.
#pragma once

namespace web {

void start(const char* version);
// True once after an uploaded image has been verified and waits for a knob press.
bool takeConfirmRequest();
// The UI's answer to the confirm screen.
void decide(bool install);
// True once after POST /api/guide/update or /api/firmware/update has stored a download request and sent its answer.
bool takeRestart();

}  // namespace web

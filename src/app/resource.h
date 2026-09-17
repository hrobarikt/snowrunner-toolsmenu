#pragma once

// The injected module, carried inside the exe so there is one file to download
// and nothing to install.
#define IDR_SRTM_DLL 101

// The tray and window icon. Only present when src/app/app.ico exists at
// configure time; the app falls back to the stock Windows icon without it.
#define IDI_APP 102

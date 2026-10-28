#pragma once

// The TSF profile is registered with the DLL path and uIconIndex 0, which
// selects the first icon group in the file. Keep the brand icon the only
// ICON resource so index 0 always resolves to it (DEV-1744).
#define IDI_AZOOKEY_BRAND 1

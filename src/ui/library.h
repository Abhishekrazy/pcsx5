#pragma once

#include "ui/ui.h"
#include "ui/thumbnail.h"

#include "imgui.h"

#include <string>
#include <vector>

namespace Ui {

// Draw the game library panel.  Caller has already set up a dock host
// or a fixed-position child window before calling.  `selected_index` is
// updated in-place when the user clicks a game card.  The Boot / Refresh
// action buttons live in the sidebar (see DrawSidebar) and are wired up
// by the caller; this panel just exposes selection state.
//
// `thumbs` is queried for the cover art of every game.  Cover images
// are loaded lazily and cached, so the first frame after switching
// covers_dir may briefly show a placeholder while textures upload.
void DrawLibraryPanel(const std::vector<GameEntry>& games,
                      int& selected_index,
                      ThumbnailCache& thumbs);

}  // namespace Ui

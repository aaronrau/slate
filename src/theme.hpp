#pragma once

namespace slate {
inline constexpr char theme[] = R"css(
#slate { background: #171b20; color: #d9e1e6; }
#main-menu { background: #22272d; border-bottom: 1px solid #111419; padding: 0 3px; }
#main-menu > menuitem { padding: 3px 9px; color: #d2d9df; font-size: 13px; }
#main-menu > menuitem:hover { background: #363d44; color: #f2f4f5; }
#slate paned > separator { background-image: none; background-color: #30363c; min-width: 1px; }
#tab-strip { background: #191d22; border-bottom: 1px solid #101418; }
#active-tab { background: #262c32; border-right: 1px solid #11151a; padding: 0 16px; }
.tab-title { color: #e0e5e9; font-size: 12px; }
.modified-dot { color: #e6ba7b; font-size: 9px; }
.file-location { color: #919ba3; font-size: 11px; padding: 0 12px; }
#markdown-modes > * { background: #1e2328; color: #b8c2c9; padding: 1px 9px; min-height: 17px;
  min-width: 0; font-size: 11px; border: 1px solid #3b4249; text-shadow: none; box-shadow: none; }
#markdown-modes > *:hover { background: #2b3338; color: #ecf0f2; }
#markdown-modes > *:checked { background: #34423f; color: #c5eee0; border-color: #54655f; }
#status-strip { background: #20252b; border-top: 1px solid #30363c; padding: 2px 10px; }
#status-strip label { color: #b1bac2; font-size: 11px; }
)css";
}

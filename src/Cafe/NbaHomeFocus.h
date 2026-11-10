#pragma once
namespace NbaHomeFocus
{
constexpr bool Requested(bool nba, bool explicitResume) { return nba && explicitResume; }
template<class Windows> bool Restore(Windows& windows, bool requested)
{
    if (!requested) return false;
    const auto window = windows.Window();
    if (!windows.Valid(window) || !windows.Visible(window)) return false;
    const auto owner = windows.Owner(window);
    if (!owner || owner != windows.CurrentProcess()) return false;
    return windows.Activate(window);
}
}

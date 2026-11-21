// The Add place dialog: searches Open-Meteo's place database through the
// fetcher (so the UI never blocks) and hands each pick to a PlaceSink.
#pragma once

#include "common.h"

#include <windows.h>

namespace wt {

class Fetcher;

class PlaceSink {
public:
    virtual bool AddPlace(const Location& loc, std::wstring& err) = 0;
protected:
    ~PlaceSink() = default;
};

// Modal; stays open after each add until Close.
void ShowAddPlaceDialog(HINSTANCE inst, HWND owner, Fetcher& fetcher, PlaceSink& sink);

// "Ottawa, Ontario, CA" (empty parts skipped, '|' removed).
std::wstring PlaceName(const GeoHit& h);

} // namespace wt

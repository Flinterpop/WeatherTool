#include "dialogs.h"

#include "fetcher.h"
#include "http.h"
#include "resource.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cwchar>

namespace wt {
namespace {

struct AddContext {
    Fetcher*   fetcher = nullptr;
    PlaceSink* sink    = nullptr;
    uint64_t   seq     = 0;          // the search whose results the list shows
    std::array<GeoHit, kMaxHits> hits{};
    size_t     count   = 0;
};

void SetHint(HWND dlg, const std::wstring& text) { SetDlgItemTextW(dlg, IDC_HINT, text.c_str()); }

void StartSearch(HWND dlg, AddContext& ctx) {
    std::array<wchar_t, 128> q{};
    GetDlgItemTextW(dlg, IDC_QUERY, q.data(), static_cast<int>(q.size()));
    std::wstring query = q.data();
    while (!query.empty() && query.back() == L' ') { query.pop_back(); }
    if (query.size() < 2) {
        SetHint(dlg, L"Type at least two letters.");
        return;
    }
    const std::wstring url = L"https://geocoding-api.open-meteo.com/v1/search?count=10&language=en&format=json&name=" +
                             UrlEncode(query);
    ++ctx.seq;
    if (!ctx.fetcher->QueueSearch(dlg, ctx.seq, url)) {
        SetHint(dlg, L"The download queue is full; try again in a moment.");
        return;
    }
    SendDlgItemMessageW(dlg, IDC_RESULTS, LB_RESETCONTENT, 0, 0);
    ctx.count = 0;
    SetHint(dlg, L"Searching…");
}

void ShowResults(HWND dlg, AddContext& ctx) {
    SearchResult r;
    while (ctx.fetcher->TakeSearch(r)) {
        if (r.seq != ctx.seq) { continue; }   // an older search, superseded
        if (!r.ok) {
            SetHint(dlg, L"Search failed: " + r.error);
            continue;
        }
        ctx.hits  = r.hits;
        ctx.count = r.count;
        const HWND list = GetDlgItem(dlg, IDC_RESULTS);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < ctx.count; ++i) {
            const GeoHit& h = ctx.hits[i];
            std::array<wchar_t, 96> where{};
            if (std::isnan(h.elevation)) {
                swprintf_s(where.data(), where.size(), L"   (%.2f, %.2f)", h.lat, h.lon);
            } else {
                swprintf_s(where.data(), where.size(), L"   (%.2f, %.2f, %.0f m)", h.lat, h.lon, h.elevation);
            }
            const std::wstring row = PlaceName(h) + where.data();
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
        }
        if (ctx.count > 0) { SendMessageW(list, LB_SETCURSEL, 0, 0); }
        SetHint(dlg, ctx.count == 0 ? L"No match. Try another spelling." : L"Pick one and press Add (or double-click).");
    }
}

void AddSelected(HWND dlg, AddContext& ctx) {
    const LRESULT sel = SendDlgItemMessageW(dlg, IDC_RESULTS, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || static_cast<size_t>(sel) >= ctx.count) {
        SetHint(dlg, L"Search, then pick a place from the list.");
        return;
    }
    const GeoHit& h = ctx.hits[static_cast<size_t>(sel)];
    Location loc;
    loc.name = PlaceName(h);
    loc.lat  = h.lat;
    loc.lon  = h.lon;
    std::wstring err;
    if (ctx.sink->AddPlace(loc, err)) {
        SetHint(dlg, L"Added " + loc.name + L". Downloading its history…");
    } else {
        SetHint(dlg, err);
    }
}

INT_PTR CALLBACK AddProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<AddContext*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        ctx = reinterpret_cast<AddContext*>(lp);
        SearchResult stale;
        while (ctx->fetcher->TakeSearch(stale)) {}   // left over from a closed dialog
        SendDlgItemMessageW(dlg, IDC_QUERY, EM_LIMITTEXT, 100, 0);
        SetFocus(GetDlgItem(dlg, IDC_QUERY));
        return FALSE;
    }
    case WM_APP_SEARCH:
        if (ctx != nullptr) { ShowResults(dlg, *ctx); }
        return TRUE;
    case WM_COMMAND: {
        if (ctx == nullptr) { return FALSE; }
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        if (id == IDC_SEARCH) { StartSearch(dlg, *ctx); return TRUE; }
        if (id == IDOK) {
            // Enter in the search box searches; anywhere else it adds.
            if (GetFocus() == GetDlgItem(dlg, IDC_QUERY)) { StartSearch(dlg, *ctx); } else { AddSelected(dlg, *ctx); }
            return TRUE;
        }
        if (id == IDC_RESULTS && code == LBN_DBLCLK) { AddSelected(dlg, *ctx); return TRUE; }
        if (id == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        return FALSE;
    }
    default:
        return FALSE;
    }
}

} // namespace

std::wstring PlaceName(const GeoHit& h) {
    std::wstring s = h.name;
    if (!h.admin1.empty() && h.admin1 != h.name) { s += L", " + h.admin1; }
    if (!h.country.empty()) { s += L", " + h.country; }
    for (wchar_t& c : s) {
        if (c == L'|') { c = L'/'; }
    }
    return s;
}

void ShowAddPlaceDialog(HINSTANCE inst, HWND owner, Fetcher& fetcher, PlaceSink& sink) {
    AddContext ctx;
    ctx.fetcher = &fetcher;
    ctx.sink    = &sink;
    const INT_PTR rc = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_ADDPLACE), owner, AddProc,
                                       reinterpret_cast<LPARAM>(&ctx));
    assert(rc != -1);
    (void)rc;
}

} // namespace wt

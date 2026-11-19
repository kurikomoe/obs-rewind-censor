#include "indicator.hpp"

#include <obs-module.h>
#include <obs.hpp>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/dstr.h>
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace
{
constexpr wchar_t window_class[] = L"5050.CensorStatus";
constexpr int panel_width = 386, panel_height = 303;
constexpr COLORREF ink = RGB(235, 240, 245), muted = RGB(160, 174, 188);
constexpr COLORREF green = RGB(105, 225, 167), amber = RGB(255, 196, 104), red = RGB(255, 142, 143);
constexpr char show_key_name[] = "5050-censor.show-indicator";
enum Field { marking, stream, picture, buffer, history, strength, image, hotkey, set_key, cancel_key, show_key, field_count };
constexpr const char *labels[] = {"Status.Marking", "Status.Stream",   "Status.Output", "Status.Buffer",
				  "Status.History", "Status.Strength", "Status.Image", "Toggle",
				  "SetMark", "CancelMark", "ShowIndicator"};

struct Row {
	std::string id;
	std::wstring name, image_name;
	PictureState picture;
	bool marking;
	int64_t buffered, delay, history, strength;
	obs_hotkey_id toggle, set, cancel;
};

std::wstring wide(const char *text)
{
	if (!text || !*text)
		return {};
	int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	if (!size)
		return {};
	std::wstring out(size, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
	out.pop_back();
	return out;
}

std::wstring tr(const char *key)
{
	return wide(obs_module_text(key));
}

void read_filter(obs_source_t *parent, obs_source_t *filter, void *data)
{
	const char *id = obs_source_get_unversioned_id(filter);
	if (!id || strcmp(id, "5050_censor_filter") != 0)
		return;
	calldata_t result{};
	if (proc_handler_call(obs_source_get_proc_handler(filter), "censor_status", &result)) {
		auto path = wide(calldata_string(&result, "image"));
		const auto slash = path.find_last_of(L"/\\");
		if (slash != std::wstring::npos)
			path.erase(0, slash + 1);
		static_cast<std::vector<Row> *>(data)->push_back(
		    {obs_source_get_uuid(filter),
		     wide(obs_source_get_name(parent)) + L"  /  " + wide(obs_source_get_name(filter)),
		     std::move(path), static_cast<PictureState>(calldata_int(&result, "picture")),
		     calldata_bool(&result, "marking"), calldata_int(&result, "buffered_ns"),
		     calldata_int(&result, "delay_ns"), calldata_int(&result, "history_ns"),
		     calldata_int(&result, "strength"),
		     static_cast<obs_hotkey_id>(calldata_int(&result, "key")),
		     static_cast<obs_hotkey_id>(calldata_int(&result, "set_key")),
		     static_cast<obs_hotkey_id>(calldata_int(&result, "cancel_key"))});
	}
	calldata_free(&result);
}

std::wstring seconds(int64_t ns)
{
	wchar_t text[32];
	swprintf_s(text, L"%.1f s", double(ns) / 1e9);
	return text;
}

struct Value {
	HWND label{}, value{};
	std::wstring text;
	COLORREF color = ink;
	bool visible = true;
};
struct Panel {
	HWND window{}, picker{}, gauge{};
	HBRUSH background{};
	HFONT regular{}, bold{};
	std::array<Value, field_count> fields{};
	std::vector<std::pair<std::string, std::wstring>> choices;
	bool registered = false, frontend = false;
	obs_hotkey_id show_key_id = OBS_INVALID_HOTKEY_ID;
} panel;

void set_field(Field index, const std::wstring &text, COLORREF color = ink)
{
	auto &field = panel.fields[index];
	if (field.text == text && field.color == color)
		return;
	field.text = text;
	field.color = color;
	SetWindowTextW(field.value, text.c_str());
	InvalidateRect(field.value, nullptr, TRUE);
}

void layout(HWND window);

void refresh_keys(const Row *row)
{
	const auto invalid = OBS_INVALID_HOTKEY_ID;
	struct Bindings {
		std::array<obs_hotkey_id, 4> ids;
		std::array<std::wstring, 4> names;
	} list{{row ? row->toggle : invalid, row ? row->set : invalid, row ? row->cancel : invalid, panel.show_key_id}, {}};
	obs_enum_hotkey_bindings([](void *data, size_t, obs_hotkey_binding_t *binding) {
		auto &list = *static_cast<Bindings *>(data);
		const auto it = std::find(list.ids.begin(), list.ids.end(), obs_hotkey_binding_get_hotkey_id(binding));
		if (it != list.ids.end()) {
			dstr name{};
			obs_key_combination_to_str(obs_hotkey_binding_get_key_combination(binding), &name);
			auto &text = list.names[it - list.ids.begin()];
			if (!text.empty()) text += L" / ";
			text += wide(name.array);
			dstr_free(&name);
		}
		return true;
	}, &list);
	bool resized = false;
	for (size_t i = 0; i < list.names.size(); ++i) {
		const auto field = static_cast<Field>(hotkey + i);
		const bool visible = field == hotkey || !list.names[i].empty();
		if (visible != panel.fields[field].visible) {
			panel.fields[field].visible = visible;
			resized = true;
		}
		if (field == hotkey && list.names[i].empty())
			list.names[i] = row ? tr("Status.Unbound") : L"—";
		set_field(field, list.names[i]);
	}
	if (resized && panel.window) layout(panel.window);
}

void refresh()
{
	std::vector<Row> rows;
	std::vector<OBSSource> sources;
	obs_enum_sources(
	    [](void *list, obs_source_t *source) {
		    static_cast<std::vector<OBSSource> *>(list)->emplace_back(source);
		    return true;
	    },
	    &sources);
	for (auto &source : sources)
		obs_source_enum_filters(source, read_filter, &rows);
	std::sort(rows.begin(), rows.end(),
		  [](const Row &a, const Row &b) { return a.name == b.name ? a.id < b.id : a.name < b.name; });
	std::vector<std::pair<std::string, std::wstring>> choices;
	for (const auto &row : rows)
		choices.emplace_back(row.id, row.name);
	int selected = int(SendMessageW(panel.picker, CB_GETCURSEL, 0, 0));
	if (choices != panel.choices || selected == CB_ERR) {
		std::string previous;
		if (selected >= 0 && size_t(selected) < panel.choices.size())
			previous = panel.choices[selected].first;
		SendMessageW(panel.picker, CB_RESETCONTENT, 0, 0);
		selected = 0;
		if (rows.empty())
			SendMessageW(panel.picker, CB_ADDSTRING, 0, LPARAM(tr("Status.Empty").c_str()));
		for (size_t i = 0; i < rows.size(); ++i) {
			SendMessageW(panel.picker, CB_ADDSTRING, 0, LPARAM(rows[i].name.c_str()));
			if (rows[i].id == previous)
				selected = int(i);
		}
		SendMessageW(panel.picker, CB_SETCURSEL, selected, 0);
		EnableWindow(panel.picker, !rows.empty());
		panel.choices = std::move(choices);
	}
	const bool live = obs_frontend_streaming_active();
	set_field(stream, tr(live ? "Status.Live" : "Status.Offline"), live ? green : muted);
	refresh_keys(rows.empty() ? nullptr : &rows[selected]);
	if (rows.empty()) {
		for (int i = 0; i < hotkey; ++i)
			if (i != stream)
				set_field(static_cast<Field>(i), L"—", muted);
		SendMessageW(panel.gauge, PBM_SETPOS, 0, 0);
		return;
	}
	const auto &r = rows[selected];
	set_field(marking, tr(r.marking ? "Status.On" : "Status.Off"), r.marking ? red : muted);
	constexpr const char *states[] = {"Status.Waiting", "Status.Warming", "Status.Clear", "Status.Mosaic",
					  "Status.Picture", "Status.Black",   "Status.Bypass"};
	const bool obscured = r.picture == PictureState::mosaic || r.picture == PictureState::image;
	set_field(picture, tr(states[static_cast<int>(r.picture)]),
		  obscured			     ? red
		  : r.picture == PictureState::clear ? green
						     : amber);
	set_field(buffer, seconds(r.buffered) + L" / " + seconds(r.delay));
	SendMessageW(panel.gauge, PBM_SETPOS,
		     r.delay > 0 ? std::min<int64_t>(1000, r.buffered * 1000 / r.delay) : 0, 0);
	set_field(history, seconds(r.history));
	set_field(strength, std::to_wstring(r.strength) + L" / 100");
	set_field(image, r.image_name.empty() ? tr("Status.None") : r.image_name);
}

void layout(HWND window)
{
	const auto dpi = GetDpiForWindow(window);
	auto px = [dpi](int v) { return MulDiv(v, dpi, 96); };
	auto place = [&](HWND h, int x, int y, int w, int height) {
		MoveWindow(h, px(x), px(y), px(w), px(height), TRUE);
	};
	auto font = [&](int weight, int size) {
		return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
				   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
				   L"Segoe UI");
	};
	auto old_regular = panel.regular, old_bold = panel.bold;
	panel.regular = font(FW_NORMAL, 14);
	panel.bold = font(FW_SEMIBOLD, 18);
	SendMessageW(panel.picker, WM_SETFONT, WPARAM(panel.regular), TRUE);
	place(panel.picker, 14, 10, panel_width - 28, 180);
	int visible_rows = 0;
	for (int i = 0; i < field_count; ++i) {
		auto &f = panel.fields[i];
		SendMessageW(f.label, WM_SETFONT, WPARAM(panel.regular), TRUE);
		SendMessageW(f.value, WM_SETFONT, WPARAM(i < 2 ? panel.bold : panel.regular), TRUE);
		ShowWindow(f.label, f.visible ? SW_SHOWNA : SW_HIDE);
		ShowWindow(f.value, f.visible ? SW_SHOWNA : SW_HIDE);
		if (!f.visible) continue;
		if (i < 2) {
			const int x = 14 + i * 182;
			place(f.label, x, 46, 175, 18);
			place(f.value, x, 65, 175, 27);
		} else {
			const int y = 103 + (visible_rows - 2) * 29 + (i > buffer ? 13 : 0);
			place(f.label, 14, y, 169, 23);
			place(f.value, 184, y, panel_width - 198, 23);
		}
		++visible_rows;
	}
	RECT bounds{0, 0, px(panel_width), px(panel_height + (visible_rows - hotkey - 1) * 29)};
	AdjustWindowRectExForDpi(&bounds, GetWindowLongW(window, GWL_STYLE), FALSE, GetWindowLongW(window, GWL_EXSTYLE), dpi);
	SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
		     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	place(panel.gauge, 14, 155, panel_width - 28, 5);
	DeleteObject(old_regular);
	DeleteObject(old_bold);
}

LRESULT CALLBACK window_proc(HWND h, UINT message, WPARAM w, LPARAM l)
{
	switch (message) {
	case WM_CREATE: {
		const auto module = GetModuleHandleW(nullptr);
		auto child = [&](const wchar_t *type, const wchar_t *title, DWORD style) {
			return CreateWindowExW(0, type, title, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, h,
					       nullptr, module, nullptr);
		};
		panel.picker = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL);
		panel.gauge = child(PROGRESS_CLASSW, L"", PBS_SMOOTH);
		SendMessageW(panel.gauge, PBM_SETRANGE32, 0, 1000);
		for (int i = 0; i < field_count; ++i) {
			panel.fields[i].visible = i <= hotkey;
			panel.fields[i].label = child(L"STATIC", tr(labels[i]).c_str(), SS_NOPREFIX);
			panel.fields[i].value = child(L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX);
		}
		layout(h);
		return 0;
	}
	case WM_SHOWWINDOW:
		if (w) {
			refresh();
			SetTimer(h, 1, 200, nullptr);
		} else
			KillTimer(h, 1);
		break;
	case WM_TIMER:
		refresh();
		return 0;
	case WM_COMMAND:
		if (HWND(l) == panel.picker && HIWORD(w) == CBN_SELCHANGE)
			refresh();
		break;
	case WM_CTLCOLORSTATIC: {
		auto dc = HDC(w);
		COLORREF color = muted;
		for (const auto &f : panel.fields)
			if (f.value == HWND(l))
				color = f.color;
		SetBkColor(dc, RGB(30, 35, 42));
		SetTextColor(dc, color);
		return LRESULT(panel.background);
	}
	case WM_DPICHANGED: {
		const RECT &r = *reinterpret_cast<RECT *>(l);
		SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
			     SWP_NOZORDER | SWP_NOACTIVATE);
		layout(h);
		return 0;
	}
	case WM_CLOSE:
		ShowWindow(h, SW_HIDE);
		return 0;
	case WM_DESTROY:
		KillTimer(h, 1);
		panel.window = nullptr;
		return 0;
	}
	return DefWindowProcW(h, message, w, l);
}

void show(void *)
{
	if (!panel.frontend)
		return;
	if (!panel.window) {
		const auto module = GetModuleHandleW(nullptr);
		if (!panel.registered) {
			INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
			InitCommonControlsEx(&controls);
			panel.background = CreateSolidBrush(RGB(30, 35, 42));
			WNDCLASSW wc{};
			wc.hInstance = module;
			wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
			wc.hbrBackground = panel.background;
			wc.lpfnWndProc = window_proc;
			wc.lpszClassName = window_class;
			panel.registered = RegisterClassW(&wc) != 0;
			if (!panel.registered) {
				DeleteObject(panel.background);
				panel.background = nullptr;
				blog(LOG_ERROR, "[5050 Censor] Status window registration failed");
				return;
			}
		}
		auto main = static_cast<HWND>(obs_frontend_get_main_window_handle());
		MONITORINFO monitor{sizeof(monitor)};
		GetMonitorInfoW(MonitorFromWindow(main, MONITOR_DEFAULTTOPRIMARY), &monitor);
		auto dpi = GetDpiForWindow(main);
		if (!dpi)
			dpi = 96;
		RECT rect{0, 0, MulDiv(panel_width, dpi, 96), MulDiv(panel_height, dpi, 96)};
		constexpr DWORD style = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
		constexpr DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
		AdjustWindowRectExForDpi(&rect, style, FALSE, ex, dpi);
		panel.window = CreateWindowExW(ex, window_class, tr("Status.Title").c_str(), style,
					       monitor.rcWork.right - (rect.right - rect.left) - 20,
					       monitor.rcWork.top + 20, rect.right - rect.left,
					       rect.bottom - rect.top, nullptr, nullptr, module, nullptr);
		if (!panel.window)
			blog(LOG_ERROR, "[5050 Censor] Could not open status window (%lu)", GetLastError());
	}
	if (panel.window)
		ShowWindow(panel.window, SW_SHOWNOACTIVATE);
}

void close_window()
{
	if (panel.show_key_id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(panel.show_key_id);
	if (panel.window)
		DestroyWindow(panel.window);
	if (panel.registered)
		UnregisterClassW(window_class, GetModuleHandleW(nullptr));
	DeleteObject(panel.regular);
	DeleteObject(panel.bold);
	// UnregisterClass releases the registered class's background brush.
	panel = {};
}

void frontend_event(obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_PROFILE_CHANGED) {
		// OBS saves frontend shortcuts in the active profile's Hotkeys section.
		auto *profile = obs_frontend_get_profile_config();
		const char *json = profile ? config_get_string(profile, "Hotkeys", show_key_name) : nullptr;
		auto *saved = json ? obs_data_create_from_json(json) : nullptr;
		auto *keys = saved ? obs_data_get_array(saved, "bindings") : nullptr;
		obs_hotkey_load(panel.show_key_id, keys);
		obs_data_array_release(keys);
		obs_data_release(saved);
	}
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		show(nullptr);
	if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		obs_frontend_remove_event_callback(frontend_event, nullptr);
		close_window();
	}
}
} // namespace

void indicator_load()
{
	if (!obs_frontend_get_main_window())
		return; // A headless libobs host has no frontend UI.
	panel.frontend = true;
	panel.show_key_id = obs_hotkey_register_frontend(show_key_name, obs_module_text("ShowIndicator"),
		[](void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) { if (pressed) indicator_show(); }, nullptr);
	obs_frontend_add_tools_menu_item(obs_module_text("Status.Title"), show, nullptr);
	obs_frontend_add_event_callback(frontend_event, nullptr);
}

void indicator_show()
{
	obs_queue_task(OBS_TASK_UI, show, nullptr, false);
}

void indicator_unload()
{
	if (panel.frontend) {
		obs_frontend_remove_event_callback(frontend_event, nullptr);
		close_window();
	}
}

#include "stdafx.h"
#pragma hdrstop

#include "UI_CommandPalette.h"
#include "UI_MainCommand.h"
#include "ui_main.h"

namespace
{
constexpr LPCSTR kPopupId = "Command Palette###xfined_command_palette";
constexpr int kNoMatch = 0x3fffffff;

struct SCatalogEntry
{
	CommandPalette::SResult result;
	xr_string search_label;
	xr_string search_path;
	xr_string search_name;
	xr_string search_shortcut;
};

struct SPaletteState
{
	bool open{};
	bool open_requested{};
	bool close_requested{};
	bool focus_search{};
	bool filter_dirty{};
	char query[256]{};
	xr_string filtered_query;
	xr_vector<CommandPalette::SResult> filtered;
	int selected{};
};

SPaletteState g_State;

xr_string LowerAscii(LPCSTR value)
{
	xr_string result = value ? value : "";
	for (char& character : result)
	{
		if (character >= 'A' && character <= 'Z')
			character = char(character - 'A' + 'a');
		else if (character == '\\')
			character = ' ';
	}
	return result;
}

xr_vector<xr_string> Tokenize(LPCSTR query)
{
	xr_vector<xr_string> tokens;
	const xr_string lower = LowerAscii(query);
	size_t begin = 0;
	while (begin < lower.size())
	{
		while (begin < lower.size() && (lower[begin] == ' ' || lower[begin] == '\t'))
			++begin;
		if (begin == lower.size())
			break;
		size_t end = begin;
		while (end < lower.size() && lower[end] != ' ' && lower[end] != '\t')
			++end;
		tokens.emplace_back(lower.substr(begin, end - begin));
		begin = end;
	}
	return tokens;
}

int ScoreToken(const xr_string& needle, const xr_string& haystack)
{
	if (needle.empty())
		return 0;
	if (needle == haystack)
		return int(haystack.size() - needle.size());
	if (haystack.compare(0, needle.size(), needle) == 0)
		return 20 + int(haystack.size() - needle.size());

	const size_t substring = haystack.find(needle);
	if (substring != xr_string::npos)
	{
		const bool word_boundary = substring == 0 || haystack[substring - 1] == ' ' ||
			haystack[substring - 1] == '_' || haystack[substring - 1] == '-';
		return (word_boundary ? 80 : 140) + int(substring);
	}

	size_t needle_index = 0;
	size_t first_match = xr_string::npos;
	size_t previous_match = 0;
	int gaps = 0;
	for (size_t index = 0; index < haystack.size() && needle_index < needle.size(); ++index)
	{
		if (haystack[index] != needle[needle_index])
			continue;
		if (first_match == xr_string::npos)
			first_match = index;
		else
			gaps += int(index - previous_match - 1);
		previous_match = index;
		++needle_index;
	}
	if (needle_index != needle.size())
		return kNoMatch;
	return 300 + int(first_match) + gaps * 4;
}

int ScoreEntry(const SCatalogEntry& entry, const xr_vector<xr_string>& tokens)
{
	int total = 0;
	for (const xr_string& token : tokens)
	{
		int token_score = ScoreToken(token, entry.search_label);
		token_score = std::min(token_score, ScoreToken(token, entry.search_path) + 10);
		token_score = std::min(token_score, ScoreToken(token, entry.search_name) + 20);
		token_score = std::min(token_score, ScoreToken(token, entry.search_shortcut) + 30);
		if (token_score >= kNoMatch)
			return kNoMatch;
		total += token_score;
	}
	return total;
}

xr_string FormatShortcut(const xr_shortcut& shortcut)
{
	if (!shortcut.key)
		return {};

	xr_string result;
	if (shortcut.ext.test(xr_shortcut::flCtrl))
		result += "Ctrl+";
	if (shortcut.ext.test(xr_shortcut::flShift))
		result += "Shift+";
	if (shortcut.ext.test(xr_shortcut::flAlt))
		result += "Alt+";

	if ((shortcut.key >= 'A' && shortcut.key <= 'Z') ||
		(shortcut.key >= '0' && shortcut.key <= '9'))
	{
		result += char(shortcut.key);
		return result;
	}
	if (shortcut.key >= VK_F1 && shortcut.key <= VK_F24)
	{
		char key_name[8]{};
		sprintf_s(key_name, "F%u", shortcut.key - VK_F1 + 1);
		result += key_name;
		return result;
	}

	const UINT scan_code = MapVirtualKeyW(shortcut.key, MAPVK_VK_TO_VSC);
	wchar_t wide_key_name[64]{};
	char key_name[_countof(wide_key_name) * 3 + 1]{};
	// The ANSI API follows the active Windows code page, while ImGui and MCP require UTF-8.
	const int key_name_length = scan_code ? GetKeyNameTextW(LONG(scan_code << 16), wide_key_name,
		_countof(wide_key_name)) : 0;
	const int utf8_length = key_name_length ? WideCharToMultiByte(CP_UTF8, 0, wide_key_name,
		key_name_length, key_name, int(sizeof(key_name) - 1), nullptr, nullptr) : 0;
	if (utf8_length)
	{
		key_name[utf8_length] = 0;
		result += key_name;
	}
	else
	{
		char fallback[16]{};
		sprintf_s(fallback, "Key %u", shortcut.key);
		result += fallback;
	}
	return result;
}

void SplitPath(LPCSTR command_description, LPCSTR sub_description, xr_string& category,
	xr_string& path, xr_string& label)
{
	path = command_description ? command_description : "";
	if (sub_description && sub_description[0])
	{
		if (!path.empty())
			path += "\\";
		path += sub_description;
	}

	const size_t first_separator = path.find('\\');
	const size_t last_separator = path.rfind('\\');
	category = first_separator == xr_string::npos ? "General" : path.substr(0, first_separator);
	label = last_separator == xr_string::npos ? path : path.substr(last_separator + 1);
	if (sub_description && sub_description[0] && first_separator == xr_string::npos)
		category = command_description;
}

xr_string DisplayPath(const xr_string& path)
{
	xr_string display = path;
	size_t separator = 0;
	while ((separator = display.find('\\', separator)) != xr_string::npos)
	{
		display.replace(separator, 1, " > ");
		separator += 3;
	}
	return display;
}

void BuildCatalog(xr_vector<SCatalogEntry>& catalog)
{
	catalog.clear();
	const ECommandVec& commands = GetEditorCommands();
	for (u32 command_index = 0; command_index < commands.size(); ++command_index)
	{
		SECommand* command = commands[command_index];
		if (!command || !command->Desc()[0] || command->command.empty())
			continue;

		for (u32 subcommand_index = 0; subcommand_index < command->sub_commands.size(); ++subcommand_index)
		{
			SESubCommand* subcommand = command->sub_commands[subcommand_index];
			if (!subcommand)
				continue;

			SCatalogEntry entry;
			entry.result.command = command->idx;
			entry.result.subcommand = subcommand_index;
			entry.result.name = command->Name();
			SplitPath(command->Desc(), subcommand->desc.c_str(), entry.result.category,
				entry.result.path, entry.search_label);
			entry.result.path = DisplayPath(entry.result.path);
			entry.result.shortcut = FormatShortcut(subcommand->shortcut);
			entry.result.id = command->Name();
			if (!subcommand->desc.empty())
			{
				entry.result.id += ".\"";
				entry.result.id += subcommand->desc;
				entry.result.id += "\"";
			}
			entry.search_label = LowerAscii(entry.search_label.c_str());
			entry.search_path = LowerAscii(entry.result.path.c_str());
			entry.search_name = LowerAscii(entry.result.name.c_str());
			entry.search_shortcut = LowerAscii(entry.result.shortcut.c_str());
			catalog.emplace_back(std::move(entry));
		}
	}
}

void RefreshFiltered()
{
	CommandPalette::Query(g_State.query, g_State.filtered, 250);
	g_State.filtered_query = g_State.query;
	if (g_State.filtered.empty())
		g_State.selected = 0;
	else
		g_State.selected = std::clamp(g_State.selected, 0, int(g_State.filtered.size()) - 1);
}
}

namespace CommandPalette
{
void Open(LPCSTR query)
{
	xr_strcpy(g_State.query, sizeof(g_State.query), query ? query : "");
	g_State.selected = 0;
	g_State.close_requested = false;
	g_State.open_requested = true;
	g_State.focus_search = true;
	g_State.filter_dirty = true;
}

void Close()
{
	g_State.open_requested = false;
	g_State.close_requested = true;
}

bool IsOpen()
{
	return !g_State.close_requested && (g_State.open || g_State.open_requested);
}

void Query(LPCSTR query, xr_vector<SResult>& results, u32 limit)
{
	xr_vector<SCatalogEntry> catalog;
	BuildCatalog(catalog);
	const xr_vector<xr_string> tokens = Tokenize(query);

	results.clear();
	results.reserve(std::min<size_t>(catalog.size(), limit));
	for (SCatalogEntry& entry : catalog)
	{
		entry.result.score = ScoreEntry(entry, tokens);
		if (entry.result.score < kNoMatch)
			results.emplace_back(std::move(entry.result));
	}

	std::stable_sort(results.begin(), results.end(), [](const SResult& left, const SResult& right)
	{
		if (left.score != right.score)
			return left.score < right.score;
		const int path_order = stricmp(left.path.c_str(), right.path.c_str());
		if (path_order)
			return path_order < 0;
		return stricmp(left.id.c_str(), right.id.c_str()) < 0;
	});
	if (results.size() > limit)
		results.resize(limit);
}

bool Execute(u32 command_index, u32 subcommand_index)
{
	ECommandVec& commands = GetEditorCommands();
	if (command_index >= commands.size())
		return false;
	SECommand* command = commands[command_index];
	if (!command || command->command.empty() || subcommand_index >= command->sub_commands.size())
		return false;
	SESubCommand* subcommand = command->sub_commands[subcommand_index];
	if (!subcommand)
		return false;
	ExecCommand(command->idx, subcommand->p0, subcommand->p1);
	return true;
}

bool Execute(LPCSTR id)
{
	if (!id || !id[0])
		return false;
	xr_vector<SCatalogEntry> catalog;
	BuildCatalog(catalog);
	const auto found = std::find_if(catalog.begin(), catalog.end(), [id](const SCatalogEntry& entry)
	{
		return 0 == stricmp(entry.result.id.c_str(), id);
	});
	return found != catalog.end() && Execute(found->result.command, found->result.subcommand);
}

void Draw()
{
	if (g_State.close_requested && !g_State.open)
	{
		g_State.close_requested = false;
		return;
	}
	if (g_State.open_requested)
	{
		g_State.open_requested = false;
		g_State.open = true;
		g_State.filter_dirty = true;
		ImGui::OpenPopup(kPopupId);
	}
	if (!g_State.open)
		return;

	UI->BlockShortCuts();
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImVec2 size(std::min(920.0f, viewport->WorkSize.x * 0.72f),
		std::min(620.0f, viewport->WorkSize.y * 0.72f));
	const ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
		viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
	ImGui::SetNextWindowPos(center,
		ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);

	bool keep_open = true;
	if (!ImGui::BeginPopupModal(kPopupId, &keep_open, ImGuiWindowFlags_NoSavedSettings))
	{
		if (!ImGui::IsPopupOpen(kPopupId))
			g_State.open = false;
		return;
	}

	if (g_State.focus_search)
	{
		ImGui::SetKeyboardFocusHere();
		g_State.focus_search = false;
	}
	ImGui::SetNextItemWidth(-50.0f);
	const bool submitted = ImGui::InputTextWithHint("##command_palette_query", "Search commands...",
		g_State.query, sizeof(g_State.query), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	if (g_State.filter_dirty || g_State.filtered_query != g_State.query)
	{
		if (g_State.filtered_query != g_State.query)
			g_State.selected = 0;
		RefreshFiltered();
		g_State.filter_dirty = false;
	}

	if (!g_State.filtered.empty())
	{
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
			g_State.selected = (g_State.selected + 1) % int(g_State.filtered.size());
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
			g_State.selected = (g_State.selected + int(g_State.filtered.size()) - 1) % int(g_State.filtered.size());
	}

	ImGui::SameLine();
	ImGui::TextDisabled("%u", u32(g_State.filtered.size()));
	ImGui::Separator();

	int execute_index = submitted && !g_State.filtered.empty() ? g_State.selected : -1;
	if (g_State.filtered.empty())
		ImGui::TextDisabled("No matching commands");
	else if (ImGui::BeginTable("##command_palette_results", 4,
		ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
		ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp,
		ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing())))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 125.0f);
		ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed, 110.0f);
		ImGui::TableHeadersRow();

		for (int index = 0; index < int(g_State.filtered.size()); ++index)
		{
			const SResult& result = g_State.filtered[index];
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::PushID(index);
			if (ImGui::Selectable(result.category.c_str(), index == g_State.selected,
				ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
			{
				g_State.selected = index;
				execute_index = index;
			}
			ImGui::PopID();
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(result.path.c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::TextDisabled("%s", result.name.c_str());
			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted(result.shortcut.empty() ? "-" : result.shortcut.c_str());
			if (index == g_State.selected && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
				ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
				ImGui::SetScrollHereY(0.5f);
		}
		ImGui::EndTable();
	}

	const bool cancel = g_State.close_requested || ImGui::IsKeyPressed(ImGuiKey_Escape);
	if (cancel || !keep_open || execute_index >= 0)
	{
		ImGui::CloseCurrentPopup();
		g_State.open = false;
		g_State.close_requested = false;
	}
	ImGui::EndPopup();

	if (!cancel && keep_open && execute_index >= 0)
	{
		const SResult selected = g_State.filtered[execute_index];
		Execute(selected.command, selected.subcommand);
	}
}
}

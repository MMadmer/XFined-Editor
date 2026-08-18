#include "stdafx.h"
#include "ELog.h"
#include "UILogForm.h"
#include "..\XrCore\os_clipboard.h"
#include "..\XrEngine\XR_IOConsole.h"
#define MSG_ERROR 	0x00C4C4FF
#define MSG_INFO  	0x00E6FFE7
#define MSG_CONF 	0x00FFE6E7
#define MSG_DEF  	0x00E8E8E8
bool UILogForm::bAutoScroll = true;
string_path UILogForm::m_Filter="";
string_path UILogForm::m_Exec="";
xr_vector<xr_string>* UILogForm::List = nullptr;
xr_vector<xr_string> UILogForm::VisibleList;
xrCriticalSection UILogForm::ListGuard;
size_t UILogForm::CachedSourceCount = 0;
string_path UILogForm::CachedFilter = "";
extern bool bAllowLogCommands;
void UILogForm::AddMessage( const xr_string& msg)
{
	xr_string M;
	M.reserve(msg.size());
	for (const char ch : msg)
	{
		if (ch == '\r')
			continue;
		M += ch == '\n' ? ' ' : ch;
	}

	xrCriticalSection::raii guard(&ListGuard);
	GetList()->push_back(std::move(M));
}


void UILogForm::Show()
{
	bAllowLogCommands = true;
}

void UILogForm::Hide()
{
	bAllowLogCommands = false;
}

void UILogForm::Update()
{
	static bool FirstRun = false;
	if (bAllowLogCommands)
	{
		bool NeedCopy = false;
		if (!ImGui::Begin("Log", &bAllowLogCommands))
		{
			ImGui::End();
			return;
		}
		if (ImGui::Button("Clear")) 
		{
			xrCriticalSection::raii guard(&ListGuard);
			GetList()->clear();
			VisibleList.clear();
			CachedSourceCount = 0;
		}ImGui::SameLine();
		if (ImGui::Button("Flush")) 
		{
			FlushLog();
		}ImGui::SameLine();
		if (ImGui::Button("Copy"))
		{
			NeedCopy = true;
		}ImGui::SameLine();
		ImGui::Checkbox("Auto Scroll", &bAutoScroll); 
		ImGui::SameLine();
		ImGui::InputText("Filter", m_Filter, sizeof(m_Filter));;

		// Consume only appended lines; changing the filter rebuilds the visible cache.
		{
			xrCriticalSection::raii guard(&ListGuard);
			xr_vector<xr_string>* source = GetList();
			if (0 != xr_strcmp(CachedFilter, m_Filter) || CachedSourceCount > source->size())
			{
				VisibleList.clear();
				CachedSourceCount = 0;
				xr_strcpy(CachedFilter, m_Filter);
			}

			for (; CachedSourceCount < source->size(); ++CachedSourceCount)
			{
				const xr_string& line = source->at(CachedSourceCount);
				if (!m_Filter[0] || strstr(line.c_str(), m_Filter))
					VisibleList.push_back(line);
			}
		}

		ImGui::Spacing();
		if (ImGui::BeginChild("Log",ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),true))
		{
			const bool WasAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();
			xr_string CopyLog;
			if (NeedCopy)
			{
				for (const xr_string& line : VisibleList)
					CopyLog.append(line).append("\r\n");
				os_clipboard::copy_to_clipboard(CopyLog.c_str());
			}

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(VisibleList.size()));
			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
				{
					const char* line = VisibleList[i].c_str();
					ImVec4 color = { 1, 1, 1, 1 };
					if (0 == strncmp(line, "! ", 2))
						color = { 1, 0, 0, 1 };
					else if (0 == strncmp(line, "~ ", 2))
						color = { 1, 1, 0, 1 };
					else if (0 == strncmp(line, "* ", 2))
						color = { 0.5f, 0.5f, 0.5f, 1 };

					ImGui::PushStyleColor(ImGuiCol_Text, color);
					ImGui::TextUnformatted(line);
					ImGui::PopStyleColor();
				}
			}

			if ((bAutoScroll && WasAtBottom) || !FirstRun)
				ImGui::SetScrollHereY(1.f);

			FirstRun = true;
		}
		ImGui::EndChild();
		ImGuiInputTextFlags input_text_flags = ImGuiInputTextFlags_EnterReturnsTrue ;
		if (ImGui::InputText("Exec", m_Exec, IM_ARRAYSIZE(m_Exec), input_text_flags))
		{
			if (m_Exec[0])
			{
				Msg("~ Exec %s", m_Exec);
				Console->Execute(m_Exec);
			}
		
		}
		ImGui::End();
	}
	else
	{
		FirstRun = false;
	}
}

void UILogForm::Destroy()
{
	xrCriticalSection::raii guard(&ListGuard);
	xr_delete(List);
	VisibleList.clear();
	CachedSourceCount = 0;
	CachedFilter[0] = 0;
}

xr_vector<xr_string>* UILogForm::GetList()
{
	if (!List)List = xr_new<xr_vector<xr_string>>();
	return List;
}

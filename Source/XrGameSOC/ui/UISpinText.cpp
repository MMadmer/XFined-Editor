#include "StdAfx.h"
#include "UISpinText.h"
#include "UILines.h"
#include "../string_table.h"

CUISpinText::CUISpinText(){
    m_curItem = -1;
}

void CUISpinText::AddItem_(const char* item, int id)
{
	SInfo			_info;
	_info._orig		= item;
	_info._transl	= CStringTable().translate(item);
	_info._id		= id;

	m_list.push_back( _info );
	if (-1 == m_curItem)
	{
		m_curItem		= 0;
		SetItem			();
	}
}

void CUISpinText::SetItem()
{
	if (m_curItem < 0 || m_curItem >= static_cast<int>(m_list.size()))
	{
		m_pLines->SetText("");
		return;
	}
	m_pLines->SetText	(m_list[m_curItem]._transl.c_str());
}

LPCSTR CUISpinText::GetTokenText()
{
	if (m_curItem < 0 || m_curItem >= static_cast<int>(m_list.size()))
		return "";
	return				m_list[m_curItem]._orig.c_str();
}

void CUISpinText::SetCurrentValue(){
	m_list.clear();
	m_curItem = -1;
	xr_token* tok = GetOptToken();
	if (!tok)
	{
		m_pLines->SetText("");
		return;
	}

	while (tok->name){
		AddItem_(tok->name, tok->id);
		tok++;
	}
	if (m_list.empty())
	{
		m_pLines->SetText("");
		return;
	}

	LPCSTR current_value = GetOptTokenValue();
	if (!current_value || !current_value[0])
	{
		m_curItem = -1;
		m_pLines->SetText("");
		return;
	}
	xr_string val = current_value;

	for (u32 i = 0; i < m_list.size(); i++)
		if (val == m_list[i]._orig.c_str())
		{
			m_curItem	= i;
			break;
		}

	SetItem();
}

void CUISpinText::SaveValue()
{
	if (m_curItem < 0 || m_curItem >= static_cast<int>(m_list.size()))
		return;
	xr_token* tokens = GetOptToken();
	if (!tokens)
		return;
	LPCSTR live_value = get_token_name(tokens, m_list[m_curItem]._id);
	if (!live_value || xr_strcmp(live_value, m_list[m_curItem]._orig.c_str()) != 0)
		return;
	CUIOptionsItem::SaveValue		();
	SaveOptTokenValue				(m_list[m_curItem]._orig.c_str());
}

bool CUISpinText::IsChanged()
{
	if (m_curItem < 0 || m_curItem >= static_cast<int>(m_list.size()))
		return false;
	LPCSTR current_value = GetOptTokenValue();
	return current_value && xr_strcmp(current_value, m_list[m_curItem]._orig.c_str()) != 0;
}

void CUISpinText::OnBtnUpClick()
{
	if (CanPressUp())
	{
		m_curItem		++;
		SetItem			();
	}

	CUICustomSpin::OnBtnUpClick();
}

void CUISpinText::OnBtnDownClick()
{
	if (CanPressDown())
	{
		m_curItem--;
		SetItem		();
	}

	CUICustomSpin::OnBtnDownClick();
}

bool CUISpinText::CanPressUp()
{
	return m_curItem < (int)m_list.size() - 1;
}

bool CUISpinText::CanPressDown()
{
	return m_curItem > 0;
}

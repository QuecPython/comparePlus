/*
 * This file is part of Compare plugin for Notepad++
 * Copyright (C)2011 Jean-Sebastien Leroy (jean.sebastien.leroy@gmail.com)
 * Copyright (C)2017 Pavel Nedev (pg.nedev@gmail.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdlib>
#include <vector>
#include <memory>

#include <windows.h>
#include <tchar.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <commdlg.h>

#include "Tools.h"
#include "Compare.h"
#include "UserSettings.h"
#include "NppHelpers.h"
#include "LibHelpers.h"
#include "AboutDialog.h"
#include "SettingsDialog.h"
#include "NavDialog.h"
#include "Engine.h"
#include "NppInternalDefines.h"
#include "resource.h"


const TCHAR PLUGIN_NAME[] = TEXT("Compare");

NppData		nppData;
SciFnDirect	sciFunc;
sptr_t		sciPtr[2];

#ifdef DLOG

std::string		dLog("Compare Plugin debug log\n\n");
DWORD			dLogTime_ms = 0;
static LRESULT	dLogBuf = -1;

#endif


namespace // anonymous namespace
{

/**
 *  \class
 *  \brief
 */
class NppSettings
{
public:
	static NppSettings& get()
	{
		static NppSettings instance;
		return instance;
	}

	void enableClearCommands(bool enable) const;
	void enableNppScrollCommands(bool enable) const;
	void updatePluginMenu();
	void save();
	void setNormalMode(bool forceUpdate = false);
	void setCompareMode(bool clearHorizontalScroll = false);

	bool	compareMode;

private:
	NppSettings() : compareMode(false), _restoreMultilineTab(false) {}

	void toSingleLineTab();
	void restoreMultilineTab();
	void refreshTabBar(HWND hTabBar);
	void refreshTabBars();

	bool	_restoreMultilineTab;

	bool	_syncVScroll;
	bool	_syncHScroll;
};


/**
 *  \struct
 *  \brief
 */
struct DeletedSection
{
	DeletedSection(int action, int line, int len) : startLine(line), lineReplace(false)
	{
		restoreAction = (action == SC_PERFORMED_UNDO) ? SC_PERFORMED_REDO : SC_PERFORMED_UNDO;

		markers.resize(len, 0);
	}

	int					startLine;
	bool				lineReplace;
	int					restoreAction;
	std::vector<int>	markers;
};


/**
 *  \struct
 *  \brief
 */
struct DeletedSectionsList
{
	DeletedSectionsList() : skipPush(0), lastPushTimeMark(0) {}

	void push(int currAction, int startLine, int endLine);
	void pop(int currAction, int startLine);

	void clear()
	{
		skipPush = 0;
		sections.clear();
	}

private:
	int							skipPush;
	DWORD						lastPushTimeMark;
	std::vector<DeletedSection>	sections;
};


void DeletedSectionsList::push(int currAction, int startLine, int endLine)
{
	if (endLine <= startLine)
		return;

	if (skipPush)
	{
		--skipPush;
		return;
	}

	// Is it line replacement revert operation?
	if (!sections.empty() && sections.back().restoreAction == currAction && sections.back().lineReplace)
		return;

	DeletedSection delSection(currAction, startLine, endLine - startLine + 1);

	const int currentView = getCurrentViewId();

	const int startPos = CallScintilla(currentView, SCI_POSITIONFROMLINE, startLine, 0);
	clearChangedIndicator(currentView,
			startPos, CallScintilla(currentView, SCI_POSITIONFROMLINE, endLine, 0) - startPos);

	for (int line = CallScintilla(currentView, SCI_MARKERPREVIOUS, endLine, MARKER_MASK_LINE);
			line >= startLine; line = CallScintilla(currentView, SCI_MARKERPREVIOUS, line - 1, MARKER_MASK_LINE))
	{
		delSection.markers[line - startLine] = CallScintilla(currentView, SCI_MARKERGET, line, 0) & MARKER_MASK_ALL;
		if (line != endLine)
			clearMarks(currentView, line);
	}

	sections.push_back(delSection);

	lastPushTimeMark = ::GetTickCount();
}


void DeletedSectionsList::pop(int currAction, int startLine)
{
	if (sections.empty())
	{
		++skipPush;
		return;
	}

	DeletedSection& last = sections.back();

	if (last.restoreAction != currAction)
	{
		// Try to guess if this is the insert part of line replacement operation
		if (::GetTickCount() < lastPushTimeMark + 40)
			last.lineReplace = true;
		else
			++skipPush;

		return;
	}

	if (last.startLine != startLine)
		return;

	const int currentView = getCurrentViewId();

	const int linesCount = static_cast<int>(last.markers.size());

	const int startPos = CallScintilla(currentView, SCI_POSITIONFROMLINE, last.startLine, 0);
	clearChangedIndicator(currentView,
			startPos, CallScintilla(currentView, SCI_POSITIONFROMLINE, last.startLine + linesCount, 0) - startPos);

	for (int i = 0; i < linesCount; ++i)
	{
		clearMarks(currentView, last.startLine + i);

		if (last.markers[i])
			CallScintilla(currentView, SCI_MARKERADDSET, last.startLine + i, last.markers[i]);
	}

	sections.pop_back();
}


enum Temp_t
{
	NO_TEMP = 0,
	LAST_SAVED_TEMP,
	SVN_TEMP,
	GIT_TEMP
};


/**
 *  \class
 *  \brief
 */
class ComparedFile
{
public:
	ComparedFile() : isTemp(NO_TEMP) {}

	void initFromCurrent(bool currFileIsNew);
	void updateFromCurrent();
	void updateView();
	void clear();
	void clear(const section_t& section);
	void onBeforeClose() const;
	void onClose() const;
	void close() const;
	void restore() const;
	bool isOpen() const;

	Temp_t	isTemp;
	bool	isNew;

	int		originalViewId;
	int		originalPos;
	int		compareViewId;

	LRESULT	buffId;
	int		sciDoc;
	TCHAR	name[MAX_PATH];

	DeletedSectionsList deletedSections;
};


/**
 *  \class
 *  \brief
 */
class ComparedPair
{
public:
	inline ComparedFile& getFileByViewId(int viewId);
	inline ComparedFile& getFileByBuffId(LRESULT buffId);
	inline ComparedFile& getOtherFileByBuffId(LRESULT buffId);
	inline ComparedFile& getFileBySciDoc(int sciDoc);
	inline ComparedFile& getOldFile();
	inline ComparedFile& getNewFile();

	void positionFiles();
	void restoreFiles(int currentBuffId);

	void setStatus();

	ComparedFile	file[2];
	int				relativePos;

	bool			isFullCompare;
	bool			spacesIgnored;
	bool			caseIgnored;
	bool			movesDetected;

	std::pair<int, int>	selections[2];

	AlignmentInfo_t	alignmentInfo;
};


/**
 *  \class
 *  \brief
 */
class NewCompare
{
public:
	NewCompare(bool currFileIsNew, bool markFirstName);
	~NewCompare();

	ComparedPair	pair;

private:
	TCHAR	_firstTabText[64];
};


using CompareList_t = std::vector<ComparedPair>;


/**
 *  \class
 *  \brief
 */
class DelayedAlign : public DelayedWork
{
public:
	DelayedAlign() : DelayedWork() {}
	virtual ~DelayedAlign() = default;

	virtual void operator()();
};


/**
 *  \class
 *  \brief
 */
class DelayedActivate : public DelayedWork
{
public:
	DelayedActivate() : DelayedWork() {}
	virtual ~DelayedActivate() = default;

	virtual void operator()();

	inline void operator()(LRESULT buff)
	{
		buffId = buff;
		operator()();
	}

	LRESULT buffId;
};


/**
 *  \class
 *  \brief
 */
class DelayedClose : public DelayedWork
{
public:
	DelayedClose() : DelayedWork() {}
	virtual ~DelayedClose() = default;

	virtual void operator()();

	std::vector<LRESULT> closedBuffs;
};


/**
 *  \class
 *  \brief
 */
class DelayedMaximize : public DelayedWork
{
public:
	DelayedMaximize() : DelayedWork() {}
	virtual ~DelayedMaximize() = default;

	virtual void operator()();
};


/**
 *  \class
 *  \brief
 */
class DelayedUpdate : public DelayedWork
{
public:
	DelayedUpdate() : DelayedWork(), linesAdded(0), linesDeleted(0), fullCompare(false) {}
	virtual ~DelayedUpdate() = default;

	virtual void operator()();

	int		changePos;
	int		linesAdded;
	int		linesDeleted;

	bool	fullCompare;
};


/**
 *  \struct
 *  \brief
 */
struct TempMark_t
{
	const TCHAR*	fileMark;
	const TCHAR*	tabMark;
};


static const TempMark_t tempMark[] =
{
	{ TEXT(""),				TEXT("") },
	{ TEXT("_LastSave"),	TEXT(" ** Last Save") },
	{ TEXT("_SVN"),			TEXT(" ** SVN") },
	{ TEXT("_Git"),			TEXT(" ** Git") }
};


UserSettings Settings;

CompareList_t compareList;
std::unique_ptr<NewCompare> newCompare;

volatile unsigned notificationsLock = 0;

std::unique_ptr<ViewLocation> storedLocation;
bool goToFirst = false;

DelayedAlign	delayedAlignment;
DelayedActivate	delayedActivation;
DelayedClose	delayedClosure;
DelayedUpdate	delayedUpdate;
DelayedMaximize	delayedMaximize;

AboutDialog   	AboutDlg;
SettingsDialog	SettingsDlg;
NavDialog     	NavDlg;

toolbarIcons  tbSetFirst;
toolbarIcons  tbCompare;
toolbarIcons  tbCompareLines;
toolbarIcons  tbClearCompare;
toolbarIcons  tbFirst;
toolbarIcons  tbPrev;
toolbarIcons  tbNext;
toolbarIcons  tbLast;
toolbarIcons  tbNavBar;

HINSTANCE hInstance;
FuncItem funcItem[NB_MENU_COMMANDS] = { 0 };

// Declare local functions that appear before they are defined
void onBufferActivated(LRESULT buffId);
void syncViews(int biasView);


void NppSettings::enableClearCommands(bool enable) const
{
	HMENU hMenu = (HMENU)::SendMessage(nppData._nppHandle, NPPM_GETMENUHANDLE, NPPPLUGINMENU, 0);

	::EnableMenuItem(hMenu, funcItem[CMD_CLEAR_ACTIVE]._cmdID,
			MF_BYCOMMAND | ((!enable && !compareMode) ? (MF_DISABLED | MF_GRAYED) : MF_ENABLED));

	::EnableMenuItem(hMenu, funcItem[CMD_CLEAR_ALL]._cmdID,
			MF_BYCOMMAND | ((!enable && compareList.empty()) ? (MF_DISABLED | MF_GRAYED) : MF_ENABLED));

	::DrawMenuBar(nppData._nppHandle);

	HWND hNppToolbar = NppToolbarHandleGetter::get();
	if (hNppToolbar)
		::SendMessage(hNppToolbar, TB_ENABLEBUTTON, funcItem[CMD_CLEAR_ACTIVE]._cmdID, enable || compareMode);
}


void NppSettings::enableNppScrollCommands(bool enable) const
{
	HMENU hMenu = (HMENU)::SendMessage(nppData._nppHandle, NPPM_GETMENUHANDLE, NPPMAINMENU, 0);
	const int flag = MF_BYCOMMAND | (enable ? MF_ENABLED : (MF_DISABLED | MF_GRAYED));

	::EnableMenuItem(hMenu, IDM_VIEW_SYNSCROLLH, flag);
	::EnableMenuItem(hMenu, IDM_VIEW_SYNSCROLLV, flag);

	::DrawMenuBar(nppData._nppHandle);

	HWND hNppToolbar = NppToolbarHandleGetter::get();
	if (hNppToolbar)
	{
		::SendMessage(hNppToolbar, TB_ENABLEBUTTON, IDM_VIEW_SYNSCROLLH, enable);
		::SendMessage(hNppToolbar, TB_ENABLEBUTTON, IDM_VIEW_SYNSCROLLV, enable);
	}
}


void NppSettings::updatePluginMenu()
{
	HMENU hMenu = (HMENU)::SendMessage(nppData._nppHandle, NPPM_GETMENUHANDLE, NPPPLUGINMENU, 0);
	const int flag = MF_BYCOMMAND | (compareMode ? MF_ENABLED : (MF_DISABLED | MF_GRAYED));

	::EnableMenuItem(hMenu, funcItem[CMD_CLEAR_ACTIVE]._cmdID,
			MF_BYCOMMAND | ((!compareMode && !newCompare) ? (MF_DISABLED | MF_GRAYED) : MF_ENABLED));

	::EnableMenuItem(hMenu, funcItem[CMD_CLEAR_ALL]._cmdID,
			MF_BYCOMMAND | ((compareList.empty() && !newCompare) ? (MF_DISABLED | MF_GRAYED) : MF_ENABLED));

	::EnableMenuItem(hMenu, funcItem[CMD_FIRST]._cmdID, flag);
	::EnableMenuItem(hMenu, funcItem[CMD_PREV]._cmdID, flag);
	::EnableMenuItem(hMenu, funcItem[CMD_NEXT]._cmdID, flag);
	::EnableMenuItem(hMenu, funcItem[CMD_LAST]._cmdID, flag);

	::DrawMenuBar(nppData._nppHandle);

	HWND hNppToolbar = NppToolbarHandleGetter::get();
	if (hNppToolbar)
	{
		::SendMessage(hNppToolbar, TB_ENABLEBUTTON, funcItem[CMD_CLEAR_ACTIVE]._cmdID, compareMode || newCompare);
		::SendMessage(hNppToolbar, TB_ENABLEBUTTON, funcItem[CMD_FIRST]._cmdID, compareMode);
		::SendMessage(hNppToolbar, TB_ENABLEBUTTON, funcItem[CMD_PREV]._cmdID, compareMode);
		::SendMessage(hNppToolbar, TB_ENABLEBUTTON, funcItem[CMD_NEXT]._cmdID, compareMode);
		::SendMessage(hNppToolbar, TB_ENABLEBUTTON, funcItem[CMD_LAST]._cmdID, compareMode);
	}
}


void NppSettings::save()
{
	HMENU hMenu = (HMENU)::SendMessage(nppData._nppHandle, NPPM_GETMENUHANDLE, NPPMAINMENU, 0);

	_syncVScroll = (::GetMenuState(hMenu, IDM_VIEW_SYNSCROLLV, MF_BYCOMMAND) & MF_CHECKED) != 0;
	_syncHScroll = (::GetMenuState(hMenu, IDM_VIEW_SYNSCROLLH, MF_BYCOMMAND) & MF_CHECKED) != 0;
}


void NppSettings::setNormalMode(bool forceUpdate)
{
	if (compareMode)
	{
		compareMode = false;

		restoreMultilineTab();

		if (NavDlg.isVisible())
			NavDlg.Hide();

		if (!isSingleView())
		{
			enableNppScrollCommands(true);

			HMENU hMenu = (HMENU)::SendMessage(nppData._nppHandle, NPPM_GETMENUHANDLE, NPPMAINMENU, 0);

			bool syncScroll = (::GetMenuState(hMenu, IDM_VIEW_SYNSCROLLV, MF_BYCOMMAND) & MF_CHECKED) != 0;
			if (syncScroll != _syncVScroll)
				::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_VIEW_SYNSCROLLV);

			syncScroll = (::GetMenuState(hMenu, IDM_VIEW_SYNSCROLLH, MF_BYCOMMAND) & MF_CHECKED) != 0;
			if (syncScroll != _syncHScroll)
				::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_VIEW_SYNSCROLLH);
		}

		updatePluginMenu();
	}
	else if (forceUpdate)
	{
		restoreMultilineTab();

		updatePluginMenu();
	}
}


void NppSettings::setCompareMode(bool clearHorizontalScroll)
{
	if (compareMode == true)
		return;

	compareMode = true;

	save();

	toSingleLineTab();

	if (clearHorizontalScroll)
	{
		int pos = CallScintilla(MAIN_VIEW, SCI_POSITIONFROMLINE, getCurrentLine(MAIN_VIEW), 0);
		CallScintilla(MAIN_VIEW, SCI_SETSEL, pos, pos);

		pos = CallScintilla(SUB_VIEW, SCI_POSITIONFROMLINE, getCurrentLine(SUB_VIEW), 0);
		CallScintilla(SUB_VIEW, SCI_SETSEL, pos, pos);
	}

	// Disable N++ vertical scroll - we handle it manually because of the Word Wrap
	if (_syncVScroll)
		::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_VIEW_SYNSCROLLV);

	// Yaron - Enable N++ horizontal scroll sync
	if (!_syncHScroll)
		::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_VIEW_SYNSCROLLH);

	// synchronize zoom levels
	int zoom = CallScintilla(getCurrentViewId(), SCI_GETZOOM, 0, 0);
	CallScintilla(getOtherViewId(), SCI_SETZOOM, zoom, 0);

	enableNppScrollCommands(false);
	updatePluginMenu();
}


void NppSettings::refreshTabBar(HWND hTabBar)
{
	if (::IsWindowVisible(hTabBar) && (TabCtrl_GetItemCount(hTabBar) > 1))
		{
		const int currentTabIdx = TabCtrl_GetCurSel(hTabBar);

		TabCtrl_SetCurFocus(hTabBar, (currentTabIdx) ? 0 : 1);
		TabCtrl_SetCurFocus(hTabBar, currentTabIdx);
	}
}


void NppSettings::refreshTabBars()
{
	HWND currentView = getCurrentView();

	HWND hNppTabBar = NppTabHandleGetter::get(SUB_VIEW);

	if (hNppTabBar)
		refreshTabBar(hNppTabBar);

	hNppTabBar = NppTabHandleGetter::get(MAIN_VIEW);

	if (hNppTabBar)
		refreshTabBar(hNppTabBar);

	::SetFocus(currentView);
}


void NppSettings::toSingleLineTab()
{
	if (!_restoreMultilineTab)
	{
		HWND hNppMainTabBar = NppTabHandleGetter::get(MAIN_VIEW);
		HWND hNppSubTabBar = NppTabHandleGetter::get(SUB_VIEW);

		if (hNppMainTabBar && hNppSubTabBar)
		{
			RECT tabRec;
			::GetWindowRect(hNppMainTabBar, &tabRec);
			const int mainTabYPos = tabRec.top;

			::GetWindowRect(hNppSubTabBar, &tabRec);
			const int subTabYPos = tabRec.top;

			// Both views are side-by-side positioned
			if (mainTabYPos == subTabYPos)
			{
				LONG_PTR tabStyle = ::GetWindowLongPtr(hNppMainTabBar, GWL_STYLE);

				if ((tabStyle & TCS_MULTILINE) && !(tabStyle & TCS_VERTICAL))
				{
					::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, TRUE);

					::SetWindowLongPtr(hNppMainTabBar, GWL_STYLE, tabStyle & ~TCS_MULTILINE);
					::SendMessage(hNppMainTabBar, WM_TABSETSTYLE, 0, 0);

					tabStyle = ::GetWindowLongPtr(hNppSubTabBar, GWL_STYLE);
					::SetWindowLongPtr(hNppSubTabBar, GWL_STYLE, tabStyle & ~TCS_MULTILINE);
					::SendMessage(hNppSubTabBar, WM_TABSETSTYLE, 0, 0);

					::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, FALSE);

					// Scroll current tab into view
					refreshTabBars();

					_restoreMultilineTab = true;
				}
			}
		}
	}
}


void NppSettings::restoreMultilineTab()
{
	if (_restoreMultilineTab)
	{
		_restoreMultilineTab = false;

		HWND hNppMainTabBar = NppTabHandleGetter::get(MAIN_VIEW);
		HWND hNppSubTabBar = NppTabHandleGetter::get(SUB_VIEW);

		if (hNppMainTabBar && hNppSubTabBar)
		{
			LONG_PTR tabStyle = ::GetWindowLongPtr(hNppMainTabBar, GWL_STYLE);

			::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, TRUE);

			::SetWindowLongPtr(hNppMainTabBar, GWL_STYLE, tabStyle | TCS_MULTILINE);
			::SendMessage(hNppMainTabBar, WM_TABSETSTYLE, 0, 0);

			tabStyle = ::GetWindowLongPtr(hNppSubTabBar, GWL_STYLE);
			::SetWindowLongPtr(hNppSubTabBar, GWL_STYLE, tabStyle | TCS_MULTILINE);
			::SendMessage(hNppSubTabBar, WM_TABSETSTYLE, 0, 0);

			::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, FALSE);
		}
	}
}


void ComparedFile::initFromCurrent(bool currFileIsNew)
{
	isNew = currFileIsNew;
	buffId = getCurrentBuffId();
	originalViewId = getCurrentViewId();
	compareViewId = originalViewId;
	originalPos = posFromBuffId(buffId);
	::SendMessage(nppData._nppHandle, NPPM_GETFULLCURRENTPATH, _countof(name), (LPARAM)name);

	updateFromCurrent();
}


void ComparedFile::updateFromCurrent()
{
	sciDoc = getDocId(getCurrentViewId());

	if (isTemp)
	{
		HWND hNppTabBar = NppTabHandleGetter::get(getCurrentViewId());

		if (hNppTabBar)
		{
			const TCHAR* fileExt = ::PathFindExtension(name);

			TCHAR tabName[MAX_PATH];

			_tcscpy_s(tabName, _countof(tabName), ::PathFindFileName(name));
			::PathRemoveExtension(tabName);

			int i = _tcslen(tabName) - 1 - _tcslen(tempMark[isTemp].fileMark);
			for (; i > 0 && tabName[i] != TEXT('_'); --i);

			if (i > 0)
			{
				tabName[i] = 0;
				_tcscat_s(tabName, _countof(tabName), fileExt);
				_tcscat_s(tabName, _countof(tabName), tempMark[isTemp].tabMark);

				TCITEM tab;
				tab.mask = TCIF_TEXT;
				tab.pszText = tabName;

				::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, TRUE);

				TabCtrl_SetItem(hNppTabBar, posFromBuffId(buffId), &tab);

				::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, FALSE);
			}
		}
	}
}


void ComparedFile::updateView()
{
	compareViewId = isNew ? ((Settings.OldFileViewId == MAIN_VIEW) ? SUB_VIEW : MAIN_VIEW) : Settings.OldFileViewId;
}


void ComparedFile::clear()
{
	clearWindow(viewIdFromBuffId(buffId));

	deletedSections.clear();
}


void ComparedFile::clear(const section_t& section)
{
	clearMarksAndBlanks(viewIdFromBuffId(buffId), section.off, section.len);

	deletedSections.clear();
}


void ComparedFile::onBeforeClose() const
{
	activateBufferID(buffId);

	const int view = getCurrentViewId();
	clearWindow(view);

	if (isTemp)
		CallScintilla(view, SCI_SETSAVEPOINT, 0, 0);
}


void ComparedFile::onClose() const
{
	if (isTemp)
	{
		::SetFileAttributes(name, FILE_ATTRIBUTE_NORMAL);
		::DeleteFile(name);
	}
}


void ComparedFile::close() const
{
	onBeforeClose();

	::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);

	onClose();
}


void ComparedFile::restore() const
{
	if (isTemp)
	{
		close();
		return;
	}

	activateBufferID(buffId);

	clearWindow(getCurrentViewId());

	if (viewIdFromBuffId(buffId) != originalViewId)
	{
		::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_VIEW_GOTO_ANOTHER_VIEW);

		if (!isOpen())
			return;

		const int currentPos = posFromBuffId(buffId);

		if (originalPos >= currentPos)
			return;

		for (int i = currentPos - originalPos; i; --i)
			::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_VIEW_TAB_MOVEBACKWARD);
	}
}


bool ComparedFile::isOpen() const
{
	return (::SendMessage(nppData._nppHandle, NPPM_GETFULLPATHFROMBUFFERID, buffId, (LPARAM)NULL) >= 0);
}


ComparedFile& ComparedPair::getFileByViewId(int viewId)
{
	return (viewIdFromBuffId(file[0].buffId) == viewId) ? file[0] : file[1];
}


ComparedFile& ComparedPair::getFileByBuffId(LRESULT buffId)
{
	return (file[0].buffId == buffId) ? file[0] : file[1];
}


ComparedFile& ComparedPair::getOtherFileByBuffId(LRESULT buffId)
{
	return (file[0].buffId == buffId) ? file[1] : file[0];
}


ComparedFile& ComparedPair::getFileBySciDoc(int sciDoc)
{
	return (file[0].sciDoc == sciDoc) ? file[0] : file[1];
}


ComparedFile& ComparedPair::getOldFile()
{
	return file[0].isNew ? file[1] : file[0];
}


ComparedFile& ComparedPair::getNewFile()
{
	return file[0].isNew ? file[0] : file[1];
}


void ComparedPair::positionFiles()
{
	// sync both views zoom
	const int zoom = CallScintilla(getCurrentViewId(), SCI_GETZOOM, 0, 0);
	CallScintilla(getOtherViewId(), SCI_SETZOOM, zoom, 0);

	const LRESULT currentBuffId = getCurrentBuffId();

	ComparedFile& oldFile = getOldFile();
	ComparedFile& newFile = getNewFile();

	oldFile.updateView();
	newFile.updateView();

	relativePos = (oldFile.originalViewId != newFile.originalViewId) ? 0 :
			(oldFile.originalViewId == oldFile.compareViewId) ?
			newFile.originalPos - oldFile.originalPos : oldFile.originalPos - newFile.originalPos;

	if (viewIdFromBuffId(oldFile.buffId) != oldFile.compareViewId)
	{
		activateBufferID(oldFile.buffId);
		::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_VIEW_GOTO_ANOTHER_VIEW);
		oldFile.updateFromCurrent();
	}

	if (viewIdFromBuffId(newFile.buffId) != newFile.compareViewId)
	{
		activateBufferID(newFile.buffId);
		::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_VIEW_GOTO_ANOTHER_VIEW);
		newFile.updateFromCurrent();
	}

	if (oldFile.sciDoc != getDocId(oldFile.compareViewId))
		activateBufferID(oldFile.buffId);

	if (newFile.sciDoc != getDocId(newFile.compareViewId))
		activateBufferID(newFile.buffId);

	activateBufferID(currentBuffId);
}


void ComparedPair::restoreFiles(int currentBuffId = -1)
{
	// Check if position update is needed -
	// this is for relative re-positioning to keep files initial order consistent
	if (relativePos)
	{
		ComparedFile* biasFile;
		ComparedFile* movedFile;

		// One of the files is in its original view and won't be moved - this is the bias file
		if (viewIdFromBuffId(file[0].buffId) == file[0].originalViewId)
		{
			biasFile = &file[0];
			movedFile = &file[1];
		}
		else
		{
			biasFile = &file[1];
			movedFile = &file[0];
		}

		if (biasFile->originalPos > movedFile->originalPos)
		{
			const int newPos = posFromBuffId(biasFile->buffId);

			if (newPos != biasFile->originalPos && newPos < movedFile->originalPos)
				movedFile->originalPos = newPos;
		}
	}

	if (currentBuffId == -1)
	{
		file[0].restore();
		file[1].restore();
	}
	else
	{
		getOtherFileByBuffId(currentBuffId).restore();
		getFileByBuffId(currentBuffId).restore();
	}
}


void ComparedPair::setStatus()
{
	TCHAR cmpType[128];

	if (isFullCompare)
		_tcscpy_s(cmpType, _countof(cmpType), TEXT("Full"));
	else
		_sntprintf_s(cmpType, _countof(cmpType), _TRUNCATE, TEXT("Sel: %d-%d vs. %d-%d"),
				selections[MAIN_VIEW].first + 1, selections[MAIN_VIEW].second + 1,
				selections[SUB_VIEW].first + 1, selections[SUB_VIEW].second + 1);

	TCHAR msg[512];

	_sntprintf_s(msg, _countof(msg), _TRUNCATE,
			TEXT("Compare (%s)    Ignore Spaces (%s)    Ignore Case (%s)    Detect Moves (%s)"), cmpType,
			spacesIgnored	? TEXT("Y")	: TEXT("N"),
			caseIgnored		? TEXT("Y")	: TEXT("N"),
			movesDetected	? TEXT("Y")	: TEXT("N"));

	::SendMessageW(nppData._nppHandle, NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE, static_cast<LPARAM>((LONG_PTR)msg));
}


NewCompare::NewCompare(bool currFileIsNew, bool markFirstName)
{
	_firstTabText[0] = 0;

	pair.file[0].initFromCurrent(currFileIsNew);

	// Enable commands to be able to clear the first file that was just set
	NppSettings::get().enableClearCommands(true);

	if (markFirstName)
	{
		HWND hNppTabBar = NppTabHandleGetter::get(pair.file[0].originalViewId);

		if (hNppTabBar)
		{
			TCITEM tab;
			tab.mask = TCIF_TEXT;
			tab.pszText = _firstTabText;
			tab.cchTextMax = _countof(_firstTabText);

			TabCtrl_GetItem(hNppTabBar, pair.file[0].originalPos, &tab);

			TCHAR tabText[MAX_PATH];
			tab.pszText = tabText;

			_sntprintf_s(tabText, _countof(tabText), _TRUNCATE, TEXT("%s ** %s to Compare"),
					_firstTabText, Settings.OldFileIsFirst ? TEXT("Old") : TEXT("New"));

			::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, TRUE);

			TabCtrl_SetItem(hNppTabBar, pair.file[0].originalPos, &tab);

			::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, FALSE);
		}
	}
}


NewCompare::~NewCompare()
{
	if (_firstTabText[0] != 0)
	{
		HWND hNppTabBar = NppTabHandleGetter::get(pair.file[0].originalViewId);

		if (hNppTabBar)
		{
			// This is workaround for Wine issue with tab bar refresh
			::InvalidateRect(hNppTabBar, NULL, FALSE);

			TCITEM tab;
			tab.mask = TCIF_TEXT;
			tab.pszText = _firstTabText;

			::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, TRUE);

			TabCtrl_SetItem(hNppTabBar, posFromBuffId(pair.file[0].buffId), &tab);

			::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, FALSE);
		}
	}

	if (!NppSettings::get().compareMode)
		NppSettings::get().enableClearCommands(false);
}


CompareList_t::iterator getCompare(LRESULT buffId)
{
	for (CompareList_t::iterator it = compareList.begin(); it < compareList.end(); ++it)
	{
		if (it->file[0].buffId == buffId || it->file[1].buffId == buffId)
			return it;
	}

	return compareList.end();
}


CompareList_t::iterator getCompareBySciDoc(int sciDoc)
{
	for (CompareList_t::iterator it = compareList.begin(); it < compareList.end(); ++it)
	{
		if (it->file[0].sciDoc == sciDoc || it->file[1].sciDoc == sciDoc)
			return it;
	}

	return compareList.end();
}


void resetCompareView(int view)
{
	if (!::IsWindowVisible(getView(view)))
		return;

	CompareList_t::iterator cmpPair = getCompareBySciDoc(getDocId(view));
	if (cmpPair != compareList.end())
		setCompareView(view, Settings.colors.blank);
}


bool isAlignmentNeeded(int view, const AlignmentInfo_t& alignmentInfo)
{
	const AlignmentViewData AlignmentPair::*pView = (view == MAIN_VIEW) ? &AlignmentPair::main : &AlignmentPair::sub;

	int firstLine = CallScintilla(view, SCI_GETFIRSTVISIBLELINE, 0, 0);
	int lastLine = firstLine + CallScintilla(view, SCI_LINESONSCREEN, 0, 0);

	firstLine = CallScintilla(view, SCI_DOCLINEFROMVISIBLE, firstLine, 0);
	lastLine = CallScintilla(view, SCI_DOCLINEFROMVISIBLE, lastLine, 0);

	bool realign = false;

	for (const auto& alignment : alignmentInfo)
	{
		if ((alignment.*pView).line >= firstLine)
		{
			if ((alignment.main.diffMask == alignment.sub.diffMask) &&
				(CallScintilla(MAIN_VIEW, SCI_VISIBLEFROMDOCLINE, alignment.main.line, 0) !=
				CallScintilla(SUB_VIEW, SCI_VISIBLEFROMDOCLINE, alignment.sub.line, 0)))
			{
				realign = true;
				break;
			}
		}

		if ((alignment.*pView).line > lastLine)
			break;
	}

	return realign;
}


void alignDiffs(const AlignmentInfo_t& alignmentInfo)
{
	CallScintilla(MAIN_VIEW, SCI_FOLDALL, SC_FOLDACTION_EXPAND, 0);
	CallScintilla(SUB_VIEW, SCI_FOLDALL, SC_FOLDACTION_EXPAND, 0);

	const int mainEndLine = CallScintilla(MAIN_VIEW, SCI_GETLINECOUNT, 0, 0) - 1;
	const int subEndLine = CallScintilla(SUB_VIEW, SCI_GETLINECOUNT, 0, 0) - 1;

	const int maxSize = static_cast<int>(alignmentInfo.size());

	// Align diffs
	for (int i = 0; i < maxSize &&
			alignmentInfo[i].main.line <= mainEndLine && alignmentInfo[i].sub.line <= subEndLine; ++i)
	{
		if (alignmentInfo[i].main.line &&
				CallScintilla(MAIN_VIEW, SCI_ANNOTATIONGETLINES, alignmentInfo[i].main.line - 1, 0))
			CallScintilla(MAIN_VIEW, SCI_ANNOTATIONSETTEXT, alignmentInfo[i].main.line - 1, (LPARAM)NULL);

		if (alignmentInfo[i].sub.line &&
				CallScintilla(SUB_VIEW, SCI_ANNOTATIONGETLINES, alignmentInfo[i].sub.line - 1, 0))
			CallScintilla(SUB_VIEW, SCI_ANNOTATIONSETTEXT, alignmentInfo[i].sub.line - 1, (LPARAM)NULL);

		const int mismatchLen =
				CallScintilla(MAIN_VIEW, SCI_VISIBLEFROMDOCLINE, alignmentInfo[i].main.line, 0) -
				CallScintilla(SUB_VIEW, SCI_VISIBLEFROMDOCLINE, alignmentInfo[i].sub.line, 0);

		if (mismatchLen > 0)
		{
			if ((i + 1 < maxSize) && (alignmentInfo[i].sub.line == alignmentInfo[i + 1].sub.line))
				continue;

			addBlankSection(SUB_VIEW, alignmentInfo[i].sub.line, mismatchLen);
		}
		else if (mismatchLen < 0)
		{
			if ((i + 1 < maxSize) && (alignmentInfo[i].main.line == alignmentInfo[i + 1].main.line))
				continue;

			addBlankSection(MAIN_VIEW, alignmentInfo[i].main.line, -mismatchLen);
		}
	}
}


void showNavBar()
{
	NavDlg.SetColors(Settings.colors);
	NavDlg.Show();
}


bool isFileCompared(int view)
{
	const int sciDoc = getDocId(view);

	CompareList_t::iterator cmpPair = getCompareBySciDoc(sciDoc);
	if (cmpPair != compareList.end())
	{
		const TCHAR* fname = ::PathFindFileName(cmpPair->getFileBySciDoc(sciDoc).name);

		TCHAR msg[MAX_PATH];
		_sntprintf_s(msg, _countof(msg), _TRUNCATE,
				TEXT("File \"%s\" is already compared - operation ignored."), fname);
		::MessageBox(nppData._nppHandle, msg, PLUGIN_NAME, MB_OK);

		return true;
	}

	return false;
}


bool isEncodingOK(const ComparedPair& cmpPair)
{
	// Warn about encoding mismatches as that might compromise the compare
	if (getEncoding(cmpPair.file[0].buffId) != getEncoding(cmpPair.file[1].buffId))
	{
		if (::MessageBox(nppData._nppHandle,
			TEXT("Trying to compare files with different encodings - \n")
			TEXT("the result might be inaccurate and misleading.\n\n")
			TEXT("Compare anyway?"), PLUGIN_NAME, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
		{
			return false;
		}
	}

	return true;
}


// Call it with no arguments when re-comparing (the files are active in both views)
bool areSelectionsValid(LRESULT currentBuffId = -1, LRESULT otherBuffId = -1)
{
	int view1 = (currentBuffId == otherBuffId) ? MAIN_VIEW : viewIdFromBuffId(currentBuffId);
	int view2 = (currentBuffId == otherBuffId) ? SUB_VIEW : viewIdFromBuffId(otherBuffId);

	if (view1 == view2)
		activateBufferID(otherBuffId);

	std::pair<int, int> viewSel = getSelectionLines(view2);
	bool valid = !(viewSel.first < 0);

	if (view1 == view2)
		activateBufferID(currentBuffId);

	if (valid)
	{
		viewSel = getSelectionLines(view1);
		valid = !(viewSel.first < 0);
	}

	if (!valid)
		::MessageBox(nppData._nppHandle, TEXT("No selected lines to compare - operation ignored."),
				PLUGIN_NAME, MB_OK);

	return valid;
}


bool setFirst(bool currFileIsNew, bool markName = false)
{
	if (isFileCompared(getCurrentViewId()))
		return false;

	// Done on purpose: First wipe the std::unique_ptr so ~NewCompare is called before the new object constructor.
	// This is important because the N++ plugin menu is updated on NewCompare construct/destruct.
	newCompare.reset();
	newCompare.reset(new NewCompare(currFileIsNew, markName));

	return true;
}


void setContent(const char* content)
{
	const int view = getCurrentViewId();

	ScopedViewUndoCollectionBlocker undoBlock(view);
	ScopedViewWriteEnabler writeEn(view);

	CallScintilla(view, SCI_SETTEXT, 0, (LPARAM)content);
	CallScintilla(view, SCI_SETSAVEPOINT, 0, 0);
}


bool checkFileExists(const TCHAR *file)
{
	if (::PathFileExists(file) == FALSE)
	{
		::MessageBox(nppData._nppHandle, TEXT("File is not written to disk - operation ignored."),
				PLUGIN_NAME, MB_OK);
		return false;
	}

	return true;
}


bool createTempFile(const TCHAR *file, Temp_t tempType)
{
	if (!setFirst(true))
		return false;

	TCHAR tempFile[MAX_PATH];

	if (::GetTempPath(_countof(tempFile), tempFile))
	{
		const TCHAR* fileName	= ::PathFindFileName(newCompare->pair.file[0].name);
		const TCHAR* fileExt	= ::PathFindExtension(newCompare->pair.file[0].name);

		if (::PathAppend(tempFile, fileName))
		{
			::PathRemoveExtension(tempFile);

			_tcscat_s(tempFile, _countof(tempFile), tempMark[tempType].fileMark);

			unsigned idxPos = _tcslen(tempFile);

			// Make sure temp file is unique
			for (int i = 1; ; ++i)
			{
				TCHAR idx[32];

				_itot_s(i, idx, _countof(idx), 10);

				if (_tcslen(idx) + idxPos + 1 > _countof(tempFile))
				{
					idxPos = _countof(tempFile);
					break;
				}

				_tcscat_s(tempFile, _countof(tempFile), idx);
				_tcscat_s(tempFile, _countof(tempFile), fileExt);

				if (!::PathFileExists(tempFile))
					break;

				tempFile[idxPos] = 0;
			}

			if ((idxPos + 1 <= _countof(tempFile)) && ::CopyFile(file, tempFile, TRUE))
			{
				::SetFileAttributes(tempFile, FILE_ATTRIBUTE_TEMPORARY);

				const int langType = ::SendMessage(nppData._nppHandle, NPPM_GETBUFFERLANGTYPE,
						newCompare->pair.file[0].buffId, 0);

				ScopedIncrementer incr(notificationsLock);

				if (::SendMessage(nppData._nppHandle, NPPM_DOOPEN, 0, (LPARAM)tempFile))
				{
					const LRESULT buffId = getCurrentBuffId();

					::SendMessage(nppData._nppHandle, NPPM_SETBUFFERLANGTYPE, buffId, langType);
					::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_EDIT_SETREADONLY);

					newCompare->pair.file[1].isTemp = tempType;

					return true;
				}
			}
		}
	}

	::MessageBox(nppData._nppHandle, TEXT("Creating temp file failed - operation aborted."), PLUGIN_NAME, MB_OK);

	newCompare.reset();

	return false;
}


void clearComparePair(LRESULT buffId)
{
	CompareList_t::iterator cmpPair = getCompare(buffId);
	if (cmpPair == compareList.end())
		return;

	ScopedIncrementer incr(notificationsLock);

	cmpPair->restoreFiles(buffId);

	compareList.erase(cmpPair);

	onBufferActivated(getCurrentBuffId());
}


void closeComparePair(CompareList_t::iterator cmpPair)
{
	HWND currentView = getCurrentView();

	ScopedIncrementer incr(notificationsLock);

	// First close the file in the SUB_VIEW as closing a file may lead to a single view mode
	// and if that happens we want to be in single main view
	cmpPair->getFileByViewId(SUB_VIEW).close();
	cmpPair->getFileByViewId(MAIN_VIEW).close();

	compareList.erase(cmpPair);

	if (::IsWindowVisible(currentView))
		::SetFocus(currentView);

	onBufferActivated(getCurrentBuffId());
}


bool initNewCompare()
{
	bool firstIsSet = (bool)newCompare;

	// Compare to self?
	if (firstIsSet && (newCompare->pair.file[0].buffId == getCurrentBuffId()))
		firstIsSet = false;

	if (!firstIsSet)
	{
		const bool singleView = isSingleView();
		const bool isNew = singleView ? true : getCurrentViewId() != Settings.OldFileViewId;

		if (!setFirst(isNew))
			return false;

		if (singleView)
		{
			if (getNumberOfFiles(getCurrentViewId()) < 2)
			{
				::MessageBox(nppData._nppHandle, TEXT("Only one file opened - operation ignored."),
						PLUGIN_NAME, MB_OK);
				return false;
			}

			::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0,
					Settings.CompareToPrev ? IDM_VIEW_TAB_PREV : IDM_VIEW_TAB_NEXT);
		}
		else
		{
			// Check if the file in the other view is compared already
			if (isFileCompared(getOtherViewId()))
				return false;

			::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_VIEW_SWITCHTO_OTHER_VIEW);
		}
	}

	newCompare->pair.file[1].initFromCurrent(!newCompare->pair.file[0].isNew);

	return true;
}


CompareList_t::iterator addComparePair()
{
	compareList.push_back(newCompare->pair);
	newCompare.reset();

	return compareList.end() - 1;
}


CompareResult runCompare(CompareList_t::iterator cmpPair, bool selectionCompare)
{
	cmpPair->positionFiles();

	section_t mainViewSection = { 0, 0 };
	section_t subViewSection = { 0, 0 };

	if (selectionCompare)
	{
		cmpPair->selections[MAIN_VIEW]	= getSelectionLines(MAIN_VIEW);
		cmpPair->selections[SUB_VIEW]	= getSelectionLines(SUB_VIEW);

		mainViewSection.off = cmpPair->selections[MAIN_VIEW].first;
		mainViewSection.len = cmpPair->selections[MAIN_VIEW].second - cmpPair->selections[MAIN_VIEW].first + 1;

		subViewSection.off = cmpPair->selections[SUB_VIEW].first;
		subViewSection.len = cmpPair->selections[SUB_VIEW].second - cmpPair->selections[SUB_VIEW].first + 1;
	}

	setStyles(Settings);

	const TCHAR* newName = ::PathFindFileName(cmpPair->getNewFile().name);
	const TCHAR* oldName = ::PathFindFileName(cmpPair->getOldFile().name);

	TCHAR progressInfo[MAX_PATH];
	_sntprintf_s(progressInfo, _countof(progressInfo), _TRUNCATE, selectionCompare ?
			TEXT("Comparing selected lines in \"%s\" vs. selected lines in \"%s\"...") :
			TEXT("Comparing \"%s\" vs. \"%s\"..."), newName, oldName);

	return compareViews(mainViewSection, subViewSection, Settings, progressInfo, cmpPair->alignmentInfo);
}


void compare(bool selectionCompare = false)
{
	ScopedIncrementer incr(notificationsLock);

	const bool				doubleView		= !isSingleView();
	const LRESULT			currentBuffId	= getCurrentBuffId();
	CompareList_t::iterator	cmpPair			= getCompare(currentBuffId);
	const bool				recompare		= (cmpPair != compareList.end());

	// Just to be sure any old state is cleared
	storedLocation.reset();
	goToFirst = false;

	if (recompare)
	{
		newCompare.reset();

		if (selectionCompare && !areSelectionsValid())
			return;

		if (cmpPair->isFullCompare && !Settings.GotoFirstDiff && !selectionCompare)
			storedLocation.reset(new ViewLocation(getCurrentViewId()));

		cmpPair->getOldFile().clear();
		cmpPair->getNewFile().clear();
	}
	// New compare
	else
	{
		if (!initNewCompare())
		{
			newCompare.reset();
			return;
		}

		cmpPair = addComparePair();

		if (cmpPair->getOldFile().isTemp)
		{
			activateBufferID(cmpPair->getNewFile().buffId);
		}
		else
		{
			activateBufferID(currentBuffId);

			if (selectionCompare &&
				!areSelectionsValid(currentBuffId, cmpPair->getOtherFileByBuffId(currentBuffId).buffId))
			{
				compareList.erase(cmpPair);
				return;
			}
		}

		if (Settings.EncodingsCheck && !isEncodingOK(*cmpPair))
		{
			clearComparePair(getCurrentBuffId());
			return;
		}
	}

	const CompareResult cmpResult = runCompare(cmpPair, selectionCompare);

	switch (cmpResult)
	{
		case CompareResult::COMPARE_MISMATCH:
		{
			cmpPair->isFullCompare	= !selectionCompare;
			cmpPair->spacesIgnored	= Settings.IgnoreSpaces;
			cmpPair->caseIgnored	= Settings.IgnoreCase;
			cmpPair->movesDetected	= Settings.DetectMoves;

			if (Settings.UseNavBar)
				showNavBar();

			NppSettings::get().setCompareMode(true);

			setCompareView(MAIN_VIEW, Settings.colors.blank);
			setCompareView(SUB_VIEW, Settings.colors.blank);

			if (!storedLocation)
			{
				if (!doubleView)
					activateBufferID(cmpPair->getNewFile().buffId);

				if (selectionCompare)
				{
					clearSelection(getCurrentViewId());
					clearSelection(getOtherViewId());
				}

				goToFirst = true;

				for (const AlignmentPair& alignment : cmpPair->alignmentInfo)
				{
					if (alignment.main.diffMask)
					{
						centerAt(MAIN_VIEW,	alignment.main.line);
						centerAt(SUB_VIEW,	alignment.sub.line);
						break;
					}
				}
			}

			LOGD("COMPARE READY\n");
		}
		return;

		case CompareResult::COMPARE_MATCH:
		{
			const ComparedFile& oldFile = cmpPair->getOldFile();

			const TCHAR* newName = ::PathFindFileName(cmpPair->getNewFile().name);

			TCHAR msg[2 * MAX_PATH];

			int choice = IDNO;

			if (oldFile.isTemp)
			{
				if (recompare)
				{
					_sntprintf_s(msg, _countof(msg), _TRUNCATE,
							TEXT("%s \"%s\" and \"%s\" match.\n\nTemp file will be closed."), selectionCompare ?
							TEXT("Selected lines in files") : TEXT("Files"),
							newName, ::PathFindFileName(oldFile.name));
				}
				else
				{
					if (oldFile.isTemp == LAST_SAVED_TEMP)
						_sntprintf_s(msg, _countof(msg), _TRUNCATE,
								TEXT("File \"%s\" has not been modified since last Save."), newName);
					else
						_sntprintf_s(msg, _countof(msg), _TRUNCATE,
								TEXT("File \"%s\" has no changes against %s."), newName,
								oldFile.isTemp == GIT_TEMP ? TEXT("Git") : TEXT("SVN"));
				}

				::MessageBox(nppData._nppHandle, msg, PLUGIN_NAME, MB_OK);
			}
			else
			{
				_sntprintf_s(msg, _countof(msg), _TRUNCATE,
						TEXT("%s \"%s\" and \"%s\" match.%s"),
						selectionCompare ? TEXT("Selected lines in files") : TEXT("Files"),
						newName, ::PathFindFileName(oldFile.name),
						Settings.PromptToCloseOnMatch ? TEXT("\n\nClose compared files?") : TEXT(""));

				if (Settings.PromptToCloseOnMatch)
					choice = ::MessageBox(nppData._nppHandle, msg, PLUGIN_NAME,
							MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
				else
					::MessageBox(nppData._nppHandle, msg, PLUGIN_NAME, MB_OK);
			}

			if (choice == IDYES)
				closeComparePair(cmpPair);
			else
				clearComparePair(getCurrentBuffId());
		}
		break;

		default:
			clearComparePair(getCurrentBuffId());
	}

	storedLocation.reset();
}


void SetAsFirst()
{
	if (!setFirst(!Settings.OldFileIsFirst, true))
		newCompare.reset();
}


void CompareWhole()
{
	compare();
}


void CompareSelectedLines()
{
	compare(true);
}


void ClearActiveCompare()
{
	newCompare.reset();

	if (NppSettings::get().compareMode)
		clearComparePair(getCurrentBuffId());
}


void ClearAllCompares()
{
	newCompare.reset();

	if (!compareList.size())
		return;

	const LRESULT buffId = getCurrentBuffId();

	ScopedIncrementer incr(notificationsLock);

	::SetFocus(getOtherView());

	const LRESULT otherBuffId = getCurrentBuffId();

	for (int i = static_cast<int>(compareList.size()) - 1; i >= 0; --i)
		compareList[i].restoreFiles();

	compareList.clear();

	NppSettings::get().setNormalMode(true);

	if (!isSingleView())
		activateBufferID(otherBuffId);

	activateBufferID(buffId);
}


void LastSaveDiff()
{
	TCHAR file[MAX_PATH];

	::SendMessage(nppData._nppHandle, NPPM_GETFULLCURRENTPATH, _countof(file), (LPARAM)file);

	if (!checkFileExists(file))
		return;

	if (createTempFile(file, LAST_SAVED_TEMP))
		compare();
}


void SvnDiff()
{
	TCHAR file[MAX_PATH];
	TCHAR svnFile[MAX_PATH];

	::SendMessage(nppData._nppHandle, NPPM_GETFULLCURRENTPATH, _countof(file), (LPARAM)file);

	if (!checkFileExists(file))
		return;

	if (!GetSvnFile(file, svnFile, _countof(svnFile)))
		return;

	if (createTempFile(svnFile, SVN_TEMP))
		compare();
}


void GitDiff()
{
	TCHAR file[MAX_PATH];

	::SendMessage(nppData._nppHandle, NPPM_GETFULLCURRENTPATH, _countof(file), (LPARAM)file);

	if (!checkFileExists(file))
		return;

	std::vector<char> content = GetGitFileContent(file);

	if (content.empty())
		return;

	if (!createTempFile(file, GIT_TEMP))
		return;

	setContent(content.data());
	content.clear();

	compare();
}


void IgnoreSpaces()
{
	Settings.IgnoreSpaces = !Settings.IgnoreSpaces;
	::SendMessage(nppData._nppHandle, NPPM_SETMENUITEMCHECK, funcItem[CMD_IGNORE_SPACES]._cmdID,
			(LPARAM)Settings.IgnoreSpaces);
	Settings.markAsDirty();
}


void IgnoreCase()
{
	Settings.IgnoreCase = !Settings.IgnoreCase;
	::SendMessage(nppData._nppHandle, NPPM_SETMENUITEMCHECK, funcItem[CMD_IGNORE_CASE]._cmdID,
			(LPARAM)Settings.IgnoreCase);
	Settings.markAsDirty();
}


void DetectMoves()
{
	Settings.DetectMoves = !Settings.DetectMoves;
	::SendMessage(nppData._nppHandle, NPPM_SETMENUITEMCHECK, funcItem[CMD_DETECT_MOVES]._cmdID,
			(LPARAM)Settings.DetectMoves);
	Settings.markAsDirty();
}


void Prev()
{
	if (NppSettings::get().compareMode)
		jumpToChange(false, Settings.WrapAround);
}


void Next()
{
	if (NppSettings::get().compareMode)
		jumpToChange(true, Settings.WrapAround);
}


void First()
{
	if (NppSettings::get().compareMode)
		jumpToFirstChange();
}


void Last()
{
	if (NppSettings::get().compareMode)
		jumpToLastChange();
}


void OpenSettingsDlg(void)
{
	if (SettingsDlg.doDialog(&Settings) == IDOK)
	{
		Settings.save();

		newCompare.reset();

		if (!compareList.empty())
		{
			setStyles(Settings);
			NavDlg.SetColors(Settings.colors);
		}
	}
}


void OpenAboutDlg()
{
#ifdef DLOG

	if (dLogBuf == -1)
	{
		::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);

		dLogBuf = getCurrentBuffId();

		HWND hNppTabBar = NppTabHandleGetter::get(getCurrentViewId());

		if (hNppTabBar)
		{
			TCHAR name[] = { TEXT("CP_debug_log") };

			TCITEM tab;
			tab.mask = TCIF_TEXT;
			tab.pszText = name;

			TabCtrl_SetItem(hNppTabBar, posFromBuffId(dLogBuf), &tab);
		}
	}
	else
	{
		activateBufferID(dLogBuf);
	}

	const int view = getCurrentViewId();

	CallScintilla(view, SCI_APPENDTEXT, dLog.size(), (LPARAM)dLog.c_str());
	CallScintilla(view, SCI_SETSAVEPOINT, 0, 0);

	dLog.clear();

#else

	AboutDlg.doDialog();

#endif
}


void createMenu()
{
	_tcscpy_s(funcItem[CMD_SET_FIRST]._itemName, nbChar, TEXT("Set as First to Compare"));
	funcItem[CMD_SET_FIRST]._pFunc				= SetAsFirst;
	funcItem[CMD_SET_FIRST]._pShKey				= new ShortcutKey;
	funcItem[CMD_SET_FIRST]._pShKey->_isAlt		= true;
	funcItem[CMD_SET_FIRST]._pShKey->_isCtrl	= true;
	funcItem[CMD_SET_FIRST]._pShKey->_isShift	= false;
	funcItem[CMD_SET_FIRST]._pShKey->_key		= '1';

	_tcscpy_s(funcItem[CMD_COMPARE]._itemName, nbChar, TEXT("Compare"));
	funcItem[CMD_COMPARE]._pFunc			= CompareWhole;
	funcItem[CMD_COMPARE]._pShKey			= new ShortcutKey;
	funcItem[CMD_COMPARE]._pShKey->_isAlt	= true;
	funcItem[CMD_COMPARE]._pShKey->_isCtrl	= true;
	funcItem[CMD_COMPARE]._pShKey->_isShift	= false;
	funcItem[CMD_COMPARE]._pShKey->_key		= 'C';

	_tcscpy_s(funcItem[CMD_COMPARE_LINES]._itemName, nbChar, TEXT("Compare Selected Lines"));
	funcItem[CMD_COMPARE_LINES]._pFunc				= CompareSelectedLines;
	funcItem[CMD_COMPARE_LINES]._pShKey				= new ShortcutKey;
	funcItem[CMD_COMPARE_LINES]._pShKey->_isAlt		= true;
	funcItem[CMD_COMPARE_LINES]._pShKey->_isCtrl	= true;
	funcItem[CMD_COMPARE_LINES]._pShKey->_isShift	= false;
	funcItem[CMD_COMPARE_LINES]._pShKey->_key		= 'N';

	_tcscpy_s(funcItem[CMD_CLEAR_ACTIVE]._itemName, nbChar, TEXT("Clear Active Compare"));
	funcItem[CMD_CLEAR_ACTIVE]._pFunc				= ClearActiveCompare;
	funcItem[CMD_CLEAR_ACTIVE]._pShKey 				= new ShortcutKey;
	funcItem[CMD_CLEAR_ACTIVE]._pShKey->_isAlt 		= true;
	funcItem[CMD_CLEAR_ACTIVE]._pShKey->_isCtrl		= true;
	funcItem[CMD_CLEAR_ACTIVE]._pShKey->_isShift	= false;
	funcItem[CMD_CLEAR_ACTIVE]._pShKey->_key 		= 'X';

	_tcscpy_s(funcItem[CMD_CLEAR_ALL]._itemName, nbChar, TEXT("Clear All Compares"));
	funcItem[CMD_CLEAR_ALL]._pFunc	= ClearAllCompares;

	_tcscpy_s(funcItem[CMD_LAST_SAVE_DIFF]._itemName, nbChar, TEXT("Diff since last Save"));
	funcItem[CMD_LAST_SAVE_DIFF]._pFunc				= LastSaveDiff;
	funcItem[CMD_LAST_SAVE_DIFF]._pShKey 			= new ShortcutKey;
	funcItem[CMD_LAST_SAVE_DIFF]._pShKey->_isAlt 	= true;
	funcItem[CMD_LAST_SAVE_DIFF]._pShKey->_isCtrl 	= true;
	funcItem[CMD_LAST_SAVE_DIFF]._pShKey->_isShift	= false;
	funcItem[CMD_LAST_SAVE_DIFF]._pShKey->_key 		= 'D';

	_tcscpy_s(funcItem[CMD_SVN_DIFF]._itemName, nbChar, TEXT("SVN Diff"));
	funcItem[CMD_SVN_DIFF]._pFunc 				= SvnDiff;
	funcItem[CMD_SVN_DIFF]._pShKey 				= new ShortcutKey;
	funcItem[CMD_SVN_DIFF]._pShKey->_isAlt 		= true;
	funcItem[CMD_SVN_DIFF]._pShKey->_isCtrl 	= true;
	funcItem[CMD_SVN_DIFF]._pShKey->_isShift	= false;
	funcItem[CMD_SVN_DIFF]._pShKey->_key 		= 'V';

	_tcscpy_s(funcItem[CMD_GIT_DIFF]._itemName, nbChar, TEXT("Git Diff"));
	funcItem[CMD_GIT_DIFF]._pFunc 				= GitDiff;
	funcItem[CMD_GIT_DIFF]._pShKey 				= new ShortcutKey;
	funcItem[CMD_GIT_DIFF]._pShKey->_isAlt 		= true;
	funcItem[CMD_GIT_DIFF]._pShKey->_isCtrl 	= true;
	funcItem[CMD_GIT_DIFF]._pShKey->_isShift	= false;
	funcItem[CMD_GIT_DIFF]._pShKey->_key 		= 'G';

	_tcscpy_s(funcItem[CMD_IGNORE_SPACES]._itemName, nbChar, TEXT("Ignore Spaces"));
	funcItem[CMD_IGNORE_SPACES]._pFunc = IgnoreSpaces;

	_tcscpy_s(funcItem[CMD_IGNORE_CASE]._itemName, nbChar, TEXT("Ignore Case"));
	funcItem[CMD_IGNORE_CASE]._pFunc = IgnoreCase;

	_tcscpy_s(funcItem[CMD_DETECT_MOVES]._itemName, nbChar, TEXT("Detect Moves"));
	funcItem[CMD_DETECT_MOVES]._pFunc = DetectMoves;

	_tcscpy_s(funcItem[CMD_NAV_BAR]._itemName, nbChar, TEXT("Navigation Bar"));
	funcItem[CMD_NAV_BAR]._pFunc = ViewNavigationBar;

	_tcscpy_s(funcItem[CMD_PREV]._itemName, nbChar, TEXT("Previous"));
	funcItem[CMD_PREV]._pFunc 				= Prev;
	funcItem[CMD_PREV]._pShKey 				= new ShortcutKey;
	funcItem[CMD_PREV]._pShKey->_isAlt 		= true;
	funcItem[CMD_PREV]._pShKey->_isCtrl 	= false;
	funcItem[CMD_PREV]._pShKey->_isShift	= false;
	funcItem[CMD_PREV]._pShKey->_key 		= VK_PRIOR;

	_tcscpy_s(funcItem[CMD_NEXT]._itemName, nbChar, TEXT("Next"));
	funcItem[CMD_NEXT]._pFunc 				= Next;
	funcItem[CMD_NEXT]._pShKey 				= new ShortcutKey;
	funcItem[CMD_NEXT]._pShKey->_isAlt 		= true;
	funcItem[CMD_NEXT]._pShKey->_isCtrl 	= false;
	funcItem[CMD_NEXT]._pShKey->_isShift	= false;
	funcItem[CMD_NEXT]._pShKey->_key 		= VK_NEXT;

	_tcscpy_s(funcItem[CMD_FIRST]._itemName, nbChar, TEXT("First"));
	funcItem[CMD_FIRST]._pFunc 				= First;
	funcItem[CMD_FIRST]._pShKey 			= new ShortcutKey;
	funcItem[CMD_FIRST]._pShKey->_isAlt 	= true;
	funcItem[CMD_FIRST]._pShKey->_isCtrl 	= true;
	funcItem[CMD_FIRST]._pShKey->_isShift	= false;
	funcItem[CMD_FIRST]._pShKey->_key 		= VK_PRIOR;

	_tcscpy_s(funcItem[CMD_LAST]._itemName, nbChar, TEXT("Last"));
	funcItem[CMD_LAST]._pFunc 				= Last;
	funcItem[CMD_LAST]._pShKey 				= new ShortcutKey;
	funcItem[CMD_LAST]._pShKey->_isAlt 		= true;
	funcItem[CMD_LAST]._pShKey->_isCtrl 	= true;
	funcItem[CMD_LAST]._pShKey->_isShift	= false;
	funcItem[CMD_LAST]._pShKey->_key 		= VK_NEXT;

	_tcscpy_s(funcItem[CMD_SETTINGS]._itemName, nbChar, TEXT("Settings..."));
	funcItem[CMD_SETTINGS]._pFunc = OpenSettingsDlg;

#ifdef DLOG
	_tcscpy_s(funcItem[CMD_ABOUT]._itemName, nbChar, TEXT("Show debug log"));
#else
	_tcscpy_s(funcItem[CMD_ABOUT]._itemName, nbChar, TEXT("Help / About..."));
#endif
	funcItem[CMD_ABOUT]._pFunc = OpenAboutDlg;
}


void deinitPlugin()
{
	// Always close it, else N++'s plugin manager would call 'ViewNavigationBar'
	// on startup, when N++ has been shut down before with opened navigation bar
	if (NavDlg.isVisible())
		NavDlg.Hide();

	if (tbSetFirst.hToolbarBmp)
		::DeleteObject(tbSetFirst.hToolbarBmp);

	if (tbCompare.hToolbarBmp)
		::DeleteObject(tbCompare.hToolbarBmp);

	if (tbCompareLines.hToolbarBmp)
		::DeleteObject(tbCompareLines.hToolbarBmp);

	if (tbClearCompare.hToolbarBmp)
		::DeleteObject(tbClearCompare.hToolbarBmp);

	if (tbFirst.hToolbarBmp)
		::DeleteObject(tbFirst.hToolbarBmp);

	if (tbPrev.hToolbarBmp)
		::DeleteObject(tbPrev.hToolbarBmp);

	if (tbNext.hToolbarBmp)
		::DeleteObject(tbNext.hToolbarBmp);

	if (tbLast.hToolbarBmp)
		::DeleteObject(tbLast.hToolbarBmp);

	if (tbNavBar.hToolbarBmp)
		::DeleteObject(tbNavBar.hToolbarBmp);

	SettingsDlg.destroy();
	AboutDlg.destroy();
	NavDlg.destroy();

	// Deallocate shortcut
	for (int i = 0; i < NB_MENU_COMMANDS; i++)
	{
		if (funcItem[i]._pShKey != NULL)
        {
			delete funcItem[i]._pShKey;
            funcItem[i]._pShKey = NULL;
        }
	}
}


void syncViews(int biasView)
{
	const int otherView = getOtherViewId(biasView);

	const int firstVisibleLine = CallScintilla(biasView, SCI_GETFIRSTVISIBLELINE, 0, 0);

	if (firstVisibleLine != CallScintilla(otherView, SCI_GETFIRSTVISIBLELINE, 0, 0))
	{
		LOGD("Syncing to " + std::string(biasView == MAIN_VIEW ? "MAIN" : "SUB") + " view, visible doc line: " +
				std::to_string(CallScintilla(biasView, SCI_DOCLINEFROMVISIBLE, firstVisibleLine, 0)) + "\n");

		ScopedIncrementer incr(notificationsLock);

		CallScintilla(otherView, SCI_SETFIRSTVISIBLELINE, firstVisibleLine, 0);
	}

	NavDlg.Update();
}


void comparedFileActivated()
{
	if (!NppSettings::get().compareMode)
	{
		if (Settings.UseNavBar && !NavDlg.isVisible())
			showNavBar();

		NppSettings::get().setCompareMode();
	}

	setCompareView(MAIN_VIEW, Settings.colors.blank);
	setCompareView(SUB_VIEW, Settings.colors.blank);

	storedLocation.reset(new ViewLocation(getCurrentViewId()));
}


void onToolBarReady()
{
	UINT style = (LR_SHARED | LR_LOADTRANSPARENT | LR_DEFAULTSIZE | LR_LOADMAP3DCOLORS);

	const bool isRTL = ((::GetWindowLongPtr(nppData._nppHandle, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0);

	if (isRTL)
		tbSetFirst.hToolbarBmp =
				(HBITMAP)::LoadImage(hInstance, MAKEINTRESOURCE(IDB_SETFIRST_RTL), IMAGE_BITMAP, 0, 0, style);
	else
		tbSetFirst.hToolbarBmp =
				(HBITMAP)::LoadImage(hInstance, MAKEINTRESOURCE(IDB_SETFIRST), IMAGE_BITMAP, 0, 0, style);

	tbCompare.hToolbarBmp =
			(HBITMAP)::LoadImage(hInstance, MAKEINTRESOURCE(IDB_COMPARE), IMAGE_BITMAP, 0, 0, style);
	tbCompareLines.hToolbarBmp =
			(HBITMAP)::LoadImage(hInstance, MAKEINTRESOURCE(IDB_COMPARE_LINES), IMAGE_BITMAP, 0, 0, style);
	tbClearCompare.hToolbarBmp =
			(HBITMAP)::LoadImage(hInstance, MAKEINTRESOURCE(IDB_CLEARCOMPARE), IMAGE_BITMAP, 0, 0, style);
	tbFirst.hToolbarBmp =
			(HBITMAP)::LoadImage(hInstance, MAKEINTRESOURCE(IDB_FIRST),	IMAGE_BITMAP, 0, 0, style);
	tbPrev.hToolbarBmp =
			(HBITMAP)::LoadImage(hInstance, MAKEINTRESOURCE(IDB_PREV),	IMAGE_BITMAP, 0, 0, style);
	tbNext.hToolbarBmp =
			(HBITMAP)::LoadImage(hInstance, MAKEINTRESOURCE(IDB_NEXT),	IMAGE_BITMAP, 0, 0, style);
	tbLast.hToolbarBmp =
			(HBITMAP)::LoadImage(hInstance, MAKEINTRESOURCE(IDB_LAST),	IMAGE_BITMAP, 0, 0, style);
	tbNavBar.hToolbarBmp =
			(HBITMAP)::LoadImage(hInstance, MAKEINTRESOURCE(IDB_NAVBAR), IMAGE_BITMAP, 0, 0, style);

	::SendMessage(nppData._nppHandle, NPPM_ADDTOOLBARICON,
			(WPARAM)funcItem[CMD_SET_FIRST]._cmdID,			(LPARAM)&tbSetFirst);
	::SendMessage(nppData._nppHandle, NPPM_ADDTOOLBARICON,
			(WPARAM)funcItem[CMD_COMPARE]._cmdID,			(LPARAM)&tbCompare);
	::SendMessage(nppData._nppHandle, NPPM_ADDTOOLBARICON,
			(WPARAM)funcItem[CMD_COMPARE_LINES]._cmdID,		(LPARAM)&tbCompareLines);
	::SendMessage(nppData._nppHandle, NPPM_ADDTOOLBARICON,
			(WPARAM)funcItem[CMD_CLEAR_ACTIVE]._cmdID,		(LPARAM)&tbClearCompare);
	::SendMessage(nppData._nppHandle, NPPM_ADDTOOLBARICON,
			(WPARAM)funcItem[CMD_FIRST]._cmdID,				(LPARAM)&tbFirst);
	::SendMessage(nppData._nppHandle, NPPM_ADDTOOLBARICON,
			(WPARAM)funcItem[CMD_PREV]._cmdID,				(LPARAM)&tbPrev);
	::SendMessage(nppData._nppHandle, NPPM_ADDTOOLBARICON,
			(WPARAM)funcItem[CMD_NEXT]._cmdID,				(LPARAM)&tbNext);
	::SendMessage(nppData._nppHandle, NPPM_ADDTOOLBARICON,
			(WPARAM)funcItem[CMD_LAST]._cmdID,				(LPARAM)&tbLast);
	::SendMessage(nppData._nppHandle, NPPM_ADDTOOLBARICON,
			(WPARAM)funcItem[CMD_NAV_BAR]._cmdID,			(LPARAM)&tbNavBar);
}


void onNppReady()
{
	// It's N++'s job actually to disable its scroll menu commands but since it's not the case provide this as a patch
	if (isSingleView())
		NppSettings::get().enableNppScrollCommands(false);

	NppSettings::get().updatePluginMenu();

	::SendMessage(nppData._nppHandle, NPPM_SETMENUITEMCHECK, funcItem[CMD_IGNORE_SPACES]._cmdID,
			(LPARAM)Settings.IgnoreSpaces);
	::SendMessage(nppData._nppHandle, NPPM_SETMENUITEMCHECK, funcItem[CMD_IGNORE_CASE]._cmdID,
			(LPARAM)Settings.IgnoreCase);
	::SendMessage(nppData._nppHandle, NPPM_SETMENUITEMCHECK, funcItem[CMD_DETECT_MOVES]._cmdID,
			(LPARAM)Settings.DetectMoves);
	::SendMessage(nppData._nppHandle, NPPM_SETMENUITEMCHECK, funcItem[CMD_NAV_BAR]._cmdID,
			(LPARAM)Settings.UseNavBar);
}


void DelayedAlign::operator()()
{
	const LRESULT			currentBuffId	= getCurrentBuffId();
	CompareList_t::iterator	cmpPair			= getCompare(currentBuffId);

	if (cmpPair == compareList.end())
		return;

	const AlignmentInfo_t& alignmentInfo = cmpPair->alignmentInfo;
	if (alignmentInfo.empty())
		return;

	bool realign = goToFirst;

	if (!realign)
	{
		realign = isAlignmentNeeded(MAIN_VIEW, alignmentInfo);

		if (!realign)
			realign = isAlignmentNeeded(SUB_VIEW, alignmentInfo);
	}

	ScopedIncrementer incr(notificationsLock);

	if (realign)
	{
		LOGD("Aligning diffs\n");

		if (!storedLocation && !goToFirst)
			storedLocation.reset(new ViewLocation(getCurrentViewId()));

		alignDiffs(alignmentInfo);
	}

	if (goToFirst)
	{
		LOGD("Go to first diff\n");

		goToFirst = false;

		std::pair<int, int> viewLoc = jumpToFirstChange(true);

		if (viewLoc.first >= 0)
			syncViews(viewLoc.first);

		cmpPair->setStatus();
	}
	else if (storedLocation)
	{
		const int view = storedLocation->getView();

		storedLocation->restore();
		storedLocation.reset();

		syncViews(view);

		cmpPair->setStatus();
	}
}


inline void onSciPaint()
{
	delayedAlignment.post(10);
}


void onSciUpdateUI(HWND view)
{
	ScopedIncrementer incr(notificationsLock);

	syncViews(getViewId(view));
}


void DelayedUpdate::operator()()
{
	if (fullCompare)
	{
		linesAdded = 0;
		linesDeleted = 0;
		fullCompare = false;

		compare();

		return;
	}

	const int changeView = getCurrentViewId();

	const int startLine = CallScintilla(changeView, SCI_LINEFROMPOSITION, changePos, 0);

	section_t mainViewSec = { startLine, 1 };
	section_t subViewSec = { startLine, 1 };

	ScopedIncrementer incr(notificationsLock);

	// Adjust views re-compare range
	if (linesAdded || linesDeleted)
	{
		const int otherView = getOtherViewId();

		section_t& changeViewSec = (changeView == MAIN_VIEW) ? mainViewSec : subViewSec;
		section_t& otherViewSec = (changeView == SUB_VIEW) ? subViewSec : mainViewSec;

		const int startOff = startLine - getPrevUnmarkedLine(otherView, startLine, MARKER_MASK_LINE);

		changeViewSec.off -= startOff;
		otherViewSec.off -= startOff;

		changeViewSec.len += (startOff + linesAdded);
		otherViewSec.len += (startOff + linesDeleted);

		const int endLine = otherViewSec.off + otherViewSec.len - 1;
		const int endOff = getNextUnmarkedLine(otherView, endLine, MARKER_MASK_LINE) - endLine;

		changeViewSec.len += endOff;
		otherViewSec.len += endOff;

		clearMarksAndBlanks(MAIN_VIEW, mainViewSec.off, mainViewSec.len);
		clearMarksAndBlanks(SUB_VIEW, subViewSec.off, subViewSec.len);

		AlignmentInfo_t alignmentInfo;
		compareViews(mainViewSec, subViewSec, Settings, TEXT("Re-comparing changes..."), alignmentInfo);
	}
	else
	{
		clearMarks(MAIN_VIEW, mainViewSec.off, mainViewSec.len);
		clearMarks(SUB_VIEW, subViewSec.off, subViewSec.len);

		AlignmentInfo_t alignmentInfo;
		compareViews(mainViewSec, subViewSec, Settings, nullptr, alignmentInfo);
	}

	linesAdded = 0;
	linesDeleted = 0;

	// Force NavBar redraw
	if (NavDlg.isVisible())
		NavDlg.Show();
}


void onSciModified(SCNotification* notifyCode)
{
	const LRESULT buffId = getCurrentBuffId();

	CompareList_t::iterator cmpPair = getCompare(buffId);
	if (cmpPair == compareList.end())
		return;

	if (notifyCode->modificationType & SC_MOD_BEFOREDELETE)
	{
		const int currentView = getCurrentViewId();

		const int startLine = CallScintilla(currentView, SCI_LINEFROMPOSITION, notifyCode->position, 0);
		const int endLine =
			CallScintilla(currentView, SCI_LINEFROMPOSITION, notifyCode->position + notifyCode->length, 0);

		// Change is on single line?
		if (endLine <= startLine)
			return;

		const int currAction =
			notifyCode->modificationType & (SC_PERFORMED_USER | SC_PERFORMED_UNDO | SC_PERFORMED_REDO);

		cmpPair->getFileByBuffId(buffId).deletedSections.push(currAction, startLine, endLine);
	}
	else if ((notifyCode->modificationType & SC_MOD_INSERTTEXT) && notifyCode->linesAdded)
	{
		const int currentView = getCurrentViewId();

		const int startLine = CallScintilla(currentView, SCI_LINEFROMPOSITION, notifyCode->position, 0);

		const int currAction =
			notifyCode->modificationType & (SC_PERFORMED_USER | SC_PERFORMED_UNDO | SC_PERFORMED_REDO);

		cmpPair->getFileByBuffId(buffId).deletedSections.pop(currAction, startLine);
	}
}


void onSciModifiedUpdate(SCNotification* notifyCode)
{
	const LRESULT buffId = getCurrentBuffId();

	CompareList_t::iterator cmpPair = getCompare(buffId);
	if (cmpPair == compareList.end())
		return;

	if (notifyCode->modificationType & SC_MOD_BEFOREDELETE)
	{
		const int currentView = getCurrentViewId();

		const int startLine = CallScintilla(currentView, SCI_LINEFROMPOSITION, notifyCode->position, 0);
		const int endLine =
			CallScintilla(currentView, SCI_LINEFROMPOSITION, notifyCode->position + notifyCode->length, 0);

		if (endLine > startLine)
		{
			ScopedIncrementer incr(notificationsLock);

			clearMarks(currentView, startLine, endLine - startLine + 1);
		}
	}
	else if (notifyCode->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))
	{
		if (!delayedUpdate.fullCompare)
		{
			if (!delayedUpdate)
			{
				delayedUpdate.changePos = notifyCode->position;
			}
			else
			{
				delayedUpdate.cancel();

				if (delayedUpdate.changePos > notifyCode->position)
					delayedUpdate.changePos = notifyCode->position;
			}

			if (notifyCode->modificationType & SC_MOD_INSERTTEXT)
				delayedUpdate.linesAdded += notifyCode->linesAdded;
			else
				delayedUpdate.linesDeleted += (-notifyCode->linesAdded);

			delayedUpdate.post(10);
		}
	}
}


void onSciZoom()
{
	CompareList_t::iterator cmpPair = getCompare(getCurrentBuffId());
	if (cmpPair == compareList.end())
		return;

	ScopedIncrementer incr(notificationsLock);

	// sync both views zoom
	const int zoom = CallScintilla(getCurrentViewId(), SCI_GETZOOM, 0, 0);
	CallScintilla(getOtherViewId(), SCI_SETZOOM, zoom, 0);
}


void DelayedActivate::operator()()
{
	CompareList_t::iterator cmpPair = getCompare(buffId);
	if (cmpPair == compareList.end())
		return;

	LOGDB(buffId, "Activate\n");

	const ComparedFile& otherFile = cmpPair->getOtherFileByBuffId(buffId);

	// When compared file is activated make sure its corresponding pair file is also active in the other view
	if (getDocId(getOtherViewId()) != otherFile.sciDoc)
	{
		ScopedIncrementer incr(notificationsLock);

		activateBufferID(otherFile.buffId);
		activateBufferID(buffId);
	}

	comparedFileActivated();
}


void onBufferActivated(LRESULT buffId)
{
	delayedAlignment.cancel();
	delayedActivation.cancel();

	CompareList_t::iterator cmpPair = getCompare(buffId);
	if (cmpPair == compareList.end())
	{
		NppSettings::get().setNormalMode();
		setNormalView(getCurrentViewId());
		resetCompareView(getOtherViewId());
	}
	else
	{
		delayedActivation.buffId = buffId;
		delayedActivation.post(30);
	}
}


void DelayedClose::operator()()
{
	const LRESULT currentBuffId = getCurrentBuffId();

	ScopedIncrementer incr(notificationsLock);

	for (int i = static_cast<int>(closedBuffs.size()) - 1; i >= 0; --i)
	{
		CompareList_t::iterator cmpPair = getCompare(closedBuffs[i]);
		if (cmpPair == compareList.end())
			continue;

		ComparedFile& closedFile = cmpPair->getFileByBuffId(closedBuffs[i]);
		ComparedFile& otherFile = cmpPair->getOtherFileByBuffId(closedBuffs[i]);

		if (closedFile.isTemp)
		{
			if (closedFile.isOpen())
				closedFile.close();
			else
				closedFile.onClose();
		}

		if (otherFile.isTemp)
		{
			if (otherFile.isOpen())
			{
				LOGDB(otherFile.buffId, "Close\n");

				otherFile.close();
			}
			else
			{
				otherFile.onClose();
			}
		}
		else
		{
			if (otherFile.isOpen())
				otherFile.restore();
		}

		compareList.erase(cmpPair);
	}

	closedBuffs.clear();

	activateBufferID(currentBuffId);
	onBufferActivated(currentBuffId);

	// If it is the last file and it is not in the main view - move it there
	if (getNumberOfFiles() == 1 && getCurrentViewId() == SUB_VIEW)
	{
		::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);

		const LRESULT newBuffId = getCurrentBuffId();

		activateBufferID(currentBuffId);
		::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_VIEW_GOTO_ANOTHER_VIEW);
		activateBufferID(newBuffId);
		::SendMessage(nppData._nppHandle, NPPM_MENUCOMMAND, 0, IDM_FILE_CLOSE);
	}
}


void onFileBeforeClose(LRESULT buffId)
{
	CompareList_t::iterator cmpPair = getCompare(buffId);
	if (cmpPair == compareList.end())
		return;

	delayedAlignment.cancel();
	delayedUpdate.cancel();
	delayedActivation.cancel();

	delayedClosure.cancel();
	delayedClosure.closedBuffs.push_back(buffId);

	const LRESULT currentBuffId = getCurrentBuffId();

	ScopedIncrementer incr(notificationsLock);

	ComparedFile& closedFile = cmpPair->getFileByBuffId(buffId);
	closedFile.onBeforeClose();

	if (cmpPair->relativePos && (closedFile.originalViewId == viewIdFromBuffId(buffId)))
	{
		ComparedFile& otherFile = cmpPair->getOtherFileByBuffId(buffId);

		otherFile.originalPos = posFromBuffId(buffId) + cmpPair->relativePos;

		if (cmpPair->relativePos > 0)
			--otherFile.originalPos;
		else
			++otherFile.originalPos;

		if (otherFile.originalPos < 0)
			otherFile.originalPos = 0;
	}

	if (currentBuffId != buffId)
		activateBufferID(currentBuffId);

	delayedClosure.post(30);
}


void onFileSaved(LRESULT buffId)
{
	CompareList_t::iterator cmpPair = getCompare(buffId);
	if (cmpPair == compareList.end())
		return;

    const ComparedFile& otherFile = cmpPair->getOtherFileByBuffId(buffId);

    const LRESULT currentBuffId = getCurrentBuffId();
    const bool pairIsActive = (currentBuffId == buffId || currentBuffId == otherFile.buffId);

	ScopedIncrementer incr(notificationsLock);

    if (!pairIsActive)
        activateBufferID(buffId);

    if (pairIsActive && Settings.RecompareOnSave)
    {
        delayedAlignment.cancel();
        delayedUpdate.cancel();

        delayedUpdate.fullCompare = true;

        delayedUpdate.post(30);
    }

    if (otherFile.isTemp == LAST_SAVED_TEMP)
    {
        HWND hNppTabBar = NppTabHandleGetter::get(otherFile.compareViewId);

        if (hNppTabBar)
        {
            TCHAR tabText[MAX_PATH];

            TCITEM tab;
            tab.mask = TCIF_TEXT;
            tab.pszText = tabText;
            tab.cchTextMax = _countof(tabText);

            const int tabPos = posFromBuffId(otherFile.buffId);
            TabCtrl_GetItem(hNppTabBar, tabPos, &tab);

            _tcscat_s(tabText, _countof(tabText), TEXT(" - Outdated"));

			::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, TRUE);

            TabCtrl_SetItem(hNppTabBar, tabPos, &tab);

			::SendMessage(nppData._nppHandle, NPPM_HIDETABBAR, 0, FALSE);
        }
    }

    if (!pairIsActive)
    {
        activateBufferID(currentBuffId);
        onBufferActivated(currentBuffId);
    }
}


void DelayedMaximize::operator()()
{
	if (notificationsLock)
		--notificationsLock;

	NavDlg.Update();
}

} // anonymous namespace


void ViewNavigationBar()
{
	Settings.UseNavBar = !Settings.UseNavBar;
	::SendMessage(nppData._nppHandle, NPPM_SETMENUITEMCHECK, funcItem[CMD_NAV_BAR]._cmdID,
			(LPARAM)Settings.UseNavBar);
	Settings.markAsDirty();

	if (NppSettings::get().compareMode)
	{
		if (Settings.UseNavBar)
			showNavBar();
		else
			NavDlg.Hide();
	}
}


// Main plugin DLL function
BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD  reasonForCall, LPVOID)
 {
	hInstance = hinstDLL;

	switch (reasonForCall)
	{
		case DLL_PROCESS_ATTACH:
			createMenu();
		break;

		case DLL_PROCESS_DETACH:
			deinitPlugin();
		break;

		case DLL_THREAD_ATTACH:
		break;

		case DLL_THREAD_DETACH:
		break;
	}

	return TRUE;
}


//
// Notepad++ API functions below
//

extern "C" __declspec(dllexport) void setInfo(NppData notpadPlusData)
{
	nppData		= notpadPlusData;
	sciFunc		= (SciFnDirect)::SendMessage(notpadPlusData._scintillaMainHandle, SCI_GETDIRECTFUNCTION, 0, 0);
	sciPtr[0]	= (sptr_t)::SendMessage(notpadPlusData._scintillaMainHandle, SCI_GETDIRECTPOINTER, 0, 0);
	sciPtr[1]	= (sptr_t)::SendMessage(notpadPlusData._scintillaSecondHandle, SCI_GETDIRECTPOINTER, 0, 0);

	if (!sciFunc || !sciPtr[0] || !sciPtr[1])
	{
		::MessageBox(notpadPlusData._nppHandle,
				TEXT("Error getting direct Scintilla call pointers, plugin init failed!"),
				PLUGIN_NAME, MB_OK | MB_ICONERROR);

		exit(EXIT_FAILURE);
	}

	Settings.load();

	AboutDlg.init(hInstance, nppData);
	SettingsDlg.init(hInstance, nppData);
	NavDlg.init(hInstance);
}


extern "C" __declspec(dllexport) const TCHAR * getName()
{
	return PLUGIN_NAME;
}


extern "C" __declspec(dllexport) FuncItem * getFuncsArray(int* nbF)
{
	*nbF = NB_MENU_COMMANDS;
	return funcItem;
}


extern "C" __declspec(dllexport) void beNotified(SCNotification* notifyCode)
{
	switch (notifyCode->nmhdr.code)
	{
		// Handle wrap refresh
		case SCN_PAINTED:
			if (NppSettings::get().compareMode && !notificationsLock &&
					!delayedActivation && !delayedClosure && !delayedUpdate)
				onSciPaint();
		break;

		// Vertical scroll sync
		case SCN_UPDATEUI:
			if (NppSettings::get().compareMode && !notificationsLock && !storedLocation && !goToFirst &&
					!delayedActivation && !delayedClosure && !delayedUpdate)
				onSciUpdateUI((HWND)notifyCode->nmhdr.hwndFrom);
		break;

		case NPPN_BUFFERACTIVATED:
			if (!compareList.empty() && !notificationsLock && !delayedClosure)
				onBufferActivated(notifyCode->nmhdr.idFrom);
		break;

		case NPPN_FILEBEFORECLOSE:
			if (newCompare && (newCompare->pair.file[0].buffId == static_cast<LRESULT>(notifyCode->nmhdr.idFrom)))
				newCompare.reset();
#ifdef DLOG
			else if (dLogBuf == static_cast<LRESULT>(notifyCode->nmhdr.idFrom))
				dLogBuf = -1;
#endif
			else if (!compareList.empty() && !notificationsLock)
				onFileBeforeClose(notifyCode->nmhdr.idFrom);
		break;

		case NPPN_FILESAVED:
			if (!compareList.empty() && !notificationsLock)
				onFileSaved(notifyCode->nmhdr.idFrom);
		break;

		// This is used to monitor either:
		// - text change to automatically update results or
		// - deletion of lines to properly clear their compare markings
		case SCN_MODIFIED:
			if (NppSettings::get().compareMode && !notificationsLock)
			{
				if (Settings.UpdateOnChange)
					onSciModifiedUpdate(notifyCode);
				else
					onSciModified(notifyCode);
			}
		break;

		case SCN_ZOOM:
			if (NppSettings::get().compareMode && !notificationsLock)
				onSciZoom();
		break;

		case NPPN_WORDSTYLESUPDATED:
			setStyles(Settings);
			NavDlg.SetColors(Settings.colors);
		break;

		case NPPN_TBMODIFICATION:
			onToolBarReady();
		break;

		case NPPN_READY:
			onNppReady();
		break;

		case NPPN_BEFORESHUTDOWN:
			ClearAllCompares();
		break;

		case NPPN_SHUTDOWN:
			Settings.save();
			deinitPlugin();
		break;
	}
}


extern "C" __declspec(dllexport) LRESULT messageProc(UINT msg, WPARAM wParam, LPARAM)
{
	if ((msg == WM_SIZE))
	{
		if ((wParam == SIZE_MINIMIZED) && !notificationsLock)
		{
			LOGD("Notepad++ minimized\n");

			// On rare occasions Alignment is posted (Sci paint event is received) before minimize event is received
			delayedAlignment.cancel();

			++notificationsLock;
		}
		else if (wParam == SIZE_MAXIMIZED && notificationsLock)
		{
			LOGD("Notepad++ restored\n");

			delayedMaximize.post(100);
		}
	}

	return TRUE;
}


extern "C" __declspec(dllexport) BOOL isUnicode()
{
	return TRUE;
}

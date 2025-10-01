/*!
	Copyright:		Atomi Systems, Inc. <http://atomisystems.com>
	Description:
*/

#include "core/rlcoreprec.h"
#include "core/rltemplatemanager.h"
#include "rldefaults.h"
#include "core/rlimagerescale.h"
#include "core/rlconfig.h"
#include "core/rlresourceitem.h"
#include "core/rlcontrol.h"
#include "core/rlslide.h"
#include "core/rlcursorpath.h"
#include "core/rlsliderenderer.h"

#include "common/threadpool.h"

#include <wx/scopeguard.h>
#include <wx/busyinfo.h>
#include <wx/wfstream.h>

#include <future>
#include <algorithm>
#include "textedit/richtextxhtmlhandler.h"

#define TEMPLATE_IMAGES_MAX_CACHE_SIZE 30
#define TEMPLATE_THUMBNAIL_GRAPHICSBITMAP_CACHE_SIZE 2000 //
#define RL_DEFAULT_SETTING_FILE "ObjectSettings"
#define RL_DEFAULT_OBJECT_TEMPLATE_FILE _("Default")

static void rlRemovePath(const wxString& strPath, wxArrayString& paths)
{
	wxFileName fn(strPath);
	for (wxString strPathIter : paths)
	{
		wxFileName fn2(strPathIter);
		if (fn.SameAs(fn2))
		{
			paths.Remove(strPathIter);
			return;
		}
	}
}

class RLTemplateThumbnailLoader
{
public:
	RLTemplateThumbnailLoader();
	~RLTemplateThumbnailLoader();

	wxString GetThumnailPath(const wxString& strThemePath, const wxString& strUUID, const wxSize& sz, double dScaleFactor);
	wxBitmap GetThumbnail(const RLTemplateInfo& templateInfo, const wxSize& size, double dScaleFactor);
	void ClearThumbnail(const wxString& strTemplate);
	void ClearGraphicsResources();
	void AddEvtHandler(wxEvtHandler* evtHandler);
	void RemoveEvtHandler(wxEvtHandler* evtHandler);

private:
	wxBitmap GetFirstSlideThumbnail(RLProject* pProject, const wxSize& size, double dScaleFactor);

	std::thread* m_thread;
	CLRUCache<wxString, wxBitmap> m_thumbnailsCache;
	bool m_canceled;
	//path/uuid/size, we need to use uuid since we store thumbnails across sessions
	std::list<std::tuple<RLTemplateInfo, wxSize, double>> m_tasks;
	std::mutex m_mutex;
	std::condition_variable m_condition;
	std::list<wxEvtHandler*> m_evtHandlers;
	std::map<wxString, wxString> m_thumbnailFileThemeUuidMap;
};

//////////////////////////////////////////////////////////////////////////
RLTemplateManager::RLTemplateManager()
{
	m_pResourceManager = new RLResourceManager(m_dbTemplate, m_csTemplate, NULL, RL_MIN_USER_TEMPLATE_ID);
	m_pStyleManager = new RLStyleManager(m_dbTemplate, m_csTemplate, NULL, RL_MIN_USER_TEMPLATE_STYLE_ID);
	m_bAllThemeLoaded = false;
	m_bAllColorLoaded = false;
	m_bAllFontLoaded = false;
	m_pIconsProj = NULL;
	m_threadPool = NULL;
	m_nLoadedThemeCount = -1;
	m_nThemeCount = 0;
	m_thumbnailLoader = NULL;
}

RLTemplateManager::~RLTemplateManager()
{
	Close();
	wxDELETE(m_pStyleManager);
	wxDELETE(m_pResourceManager);
	rlDeleteContainer(m_lsColorSchemes);
	rlDeleteContainer(m_lsFontSchemes);
}

bool RLTemplateManager::Open(const wxString& strTemplateFile, const wxString& strPassword)
{
	Close();

	RL_SCOPED_MUTEX(m_critsect);
	wxASSERT(wxThread::IsMain());
	m_threadPool = new ThreadPool(1); //greater values will slowing down creating project
	m_thumbnailLoader = new RLTemplateThumbnailLoader();
	m_strTemplateFile = strTemplateFile;
	m_strPassword = strPassword;
	if (!wxFileExists(strTemplateFile))
		return false;
	if (!m_dbTemplate.Open(m_strTemplateFile, m_strPassword, WXSQLITE_OPEN_READONLY))
	{
		wxString strMsg = "Error while opening library: %s.\nError Msg: %s";
		wxLogError(strMsg, strTemplateFile.wc_str(), m_dbTemplate.GetErrorMsg());
		return false;
	}
	if (!m_dbTemplate.Begin(WXSQLITE_TRANSACTION_DEFERRED))
		return false;

	//Set up external file path
	m_pResourceManager->SetDataBaseFile(m_strTemplateFile);

	if (!m_pResourceManager->DoLoadItemsInfo(RLVersionNum(RL_VERSION_FILE_FORMAT)))
		return false;
	if (!m_pStyleManager->DoLoadBaseStyles(RLVersionNum(RL_VERSION_FILE_FORMAT)))
		return false;
	return true;
}

void RLTemplateManager::Close()
{
	RL_SCOPED_MUTEX(m_critsect);
	wxASSERT(wxThread::IsMain());
	wxDELETE(m_threadPool);
	wxDELETE(m_thumbnailLoader);
	m_themeEvtHandlers.clear();

	RLProject* pBlankProject = m_lsTemplates.size() > 0 ? m_lsTemplates.front() : NULL;
	RLTheme* pBlankTheme = nullptr;
	if (pBlankProject)
	{
		auto& blankThemes = pBlankProject->ThemeManager()->GetThemes();
		if (!blankThemes.empty())
			pBlankTheme = blankThemes.front();
	}

	// Close templates, themes
	while (!m_lsTemplates.empty())
	{
		RLProject* pProject = m_lsTemplates.front();

		if (pProject == pBlankProject)
		{
			m_lsTemplates.pop_front();
			continue;
		}

		// Only save user object templates
		bool bSave = false;
		wxFileName filename(pProject->GetFilePath());
		if (IsUserTemplatePath(filename.GetFullPath()) && filename.GetExt().IsSameAs(RL_OBJECT_TEMPLATE_FILE_EXT))
			bSave = true;

		RLTheme* pTheme = pProject->ThemeManager()->GetThemes().front();
		if (pTheme)
		{
			std::list<RLSlideContainer*> layoutContainers = { pTheme->GetSlideMaster() };
			layoutContainers.push_back(pTheme->GetFeedbackMaster());
			auto objectSettingContainers = pProject->GetSlideContainers(SLIDE_CONTAINER_OBJECT_SETTINGS);

			std::vector<RLSlideContainer*> vtContainers;
			vtContainers.insert(vtContainers.end(), layoutContainers.begin(), layoutContainers.end());
			vtContainers.insert(vtContainers.end(), objectSettingContainers.begin(), objectSettingContainers.end());

			for (auto* pContainer : vtContainers)
			{
				if (!pContainer)
					continue;

				RLSlideContainerType containerType = pContainer->GetType();
				bool bUseBlankReference = (containerType == SLIDE_CONTAINER_FEEDBACK ||
					containerType == SLIDE_CONTAINER_LAYOUT);

				if (bUseBlankReference && pBlankTheme)
				{
					RLSlideContainer* pBlankContainer = nullptr;
					if (containerType == SLIDE_CONTAINER_LAYOUT)
						pBlankContainer = pBlankTheme->GetSlideMaster();
					else if (containerType == SLIDE_CONTAINER_FEEDBACK)
						pBlankContainer = pBlankTheme->GetFeedbackMaster();

					if (pBlankContainer)
					{
						struct UnmatchedPlaceholderInfo
						{
							RLControl* pControl;
							RLPlaceholderType placeholderType;
							bool bInLayout;
							bool bInTitleSlide;

							UnmatchedPlaceholderInfo(RLControl* ctrl, RLPlaceholderType type, bool inLayout, bool inTitle)
								: pControl(ctrl), placeholderType(type), bInLayout(inLayout), bInTitleSlide(inTitle)
							{}
						};

						std::vector<UnmatchedPlaceholderInfo> unmatchedPlaceholders;
						bool bInLayout = (containerType == SLIDE_CONTAINER_LAYOUT ||
							containerType == SLIDE_CONTAINER_FEEDBACK);

						std::vector<RLSlide*> currentSlides(pContainer->GetSlides().begin(), pContainer->GetSlides().end());
						std::vector<RLSlide*> blankSlides(pBlankContainer->GetSlides().begin(), pBlankContainer->GetSlides().end());

						size_t slideCount = std::min(currentSlides.size(), blankSlides.size());

						for (size_t slideIdx = 0; slideIdx < currentSlides.size(); ++slideIdx)
						{
							RLSlide* pSlide = currentSlides[slideIdx];
							if (!pSlide)
								continue;

							const RLControlList& lsControls = pSlide->GetControls();
							bool bInTitleSlide = (pSlide->GetSlideName().Contains(_("Title")));

							RLSlide* pBlankSlide = nullptr;
							if (slideIdx < blankSlides.size())
							{
								pBlankSlide = blankSlides[slideIdx];
							}

							if (!pBlankSlide)
							{
								for (auto* pTempSlide : blankSlides)
								{
									if (pTempSlide &&
										(pTempSlide->GetSlideName() == pSlide->GetSlideName()
											|| pSlide->GetSlideName().Contains(pTempSlide->GetSlideName())))
									{
										pBlankSlide = pTempSlide;
										break;
									}
								}

								if (!pBlankSlide)
								{
									for (auto* pTempSlide : blankSlides)
									{
										if (pTempSlide && pTempSlide->GetType() == pSlide->GetType())
										{
											pBlankSlide = pTempSlide;
											break;
										}
									}
								}
							}

							if (pBlankSlide)
							{
								pSlide->SetSlideName(pBlankSlide->GetSlideName());

								const RLControlList& lsBlankControls = pBlankSlide->GetControls();

								struct ControlKey
								{
									int placeholderType;
									int kind;
									int subKind;

									bool operator<(const ControlKey& other) const
									{
										if (placeholderType != other.placeholderType)
											return placeholderType < other.placeholderType;
										if (kind != other.kind)
											return kind < other.kind;
										return subKind < other.subKind;
									}
								};

								std::map<ControlKey, std::vector<RLControl*>> blankControlGroups;
								std::map<ControlKey, std::vector<RLControl*>> currentControlGroups;

								for (RLControl* pTempControl : lsBlankControls)
								{
									if (!pTempControl)
										continue;

									ControlKey key = {
										pTempControl->GetPlaceholderType(),
										pTempControl->GetKind(),
										pTempControl->GetSubKind()
									};
									blankControlGroups[key].push_back(pTempControl);
								}

								for (RLControl* pControl : lsControls)
								{
									if (!pControl)
										continue;

									ControlKey key = {
										pControl->GetPlaceholderType(),
										pControl->GetKind(),
										pControl->GetSubKind()
									};
									currentControlGroups[key].push_back(pControl);
								}

								for (auto& pair : currentControlGroups)
								{
									const ControlKey& key = pair.first;
									std::vector<RLControl*>& currentControls = pair.second;

									auto blankIt = blankControlGroups.find(key);
									if (blankIt == blankControlGroups.end())
									{
										for (RLControl* pControl : currentControls)
										{
											RLPlaceholderType placeholderType = (RLPlaceholderType)pControl->GetPlaceholderType();
											if (placeholderType != RL_PLACEHOLDER_NONE)
											{
												unmatchedPlaceholders.push_back(
													UnmatchedPlaceholderInfo(pControl, placeholderType, bInLayout, bInTitleSlide)
												);
											}
										}
										continue;
									}

									std::vector<RLControl*>& blankControls = blankIt->second;
									size_t matchCount = std::min(currentControls.size(), blankControls.size());

									for (size_t i = 0; i < matchCount; ++i)
									{
										RLControl* pControl = currentControls[i];
										RLControl* pBlankControl = blankControls[i];

										if (!pControl || !pBlankControl)
											continue;

										int nStateCount = pControl->GetStateCount();
										int nBlankStateCount = pBlankControl->GetStateCount();

										if (nStateCount == nBlankStateCount)
										{
											for (int j = 0; j < nStateCount && j < nBlankStateCount; ++j)
											{
												wxString strBlankText = rlHTML2PlainText(pBlankControl->GetStateByIndex(j)->GetText());
												wxString strCurrentText = rlHTML2PlainText(pControl->GetStateByIndex(j)->GetText());
												wxMessageOutputDebug().Printf("Slide[%zu] Control[%zu/%zu] strBlankText: %s\n", 
													slideIdx, i, matchCount, strBlankText);
												wxMessageOutputDebug().Printf("Slide[%zu] Control[%zu/%zu] strCurrentText: %s\n", 
													slideIdx, i, matchCount, strCurrentText);
												pControl->GetStateByIndex(j)->SetText(strBlankText);
												pControl->GetStateByIndex(j)->SetName(pBlankControl->GetStateByIndex(j)->GetName());
											}
										}
									}

									for (size_t i = matchCount; i < currentControls.size(); ++i)
									{
										RLControl* pControl = currentControls[i];
										if (!pControl)
											continue;

										RLPlaceholderType placeholderType = (RLPlaceholderType)pControl->GetPlaceholderType();
										if (placeholderType != RL_PLACEHOLDER_NONE)
										{
											unmatchedPlaceholders.push_back(
												UnmatchedPlaceholderInfo(pControl, placeholderType, bInLayout, bInTitleSlide)
											);
										}
									}
								}
							}
							else
							{
								for (RLControl* pControl : lsControls)
								{
									if (!pControl)
										continue;

									RLPlaceholderType placeholderType = (RLPlaceholderType)pControl->GetPlaceholderType();
									if (placeholderType != RL_PLACEHOLDER_NONE)
									{
										unmatchedPlaceholders.push_back(
											UnmatchedPlaceholderInfo(pControl, placeholderType, bInLayout, bInTitleSlide)
										);
									}
								}
							}
						}

						if (!unmatchedPlaceholders.empty())
						{
							wxMessageOutputDebug().Printf("=== Processing %zu Unmatched Placeholders ===\n",
								unmatchedPlaceholders.size());

							for (const auto& info : unmatchedPlaceholders)
							{
								if (!info.pControl)
									continue;

								wxString strDefaultText = RLSlideMaster::GetPlaceholderText(
									info.placeholderType,
									info.bInLayout,
									info.bInTitleSlide
								);

								wxMessageOutputDebug().Printf("Control ID: %d, PlaceholderType: %d, DefaultText: %s\n",
									(int)info.pControl->GetControlID(), (int)info.placeholderType, strDefaultText);

								int nStateCount = info.pControl->GetStateCount();
								for (int i = 0; i < nStateCount; ++i)
								{
									wxString strCurrentText = rlHTML2PlainText(info.pControl->GetStateByIndex(i)->GetText());
									wxMessageOutputDebug().Printf("  Before: %s\n", strCurrentText);
									info.pControl->GetStateByIndex(i)->SetText(strDefaultText);
									wxMessageOutputDebug().Printf("  After: %s\n", strDefaultText);
								}
							}
						}
					}
				}
				else
				{
					for (auto* pSlide : pContainer->GetSlides())
					{
						if (auto* lsControls = pSlide ? pSlide->GetTopLevelControls() : nullptr)
						{
							for (auto* pControl : *lsControls)
							{
								if (pControl)
									pProject->UpdateControlTranslation(pControl, true);
							}
						}
					}
				}
			}
			pTheme->MarkNeedUpdate(true, true);
		}

		bSave = true;
		pProject->Close(bSave);
		delete pProject;
		m_lsTemplates.pop_front();
	}

	if (pBlankProject)
	{
		pBlankProject->Close(false);
		delete pBlankProject;
	}

	m_lsOpenedTemplates.clear();
	RLResourceTaskThread::Get()->OnNotifyFromDB(m_pResourceManager, true);
	if (m_dbTemplate.IsOpen())
	{
		m_pResourceManager->StoreExternalFiles();
		m_pResourceManager->DoUnLoad();
	}
	m_pStyleManager->DoUnLoadBaseStyles();
	m_pResourceManager->DoUnLoad();
}


void RLTemplateManager::DoSortTemplates()
{
	std::list<RLProject*> lsBuiltIn;
	std::list<RLProject*> lsCustom;
	for (RLProject* pProject : m_lsTemplates)
	{
		if (IsBuiltInThemePath(pProject->GetFilePath()))
			lsBuiltIn.push_back(pProject);
		else
			lsCustom.push_back(pProject);
	}
	wxString strBlankThemePath = GetBlankThemePath();
	lsBuiltIn.sort([&](RLProject* p1, RLProject* p2) {
		wxFileName fn1(p1->GetFilePath());
		wxFileName fn2(p2->GetFilePath());
		if (fn1.GetFullPath() == strBlankThemePath)
			return true;
		if (fn2.GetFullPath() == strBlankThemePath)
			return false;
		return fn1.GetName() < fn2.GetName();
	});
	lsCustom.sort([&](RLProject* p1, RLProject* p2) {
		wxFileName fn1(p1->GetFilePath());
		wxFileName fn2(p2->GetFilePath());
		return fn1.GetName() < fn2.GetName();
	});
	m_lsTemplates.clear();
	m_lsTemplates.insert(m_lsTemplates.end(), lsBuiltIn.begin(), lsBuiltIn.end());
	m_lsTemplates.insert(m_lsTemplates.end(), lsCustom.begin(), lsCustom.end());
}

void RLTemplateManager::DoSortTemplateInfos()
{
	std::list<RLTemplateInfo> lsBuiltIn;
	std::list<RLTemplateInfo> lsCustom;
	for (const RLTemplateInfo& templateInfo : m_lsTemplateInfo)
	{
		if (IsBuiltInThemePath(templateInfo.m_strFilePath))
			lsBuiltIn.push_back(templateInfo);
		else
			lsCustom.push_back(templateInfo);
	}
	lsBuiltIn.sort([](const RLTemplateInfo& p1, const RLTemplateInfo& p2) {
		// 1st: default
		if (p1.m_bDefault != p2.m_bDefault)
			return p1.m_bDefault;
		// 2nd: blank
		if (p1.m_bBlank != p2.m_bBlank)
			return p1.m_bBlank;
		// template before theme
		if (p1.m_eKind != p2.m_eKind && (p1.m_eKind == RL_PROJECT_SLIDE_TEMPLATE || p2.m_eKind == RL_PROJECT_SLIDE_TEMPLATE))
			return p1.m_eKind == RL_PROJECT_SLIDE_TEMPLATE;

		wxFileName fn1(p1.m_strFilePath);
		wxFileName fn2(p2.m_strFilePath);
		return fn1.GetName() < fn2.GetName();
	});
	lsCustom.sort([](const RLTemplateInfo& p1, const RLTemplateInfo& p2) {
		wxFileName fn1(p1.m_strFilePath);
		wxFileName fn2(p2.m_strFilePath);
		return fn1.GetName() < fn2.GetName();
	});
	m_lsTemplateInfo.clear();
	if (!lsBuiltIn.empty() && lsBuiltIn.front().m_bDefault)
	{
		m_lsTemplateInfo.push_back(lsBuiltIn.front());
		lsBuiltIn.pop_front();
	}
	m_lsTemplateInfo.insert(m_lsTemplateInfo.end(), lsCustom.begin(), lsCustom.end());
	m_lsTemplateInfo.insert(m_lsTemplateInfo.end(), lsBuiltIn.begin(), lsBuiltIn.end());
}

RLProject* RLTemplateManager::DoOpenTemplate(const wxString& strFilePath, const wxString strPassword /*= ""*/)
{
	RLProject* pProject = new RLProject();
	int nRet = pProject->Open(strFilePath, strPassword);
	if (nRet == CORE_SUCCESS)
	{
		RLSlideList lsAllSlides = pProject->GetAllSlides(true);
		for (RLSlide* pSlide : lsAllSlides)
		{
			pSlide->UpdateCustomControlID();
		}
	}
	else
	{
		wxDELETE(pProject);
	}
	return pProject;
}

RLProject* RLTemplateManager::OpenTemplate(const wxString& strFilePath, const wxString& strPassword /*= ""*/)
{
	wxFileName fnOpeningPath(strFilePath);
	while (true)
	{
		bool bOpening = false;
		{
			RL_SCOPED_MUTEX(m_csTemplate);
			for (RLProject* pProject : m_lsTemplates)
			{
				wxFileName fnProjPath(pProject->GetFilePath());
				if (fnProjPath.SameAs(fnOpeningPath))
				{
					return pProject;
				}
			}

			for (wxString strProjPath : m_lsOpenedTemplates)
			{
				wxFileName fnProjPath(strProjPath);
				if (fnProjPath.SameAs(fnOpeningPath))
				{
					bOpening = true;
					break;
				}
			}
			if (!bOpening)
			{
				m_lsOpenedTemplates.push_back(strFilePath);
				break;
			}
		}
		wxThread::Sleep(10);
	}	
	RLProject* pProject = DoOpenTemplate(strFilePath, strPassword);
	RL_SCOPED_MUTEX(m_csTemplate);
	if (pProject)
	{
		m_lsTemplates.push_back(pProject);
		DoSortTemplates();
	}
	else
	{
		rlRemovePath(strFilePath, m_lsOpenedTemplates);
	}
	return pProject;
}

void RLTemplateManager::CloseTemplate(const RLProject* pProject, bool bSaveChanged /*= true*/)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	if (!pProject)
		return;
	rlRemovePath(pProject->GetFilePath(), m_lsOpenedTemplates);
	for (RLProject* pProj : m_lsTemplates)
	{
		if (pProject == pProj)
		{
			pProj->Close(bSaveChanged);
			m_lsTemplates.remove(pProj);
			delete pProj;
			break;
		}
	}
}

std::list<RLProject*> RLTemplateManager::GetTemplates(int nKindFlags, RLOwnerType eOwnerType)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	std::list<RLProject*> lsProjects;
	for (RLProject* pProject : m_lsTemplates)
	{
		RLProjectKind eProjectKind = pProject->GetProjectKind();
		RLOwnerType eProjectOwnerType;
		if (IsCustomThemePath(pProject->GetFilePath()))
			eProjectOwnerType = RL_CUSTOM;
		else if (IsBuiltInThemePath(pProject->GetFilePath()))
			eProjectOwnerType = RL_BUILTIN;
		else
			continue;
		if ((eProjectKind & nKindFlags) != 0 && eProjectOwnerType == eOwnerType)
		{
			lsProjects.push_back(pProject);
		}
	}
	return lsProjects;
}

std::list<RLProject*> RLTemplateManager::GetTemplates(int nKindFlags)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	std::list<RLProject*> lsProjects;
	for (RLProject* pProject : m_lsTemplates)
	{
		if ((pProject->GetProjectKind() & nKindFlags) != 0)
		{
			lsProjects.push_back(pProject);
		}
	}
	return lsProjects;
}

RLProject* RLTemplateManager::GetTemplate(int nKindFlags, int nIndex)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	int i = 0;
	for (RLProject* pProject : m_lsTemplates)
	{
		if ((pProject->GetProjectKind() & nKindFlags) != 0)
		{
			if (nIndex == i)
				return pProject;
			i++;
		}
	}
	return NULL;
}

RLColorScheme RLTemplateManager::GetClassicColor()
{
	RLColorScheme cl(_("Classic"));

	cl.SetARGB(RLColorScheme::RL_THEME_DARK_1, 4278190080);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_1, 4294967295);
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_2, 4282668138);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_2, 4293388006);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_1, 4294967167);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_2, 4294951039);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_3, 4294934399);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_4, 4286578559);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_5, 4286567679);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_6, 4286545919);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_7, 4290805759);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_8, 4294934464);
	return cl;
}

RLFontScheme RLTemplateManager::GetClassicFont()
{
	RLFontScheme font("Arial-Tahoma");
	font.SetFont(RLFontScheme::LATIN_MAJOR, "Arial");
	font.SetFont(RLFontScheme::LATIN_MINOR, "Tahoma");
	return font;
}

void RLTemplateManager::SetDefaultTheme(const RLProject* pProject)
{
	if (pProject)
	{
		SetDefaultTheme(pProject->GetFilePath());
	}
}

void RLTemplateManager::SetDefaultTheme(const wxString& strFilePath)
{
	OPT_SAVE("theme.default", strFilePath);

	RL_SCOPED_MUTEX(m_csTemplate);
	for (RLTemplateInfo& item : m_lsTemplateInfo)
	{
		item.m_bDefault = item.m_strFilePath == strFilePath;
	}
	for (wxEvtHandler* evtHanlder : m_themeEvtHandlers)
	{
		wxCommandEvent* pEvt = new wxCommandEvent(RLEVT_THEME_DEFAULT_CHANGED);
		pEvt->SetString(strFilePath);
		evtHanlder->QueueEvent(pEvt);
	}
}

RLProject* RLTemplateManager::GetDefaultTheme()
{
	wxString strPath = OPT_GET_STR("theme.default");
	if (!strPath.IsEmpty())
		return GetTemplate(strPath);
	return NULL;
}

wxString RLTemplateManager::GetDefaultThemePath()
{
	return OPT_GET_STR("theme.default");
}

void RLTemplateManager::RemoveDefaultTheme()
{
	RLConfig::Get()->ConfigManager()->RemoveOption("theme.default");

	RL_SCOPED_MUTEX(m_csTemplate);
	for (RLTemplateInfo& item : m_lsTemplateInfo)
	{
		item.m_bDefault = false;
	}
	for (wxEvtHandler* evtHanlder : m_themeEvtHandlers)
	{
		wxCommandEvent* pEvt = new wxCommandEvent(RLEVT_THEME_DEFAULT_CHANGED);
		evtHanlder->QueueEvent(pEvt);
	}
}

RLProject* RLTemplateManager::GetBlankTheme()
{
	wxString strPath = GetBlankThemePath();
	if (!strPath.IsEmpty())
		return GetTemplate(strPath);
	wxASSERT(false);
	return NULL;
}

void RLTemplateManager::SetDefaultAITemplate(const wxString& strFilePath)
{
	OPT_SAVE("theme.ai.default", strFilePath);
}

wxString RLTemplateManager::GetDefaultAITemplatePath() const
{
	wxString strRet = OPT_GET_STR("theme.ai.default", "");
	if (strRet.IsEmpty() || !wxFileName::FileExists(strRet))
	{
		wxArrayString arrAITemplates;
		int nAITemplateCount = wxDir::GetAllFiles(RLConfig::Get()->GetAITemplateLocation(), &arrAITemplates, RL_SLIDE_TEMPLATE_FILTER, wxDIR_FILES);
		if (nAITemplateCount > 0)
		{
			wxString strFirstAITemplatePath = arrAITemplates[0];
			strRet = strFirstAITemplatePath;
		}
	}
	return strRet; 
}

RLProject* RLTemplateManager::GetDefaultAITemplateProject()
{
	wxString strTemplatePath = GetDefaultAITemplatePath();
	if (!strTemplatePath.IsEmpty() && wxFileName::FileExists(strTemplatePath))
	{
		return OpenTemplate(strTemplatePath);
	}
	return NULL;
}

wxString RLTemplateManager::GetBlankThemePath()
{
	wxFileName fn;
	fn.SetPath(RLConfig::Get()->GetBuiltInThemeFilePath());
	fn.SetName("Blank");
	fn.SetExt(RL_THEME_FILE_EXT);
	return fn.GetFullPath();
}

void RLTemplateManager::RemoveTemplateItemInfo(const wxString& strFilePath)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	size_t sizeBefore = m_lsTemplateInfo.size();
	m_lsTemplateInfo.remove_if([strFilePath](RLTemplateInfo item)
	{
		wxFileName fnItem(item.m_strFilePath);
		wxFileName fnItem2(strFilePath);
		return fnItem.SameAs(fnItem2);
	});
	if (sizeBefore == m_lsTemplateInfo.size())
		return;

	for (wxEvtHandler* evtHanlder : m_themeEvtHandlers)
	{
		wxCommandEvent* pEvt = new wxCommandEvent(RLEVT_THEME_REMOVED);
		pEvt->SetString(strFilePath);
		evtHanlder->QueueEvent(pEvt);
	}
}

void RLTemplateManager::DoAddTemplateInfo(RLProject* pTemplate)
{
	wxFileName filename(pTemplate->GetFilePath());

	RLTemplateInfo item;
	item.m_strUUID = RLUUIDGen::ConvertUUID2String(pTemplate->ThemeManager()->GetFirstTheme()->GetUUID());
	item.m_strFilePath = pTemplate->GetFilePath();
	item.m_strName = filename.GetName();
	item.m_eKind = pTemplate->GetProjectKind();
	item.m_bResponsive = pTemplate->IsResponsive();
	item.m_bBlank = false;
	item.m_bDefault = false;

	std::list<RLTemplateInfo>::iterator it = m_lsTemplateInfo.begin();
	if (IsCustomThemePath(item.m_strFilePath))
	{
		// Insert after custom existing theme and template but keep order
		for (; it != m_lsTemplateInfo.end(); it++)
		{
			RLTemplateInfo itemIt = *it;
			if (itemIt.m_strFilePath > item.m_strFilePath)
				break;
		}
	}
	else
	{
		it = m_lsTemplateInfo.end();
	}
	m_lsTemplateInfo.insert(it, item);
	DoSortTemplateInfos();

	for (wxEvtHandler* evtHanlder : m_themeEvtHandlers)
	{
		wxCommandEvent* pEvt = new wxCommandEvent(RLEVT_THEME_ADDED);
		pEvt->SetString(item.m_strFilePath);
		evtHanlder->QueueEvent(pEvt);
	}
}

void RLTemplateManager::AddTemplateItemInfo(const wxString& strFilePath)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	if (IsCustomThemePath(strFilePath) 
		|| IsBuiltInThemePath(strFilePath)
		)
	{
		for (RLProject* pTemplate : m_lsTemplates)
		{
			wxFileName fnProjPath(pTemplate->GetFilePath());
			wxFileName fnFindingPath(strFilePath);
			if (fnProjPath.SameAs(fnFindingPath))
			{
				DoAddTemplateInfo(pTemplate);
				return;
			}
		}
		wxASSERT(false);
	}
	else
	{
		wxASSERT(false);
	}
}

void RLTemplateManager::AddTemplateItemInfo(RLProject* pTemplate)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	DoAddTemplateInfo(pTemplate);
}

bool RLTemplateManager::IsCustomThemePath(wxString strFilePath)
{
	bool bCustomPath = false;
	wxFileName fn(strFilePath);
	wxASSERT(!fn.IsDir());
	wxFileName fnFilePath;
	fnFilePath.AssignDir(fn.GetPath());
	wxFileName fnCustomTheme;
	fnCustomTheme.AssignDir(RLConfig::Get()->GetCustomThemeFilePath());
	bCustomPath = fnFilePath.SameAs(fnCustomTheme);
	return bCustomPath;
}

bool RLTemplateManager::IsBuiltInThemePath(wxString strFilePath)
{
	bool bBuiltInPath = false;
	wxFileName fn(strFilePath);
	wxASSERT(!fn.IsDir());
	wxFileName fnFilePath;
	fnFilePath.AssignDir(fn.GetPath());
	wxFileName fnBuiltinTheme;
	fnBuiltinTheme.AssignDir(RLConfig::Get()->GetBuiltInThemeFilePath());
	bBuiltInPath = fnFilePath.SameAs(fnBuiltinTheme);
	return bBuiltInPath;
}

bool RLTemplateManager::IsUserTemplatePath(wxString strFilePath)
{
	bool bTemplatePath = false;
	wxFileName fn(strFilePath);
	wxASSERT(!fn.IsDir());
	wxFileName fnFilePath;
	fnFilePath.AssignDir(fn.GetPath());
	wxFileName fnUserTemplatePath;
	fnUserTemplatePath.AssignDir(RLConfig::Get()->GetUserTemplateLocation());
	bTemplatePath = fnFilePath.SameAs(fnUserTemplatePath);
	return bTemplatePath;
}

wxArrayString RLTemplateManager::GetObjectTemplateFiles()
{
	// Open or create default objects template
	wxFileName fnTemplate;
	fnTemplate.AssignDir(RLConfig::Get()->GetUserTemplateLocation());
	wxArrayString lsTemplatesFiles;
	wxDir::GetAllFiles(fnTemplate.GetPath(), &lsTemplatesFiles, RL_OBJECT_TEMPLATE_FILTER, wxDIR_FILES);

	wxFileName fnObjectSetting = fnTemplate;
	fnObjectSetting.SetName(RL_DEFAULT_SETTING_FILE);
	fnObjectSetting.SetExt(RL_OBJECT_TEMPLATE_FILE_EXT);
	for (size_t i = 0; i < lsTemplatesFiles.size(); ++i)
	{
		wxFileName fnObjectTemplate(lsTemplatesFiles[i]);
		if (fnObjectTemplate.GetName() == RL_DEFAULT_SETTING_FILE)
		{
			lsTemplatesFiles.erase(lsTemplatesFiles.begin() + i);
			break;
		}
	}
	return lsTemplatesFiles;
}

void RLTemplateManager::LoadTemplateListInfo()
{
	// Only load uid, name & thumbnail
	wxArrayString lsTemplateFiles;
	wxArrayString lsTmp;
	size_t nTemplates = wxDir::GetAllFiles(RLConfig::Get()->GetCustomThemeFilePath(), &lsTmp, RL_THEME_FILTER, wxDIR_FILES);
	nTemplates += wxDir::GetAllFiles(RLConfig::Get()->GetCustomThemeFilePath(), &lsTmp, RL_SLIDE_TEMPLATE_FILTER, wxDIR_FILES);
	lsTmp.Sort();
	lsTemplateFiles.insert(lsTemplateFiles.end(), lsTmp.begin(), lsTmp.end());
	lsTmp.Clear();
	nTemplates += wxDir::GetAllFiles(RLConfig::Get()->GetBuiltInThemeFilePath(), &lsTmp, RL_THEME_FILTER, wxDIR_FILES);
	nTemplates += wxDir::GetAllFiles(RLConfig::Get()->GetBuiltInThemeFilePath(), &lsTmp, RL_SLIDE_TEMPLATE_FILTER, wxDIR_FILES);
	lsTmp.Sort();
	lsTemplateFiles.insert(lsTemplateFiles.end(), lsTmp.begin(), lsTmp.end());
	lsTmp.Clear();
	
	nTemplates += wxDir::GetAllFiles(RLConfig::Get()->GetAITemplateLocation(), &lsTmp, RL_SLIDE_TEMPLATE_FILTER, wxDIR_FILES);
	lsTmp.Sort();
	lsTemplateFiles.insert(lsTemplateFiles.end(), lsTmp.begin(), lsTmp.end());
	lsTmp.Clear();

	// Just for translations
	static std::vector<wxString> vtTemplateNames = {
		_("Agriculture"),
		_("Architecture"),
		_("Astronomy"),
		_("Bamboo Architecture"),
		_("Blank"),
		_("Business Environment"),
		_("Customer Service"),
		_("Digital Transformation"),
		_("Education"),
		_("Electrical Safety"),
		_("Green Energy"),
		_("Internet Safety"),
		_("Math Detective"),
		_("Quantum"),
		_("STEM"),
		_("Telemedicine"),
		_("Transportation")
	};

	for (size_t i = 0; i < nTemplates; i++)
	{
		RLSQLiteDatabase db;
		if (OpenDatabase(db, lsTemplateFiles[i]) == CORE_SUCCESS)
		{
			bool bResponsive = false;
			wxString strDesc;
			wxString strSQL = "SELECT [description], [others] FROM [projects];";
			RLSQLiteResultSet rsRes = db.ExecuteQuery(strSQL);
			wxASSERT(!rsRes.Eof());
			while (rsRes.NextRow())
			{
				if (rsRes.FindColumnIndex("others") >= 0)
				{
					wxString strOthers = rsRes.GetAsString("others");
					RLJSONReader jReader;
					wxJSONValue jvContents = jReader.Parse(strOthers);
					if (!jvContents.IsValid())
					{
						wxLogMessage("LoadTemplateListInfo:: ERROR: %d. Contents: %s", jReader.GetParsedErrorCode(), strOthers);
						continue;;
					}

					if (jvContents.HasMember("responsive"))
						bResponsive = jvContents.ItemAt("responsive").AsBool();
				}
				else
					wxASSERT(false);

				if (rsRes.FindColumnIndex("description") >= 0)
					strDesc = rsRes.GetAsString("description");

				break;
			}

			strSQL = "SELECT [uuid], [thumbnail]  FROM [design] WHERE [index] = 0;";
			rsRes = db.ExecuteQuery(strSQL);
			wxASSERT(!rsRes.Eof());
			while (rsRes.NextRow())
			{
				if (rsRes.FindColumnIndex("uuid") >= 0)
				{
					boost::uuids::uuid uid;
					if (!RLUUIDGen::ConvertString2UUID(rsRes.GetString("uuid"), uid))
						continue;

					RLTemplateInfo item;
					item.m_strUUID = rsRes.GetString("uuid");
					item.m_strFilePath = lsTemplateFiles[i];
					item.m_strName = wxGetTranslation(wxFileName(lsTemplateFiles[i]).GetName());
					item.m_strDesc = strDesc;
					item.m_bResponsive = bResponsive;
					item.m_eKind = RLProject::GetProjectKind(item.m_strFilePath);
					item.m_bDefault = lsTemplateFiles[i] == GetDefaultThemePath();
					item.m_bBlank = lsTemplateFiles[i] == GetBlankThemePath();

					RL_SCOPED_MUTEX(m_csTemplate);
					m_lsTemplateInfo.push_back(item);
					break; // Only load first theme thumbnail
				}
			}
		}
		CloseDatabase(db, false);
	}

	RL_SCOPED_MUTEX(m_csTemplate);
	DoSortTemplateInfos();
}

std::list<RLTemplateInfo> RLTemplateManager::GetTemplateInfos(int nKindFlags, RLOwnerType eOwnerType)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	std::list<RLTemplateInfo> templateInfos;
	for (const RLTemplateInfo& templateInfo : m_lsTemplateInfo)
	{
		if ((templateInfo.m_eKind & nKindFlags) == 0)
			continue;
		RLOwnerType eProjectOwnerType;
		if (IsCustomThemePath(templateInfo.m_strFilePath))
			eProjectOwnerType = RL_CUSTOM;
		else if (IsBuiltInThemePath(templateInfo.m_strFilePath))
			eProjectOwnerType = RL_BUILTIN;
		else
			continue;
		if (eProjectOwnerType == eOwnerType)
		{
			templateInfos.push_back(templateInfo);
		}
	}
	return templateInfos;
}

bool RLTemplateManager::IsAllThemesLoaded() const
{
	RL_SCOPED_MUTEX(m_csTemplate);
	return m_nLoadedThemeCount == m_nThemeCount;
}

bool RLTemplateManager::IsAllColorsLoaded() const
{
	RL_SCOPED_MUTEX(m_csTemplate);
	return m_bAllColorLoaded;
}

void RLTemplateManager::SetAllColorsLoaded(bool bLoaded)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	m_bAllColorLoaded = bLoaded;
}

bool RLTemplateManager::IsAllFontsLoaded() const
{
	RL_SCOPED_MUTEX(m_csTemplate);
	return m_bAllFontLoaded;
}

void RLTemplateManager::SetAllFontsLoaded(bool bLoaded)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	m_bAllFontLoaded = bLoaded;
}

RLProject* RLTemplateManager::GetTemplate(const wxString& strFilePath)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	for (RLProject* pProject : m_lsTemplates)
	{
		wxFileName fnProjPath(pProject->GetFilePath());
		wxFileName fnFindingPath(strFilePath);
		if (fnProjPath.SameAs(fnFindingPath))
		{
			return pProject;
		}
	}
	return NULL;
}

RLProject* RLTemplateManager::CreateTemplate(const wxString& strFilePath, bool bOverWriteExisting, const wxString& strProjectName, const wxString& strPassword /*= ""*/, const RLProject* pSrcProject /*= NULL*/, bool bResponsive /*= false*/)
{
	RLProject* pProject = new RLProject();
	int nRet = pProject->Create(strFilePath, bOverWriteExisting, strProjectName, strPassword, pSrcProject, bResponsive);
	if (nRet != CORE_SUCCESS)
	{
		wxDELETE(pProject);
		return NULL;
	}
	RL_SCOPED_MUTEX(m_csTemplate);
	m_lsTemplates.push_back(pProject);
	DoSortTemplates();
	m_lsOpenedTemplates.push_back(strFilePath);
	return pProject;

}

void RLTemplateManager::LoadDefaultObjectTemplates()
{
	//TODO: Move to thread
	// Open or create default objects template
	wxString strLastUsedTemplate = RLConfig::Get()->ConfigManager()->GetOptionAsString("template.last_object_template", RL_DEFAULT_OBJECT_TEMPLATE_FILE);
	wxFileName fnTemplate;
	fnTemplate.AssignDir(RLConfig::Get()->GetUserTemplateLocation());
	fnTemplate.SetName(strLastUsedTemplate);
	fnTemplate.SetExt(RL_OBJECT_TEMPLATE_FILE_EXT);

	wxArrayString lsTemplatesFiles = GetObjectTemplateFiles();
	RLProject* pProject = NULL;
	if (lsTemplatesFiles.size() > 0)
	{
		wxArrayString::const_iterator it = std::find(lsTemplatesFiles.begin(), lsTemplatesFiles.end(), fnTemplate.GetFullPath());
		if (it != lsTemplatesFiles.end())
			pProject = OpenTemplate(*it);
		if (!pProject)
		{
			// Try to open the first, if failed then try the next...
			for (unsigned int i = 0; i < lsTemplatesFiles.size(); i++)
			{
				if (lsTemplatesFiles[i] == strLastUsedTemplate)
				{
					// Already try to open
					continue;
				}
				pProject = OpenTemplate(lsTemplatesFiles[i]);
				if (pProject)
				{
					wxFileName fn(lsTemplatesFiles[i]);
					RLConfig::Get()->ConfigManager()->SaveOption("template.last_object_template", fn.GetName());
					break;
				}
			}
		}
	}

	if (!pProject)
	{
		// Create default objects template
		fnTemplate.SetName(RL_DEFAULT_OBJECT_TEMPLATE_FILE);
		fnTemplate.SetExt(RL_OBJECT_TEMPLATE_FILE_EXT);
		RLProject* pProject = CreateTemplate(fnTemplate.GetFullPath(), true, fnTemplate.GetName(), "", GetBlankTheme(), true);
		if (!pProject)
		{
			wxLogError("Could not create default object templates");
			return;
		}
		else
		{
			//Close to make the profile file valid if program crashes
			CloseTemplate(pProject, true);
			pProject = OpenTemplate(fnTemplate.GetFullPath());
			if (!pProject)
			{
				wxLogError("Could not open default object templates");
			}
			RLConfig::Get()->ConfigManager()->SaveOption("template.last_object_template", strLastUsedTemplate);
		}
	}
}

int RLTemplateManager::OpenDatabase(RLSQLiteDatabase& db, const wxString& strDataFile, const wxString& strPassword /*= ""*/)
{
	int nRet = CORE_SUCCESS;
	if (db.IsOpen())
		CloseDatabase(db, true);

	if (!wxFileExists(strDataFile))
		return CORE_FILE_NOT_EXISTS;
	if (wxFile::Access(strDataFile, wxFile::read) == false)
		return CORE_FILE_ACCESS_DENIED;
	if (wxFile::Access(strDataFile, wxFile::write) == false)
		return CORE_FILE_ACCESS_DENIED;

	if (!db.Open(strDataFile, strPassword))
	{
		nRet = db.IsIncorrectPasswordError() ? CORE_WRONG_PASSWORD : CORE_PROJECT_CORRUPTED;
		return nRet;
	}
	db.SetBusyTimeout(RLOPEN_PROJECT_BUSY_TIMEOUT);
	db.ExecuteUpdate("PRAGMA locking_mode=EXCLUSIVE;");
	db.ExecuteUpdate("PRAGMA journal_mode=WAL;");
	db.ExecuteUpdate(wxString::Format("PRAGMA cache_size = %d;", 2000));

	if (!db.Begin(WXSQLITE_TRANSACTION_EXCLUSIVE))
	{
		nRet = db.IsIncorrectPasswordError() ? CORE_WRONG_PASSWORD : CORE_PROJECT_CORRUPTED;
		if (db.IsOpen())
			db.Close();
		return nRet;
	}

	return nRet;
}

void RLTemplateManager::CloseDatabase(RLSQLiteDatabase& db, bool bSaveChanged)
{
	if (db.IsOpen())
	{
		if (bSaveChanged)
			db.Commit();
		else
			db.Rollback();
		db.Close();
	}
}

const std::list<RLColorScheme*>& RLTemplateManager::GetColors() const
{
	RL_SCOPED_MUTEX(m_csTemplate);
	return m_lsColorSchemes;
}

const std::list<RLFontScheme*>& RLTemplateManager::GetFonts() const
{
	RL_SCOPED_MUTEX(m_csTemplate);
	return m_lsFontSchemes;
}

std::list<RLColorScheme> RLTemplateManager::GetBuiltInColors()
{
	std::list<RLColorScheme> lsColors;
	RLColorScheme cl(RL_APPNAME, RL_BUILTIN); // Default built-in theme
	lsColors.push_back(cl);

	cl = GetClassicColor();
	cl.SetType(RL_BUILTIN);
	lsColors.push_back(cl);

	cl.SetName(_("Grayscale"));
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_1, 0xFF000000);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_1, 0xFFFFFFFF);
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_2, 0xFF000000);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_2, 0xFFF8F8F8);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_1, 0xFFDDDDDD);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_2, 0xFFB2B2B2);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_3, 0xFF969696);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_4, 0xFF808080);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_5, 0xFF5F5F5F);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_6, 0xFF4D4D4D);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_7, 0xFF424242);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_8, 0xFF373737);
	lsColors.push_back(cl);

	cl.SetName(_("Blue"));
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_1, 0xFF000000);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_1, 0xFFFFFFFF);
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_2, 0xFF112244);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_2, 0xFFccddee);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_1, 0xFF1144bb);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_2, 0xFF4466dd);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_3, 0xFF7799ee);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_4, 0xFF334477);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_5, 0xFF6677aa);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_6, 0xFFaaaacc);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_7, 0xFF887766);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_8, 0xFFddccbb);
	lsColors.push_back(cl);

	cl.SetName(_("Blue Green"));
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_1, 0xFF000000);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_1, 0xFFFFFFFF);
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_2, 0xFF122c53);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_2, 0xFFbbccee);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_1, 0xFF3377cc);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_2, 0xFF1366C1);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_3, 0xFF66aaee);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_4, 0xFF448811);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_5, 0xFF88bb55);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_6, 0xFF77bb11);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_7, 0xFFccee66);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_8, 0xFF556644);
	lsColors.push_back(cl);

	cl.SetName(_("Green"));
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_1, 0xFF000000);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_1, 0xFFFFFFFF);
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_2, 0xFF003344);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_2, 0xFFddffff);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_1, 0xFF33aa33);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_2, 0xFF448800);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_3, 0xFF118855);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_4, 0xFF447777);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_5, 0xFF335544);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_6, 0xFF88bb66);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_7, 0xFF668844);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_8, 0xFFddee77);
	lsColors.push_back(cl);

	cl.SetName(_("Green Yellow"));
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_1, 0xFF000000);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_1, 0xFFFFFFFF);
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_2, 0xFF2e4a0d);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_2, 0xFFdae892);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_1, 0xFF95bf02);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_2, 0xFF115500);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_3, 0xFF559911);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_4, 0xFF88cc33);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_5, 0xFFccdd88);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_6, 0xFFddee33);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_7, 0xFFbbcc00);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_8, 0xFFffffaa);
	lsColors.push_back(cl);

	cl.SetName(_("Red"));
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_1, 0xFF000000);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_1, 0xFFFFFFFF);
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_2, 0xFF220011);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_2, 0xFFffeecc);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_1, 0xFFcc2211);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_2, 0xFFff5511);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_3, 0xFFff9922);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_4, 0xFFbb6622);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_5, 0xFF774411);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_6, 0xFF880000);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_7, 0xFF553366);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_8, 0xFFdd9966);
	lsColors.push_back(cl);

	cl.SetName(_("Red Violet"));
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_1, 0xFF000000);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_1, 0xFFFFFFFF);
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_2, 0xFF221144);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_2, 0xFFddaabb);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_1, 0xFFaa3399);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_2, 0xFF8833cc);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_3, 0xFFcc4455);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_4, 0xFFbb66cc);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_5, 0xFF995555);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_6, 0xFFdd7777);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_7, 0xFF441199);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_8, 0xFFdd7788);
	lsColors.push_back(cl);

	cl.SetName(_("Yellow"));
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_1, 0xFF000000);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_1, 0xFFFFFFFF);
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_2, 0xFF996633);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_2, 0xFFffeedd);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_1, 0xFFffdd33);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_2, 0xFFeeaa00);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_3, 0xFFcc9944);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_4, 0xFFddbb44);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_5, 0xFF998877);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_6, 0xFFdd6600);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_7, 0xFFaa3300);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_8, 0xFFeebb77);
	lsColors.push_back(cl);

	cl.SetName(_("Yellow Orange"));
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_1, 0xFF000000);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_1, 0xFFFFFFFF);
	cl.SetARGB(RLColorScheme::RL_THEME_DARK_2, 0xFFaa6611);
	cl.SetARGB(RLColorScheme::RL_THEME_LIGHT_2, 0xFFddbb99);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_1, 0xFFeebb22);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_2, 0xFFbb9944);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_3, 0xFF553300);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_4, 0xFFaa5544);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_5, 0xFFee7711);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_6, 0xFFffaa66);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_7, 0xFFaa2200);
	cl.SetARGB(RLColorScheme::RL_THEME_ACCENT_8, 0xFF553333);
	lsColors.push_back(cl);

	return lsColors;
}

std::list<RLFontScheme> RLTemplateManager::GetBuiltInFonts()
{
	std::list<RLFontScheme> lsFonts;
	RLFontScheme font("", RL_BUILTIN);

	font.SetName("Arial");
	font.SetMajorFont("Arial");
	font.SetMinorFont("Arial");
	lsFonts.push_back(font);

	// Classic: Arial-Tahoma
	font = GetClassicFont();
	font.SetType(RL_BUILTIN);
	lsFonts.push_back(font);

	font.SetName("Georgia");
	font.SetMajorFont("Georgia");
	font.SetMinorFont("Georgia");
	lsFonts.push_back(font);

	font.SetName("Arial-Times New Roman");
	font.SetMajorFont("Arial");
	font.SetMinorFont("Times New Roman");
	lsFonts.push_back(font);

	font.SetName("Times New Roman-Arial");
	font.SetMajorFont("Times New Roman");
	font.SetMinorFont("Arial");
	lsFonts.push_back(font);

#if (defined(RL_BUILD_ITUTOR) || defined(RL_BUILD_CHIEBO))
	font.SetName("MS Mincho");
	font.SetMajorFont("MS Mincho");
	font.SetMinorFont("MS Mincho");
	lsFonts.push_back(font);

	font.SetName("MS PMincho");
	font.SetMajorFont("MS PMincho");
	font.SetMinorFont("MS PMincho");
	lsFonts.push_back(font);

	font.SetName("MS Gothic");
	font.SetMajorFont("MS Gothic");
	font.SetMinorFont("MS Gothic");
	lsFonts.push_back(font);

	font.SetName("MS PGothic");
	font.SetMajorFont("MS PGothic");
	font.SetMinorFont("MS PGothic");
	lsFonts.push_back(font);
#endif

	font.SetName("Tahoma");
	font.SetMajorFont("Tahoma");
	font.SetMinorFont("Tahoma");
	lsFonts.push_back(font);

	font.SetName("Verdana");
	font.SetMajorFont("Verdana");
	font.SetMinorFont("Verdana");
	lsFonts.push_back(font);

	font.SetName("Trebuchet MS");
	font.SetMajorFont("Trebuchet MS");
	font.SetMinorFont("Trebuchet MS");
	lsFonts.push_back(font);

	return lsFonts;
}

wxString RLTemplateManager::GetThumnailPath(const wxString& strThemePath, const wxString& strUUID, const wxSize& sz, double dScaleFactor)
{
	wxFileName fnTheme(strThemePath);
	wxFileName fnThumbnailPath;
	fnThumbnailPath.AssignDir(RLConfig::Get()->GetThemeThumbnailCachePath());
	fnThumbnailPath.SetName(wxString::Format("%s_%s_%s_%dx%d_@%d.png", fnTheme.GetName(), fnTheme.GetExt(), strUUID, sz.x, sz.y, wxRound(dScaleFactor)));
	return fnThumbnailPath.GetFullPath();
}

bool RLTemplateManager::InsertColor(RLColorScheme* pColorScheme)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	if (!pColorScheme)
		return false;
	// Add to file
	wxFileName filename;
	filename.AssignDir(RLConfig::Get()->GetCustomColorFilePath());
	filename.SetFullName(wxString::Format("%s.json", pColorScheme->GetName()));
	wxString strFileName = filename.GetFullPath();
	wxFileOutputStream outStream(strFileName);
	if (outStream.IsOk())
	{
		RLColorSchemeManifest clrManifest;
		clrManifest.SetColor(pColorScheme);
		if (clrManifest.Save(outStream))
		{
			m_lsColorSchemes.push_back(pColorScheme);
			return true;
		}
	}
	return false;
}

RLColorScheme* RLTemplateManager::DeleteColor(RLColorScheme* pColorScheme)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	// Remove file
	wxFileName filename;
	filename.AssignDir(RLConfig::Get()->GetCustomColorFilePath());
	filename.SetFullName(wxString::Format("%s.json", pColorScheme->GetName()));
	wxString strFileName = filename.GetFullPath();
	if (wxRemoveFile(strFileName))
	{
		for (std::list<RLColorScheme*>::iterator it = m_lsColorSchemes.begin(); it != m_lsColorSchemes.end(); it++)
		{
			if (pColorScheme == *it)
			{
				m_lsColorSchemes.erase(it);
				break;
			}
		}
	}
	else
		return NULL;
	return pColorScheme;
}

RLColorScheme* RLTemplateManager::DeleteColor(int nIndex)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	int i = 0;
	for (std::list<RLColorScheme*>::iterator it = m_lsColorSchemes.begin(); it != m_lsColorSchemes.end(); it++, i++)
	{
		if (i == nIndex)
		{
			// Remove file
			wxFileName filename;
			filename.AssignDir(RLConfig::Get()->GetCustomColorFilePath());
			filename.SetFullName(wxString::Format("%s.json", (*it)->GetName()));
			wxString strFileName = filename.GetFullPath();
			if (wxRemoveFile(strFileName))
			{
				RLColorScheme* pColorScheme = *it;
				m_lsColorSchemes.erase(it);
				return pColorScheme;
			}
		}
	}
	return NULL;
}

bool RLTemplateManager::InsertFont(RLFontScheme* pFontScheme)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	if (!pFontScheme)
		return false;
	// Add to file
	wxFileName filename;
	filename.AssignDir(RLConfig::Get()->GetCustomFontFilePath());
	filename.SetFullName(wxString::Format("%s.json", pFontScheme->GetName()));
	wxString strFileName = filename.GetFullPath();
	wxFileOutputStream outStream(strFileName);
	if (outStream.IsOk())
	{
		RLFontSchemeManifest fontManifest;
		fontManifest.SetFont(pFontScheme);
		if (fontManifest.Save(outStream))
		{
			m_lsFontSchemes.push_back(pFontScheme);
			return true;
		}
	}
	return false;
}

RLFontScheme* RLTemplateManager::DeleteFont(RLFontScheme* pFontScheme)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	wxFileName filename;
	filename.AssignDir(RLConfig::Get()->GetCustomFontFilePath());
	filename.SetFullName(wxString::Format("%s.json", pFontScheme->GetName()));
	wxString strFileName = filename.GetFullPath();
	if (wxRemoveFile(strFileName))
	{
		for (std::list<RLFontScheme*>::iterator it = m_lsFontSchemes.begin(); it != m_lsFontSchemes.end(); it++)
		{
			if (pFontScheme == *it)
			{
				m_lsFontSchemes.erase(it);
				break;
			}
		}
	}
	else
		return NULL;
	return pFontScheme;
}

RLFontScheme* RLTemplateManager::DeleteFont(int nIndex)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	int i = 0;
	for (std::list<RLFontScheme*>::iterator it = m_lsFontSchemes.begin(); it != m_lsFontSchemes.end(); it++, i++)
	{
		if (i == nIndex)
		{
			// Remove file
			wxFileName filename;
			filename.AssignDir(RLConfig::Get()->GetCustomFontFilePath());
			filename.SetFullName(wxString::Format("%s.json", (*it)->GetName()));
			wxString strFileName = filename.GetFullPath();
			if (wxRemoveFile(strFileName))
			{
				RLFontScheme* pFontScheme = *it;
				m_lsFontSchemes.erase(it);
				return pFontScheme;
			}
		}
	}
	return NULL;
}

bool RLTemplateManager::UpdateColor(RLColorScheme* pOldColor, RLColorScheme* pNewColor)
{
	return DeleteColor(pOldColor) && InsertColor(pNewColor);
}

bool RLTemplateManager::UpdateFont(RLFontScheme* pOldFont, RLFontScheme* pNewFont)
{
	return DeleteFont(pOldFont) && InsertFont(pNewFont);
}

RLColorScheme* RLTemplateManager::GetColorByIndex(int nIndex)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	int i = 0;
	for (std::list<RLColorScheme*>::iterator it = m_lsColorSchemes.begin(); it != m_lsColorSchemes.end(); it++, i++)
	{
		if (i == nIndex)
			return (*it);
	}
	return NULL;
}

RLFontScheme* RLTemplateManager::GetFontByIndex(int nIndex)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	int i = 0;
	for (std::list<RLFontScheme*>::iterator it = m_lsFontSchemes.begin(); it != m_lsFontSchemes.end(); it++, i++)
	{
		if (i == nIndex)
			return (*it);
	}
	return NULL;
}


void RLTemplateManager::LoadThemes()
{
	wxASSERT(wxThread::IsMain());
	wxString strDefaultTheme = GetDefaultThemePath();
	if (wxFileName::FileExists(strDefaultTheme))
	{
		RLProject* pProject = OpenTemplate(strDefaultTheme);
		if (pProject && IsBuiltInThemePath(pProject->GetFilePath()))
		{
			pProject->UpdateTranslations();
		}
	}
	wxString strBlankTheme = GetBlankThemePath();
	if (strBlankTheme != strDefaultTheme && wxFileName::FileExists(strBlankTheme))
	{
		RLProject* pProject = OpenTemplate(strBlankTheme, "");
		if (pProject && IsBuiltInThemePath(pProject->GetFilePath()))
		{
			pProject->UpdateTranslations();
		}
	}

	RL_SCOPED_MUTEX(m_csTemplate);
	if (!m_threadPool)
		m_threadPool = new ThreadPool(1);
	m_threadPool->enqueue([&]() {
		RL_SCOPED_MUTEX(m_csTemplate);
		// Load built-in colors
		for (RLColorScheme cl : RLTemplateManager::GetBuiltInColors())
		{
			m_lsColorSchemes.push_back(new RLColorScheme(cl));
		}
		for (RLFontScheme font : RLTemplateManager::GetBuiltInFonts())
		{
			m_lsFontSchemes.push_back(new RLFontScheme(font));
		}

		// Load custom colors
		wxArrayString lsColorFiles;
		size_t nColor = wxDir::GetAllFiles(RLConfig::Get()->GetCustomColorFilePath(), &lsColorFiles, "*.json", wxDIR_FILES);
		for (size_t i = 0; i < nColor; i++)
		{
			wxString strColorFile = lsColorFiles[i];
			RLColorSchemeManifest colorManifest;
			if (!colorManifest.Load(strColorFile))
			{
				wxLogMessage("Cannot load theme colors: %s", strColorFile);
				continue;
			}
			RLColorScheme* pColorScheme = new RLColorScheme(colorManifest.GetColor());
			pColorScheme->SetType(RL_CUSTOM);
			m_lsColorSchemes.push_back(pColorScheme);
		}

		// Load custom fonts
		wxArrayString lsFontFiles;
		size_t nFont = wxDir::GetAllFiles(RLConfig::Get()->GetCustomFontFilePath(), &lsFontFiles, "*.json", wxDIR_FILES);
		for (size_t i = 0; i < nFont; i++)
		{
			wxString strFontFile = lsFontFiles[i];
			RLFontSchemeManifest fontManifest;
			if (!fontManifest.Load(strFontFile))
			{
				wxLogMessage("Cannot load theme fonts: %s", strFontFile);
				continue;
			}
			RLFontScheme* pFontScheme = new RLFontScheme(fontManifest.GetFont());
			pFontScheme->SetType(RL_CUSTOM);
			m_lsFontSchemes.push_back(pFontScheme);
		}
		m_bAllFontLoaded = true;
		m_bAllColorLoaded = true;
	});

	m_threadPool->enqueue([this]() {
		LoadDefaultObjectTemplates();
	});

	wxArrayString arrAllTemplateFiles;
	wxArrayString lsThemeFiles;

	wxDir::GetAllFiles(RLConfig::Get()->GetBuiltInThemeFilePath(), &lsThemeFiles, RL_THEME_FILTER, wxDIR_FILES);
	wxDir::GetAllFiles(RLConfig::Get()->GetBuiltInThemeFilePath(), &lsThemeFiles, RL_SLIDE_TEMPLATE_FILTER, wxDIR_FILES);
	lsThemeFiles.Sort();
	for (wxString strThemeFile : lsThemeFiles)
	{
		if (arrAllTemplateFiles.Index(strThemeFile) < 0 && strThemeFile != strBlankTheme && strThemeFile != strDefaultTheme)
			arrAllTemplateFiles.Add(strThemeFile);
	}
	lsThemeFiles.Clear();
	wxDir::GetAllFiles(RLConfig::Get()->GetCustomThemeFilePath(), &lsThemeFiles, RL_THEME_FILTER, wxDIR_FILES);
	wxDir::GetAllFiles(RLConfig::Get()->GetCustomThemeFilePath(), &lsThemeFiles, RL_SLIDE_TEMPLATE_FILTER, wxDIR_FILES);
	lsThemeFiles.Sort();
	for (wxString strThemeFile : lsThemeFiles)
	{
		if (arrAllTemplateFiles.Index(strThemeFile) < 0 && strThemeFile != strBlankTheme && strThemeFile != strDefaultTheme)
			arrAllTemplateFiles.Add(strThemeFile);
	}

	lsThemeFiles.Clear();
	wxDir::GetAllFiles(RLConfig::Get()->GetAITemplateLocation(), &lsThemeFiles, RL_SLIDE_TEMPLATE_FILTER, wxDIR_FILES);
	lsThemeFiles.Sort();
	wxString strDefaultAITemplate = GetDefaultAITemplatePath();
	for (wxString strThemeFile : lsThemeFiles)
	{
		if ((arrAllTemplateFiles.Index(strThemeFile) < 0 && strThemeFile != strBlankTheme && strThemeFile != strDefaultTheme)
			&& (arrAllTemplateFiles.Index(strDefaultAITemplate) < 0 && strThemeFile != strDefaultAITemplate)
			)
			arrAllTemplateFiles.Add(strThemeFile);
	}

	m_nThemeCount = arrAllTemplateFiles.size();
	m_nLoadedThemeCount = 0;

	for (size_t i = 0; i < m_nThemeCount; i++)
	{
		wxString strFilePath = arrAllTemplateFiles[i];
		m_lsOpenedTemplates.push_back(strFilePath);

		m_threadPool->enqueue([this, strFilePath]()
		{
			bool bBuiltIn = RLTemplateManager::IsBuiltInThemePath(strFilePath);
			RLProject* pProject = DoOpenTemplate(strFilePath);
			if (pProject)
			{
				// Update translations for builtin themes
				if (bBuiltIn)
				{
					pProject->UpdateTranslations();
#ifdef __WXDEBUG__
					// Check events and actions inheritance in builtin themes
					for (int i = 0; i < 2; ++i)
					{
						RLTheme* pFirstTheme = pProject->ThemeManager()->GetFirstTheme();
						RLSlideMaster* pSlideMaster = pFirstTheme ? i == 0 ? pFirstTheme->GetSlideMaster() : pFirstTheme->GetFeedbackMaster() : NULL;
						if (!pSlideMaster)
							continue;
						RLSlide* pMaster = pSlideMaster->GetMasterLayout();
						RLSlideList lsLayouts = pSlideMaster->GetLayouts();
						RLControlList lsAnimsCheck, lsEventsCheck;
						std::list<RLPlaceholderType> lsPlaceholderTypes = { RL_PLACEHOLDER_TITLE, RL_PLACEHOLDER_CONTENT, RL_PLACEHOLDER_TEXT,
														RL_PLACEHOLDER_IMAGE, RL_PLACEHOLDER_VIDEO, RL_PLACEHOLDER_QUESTION,
														RL_PLACEHOLDER_FOOTER_DATE, RL_PLACEHOLDER_FOOTER_TEXT, RL_PLACEHOLDER_FOOTER_SLIDE_NUMBER,
														RL_PLACEHOLDER_REPORT, RL_PLACEHOLDER_MEDIA, RL_PLACEHOLDER_BUTTON };
						for (RLSlide* pLayout : lsLayouts)
						{
							for (RLPlaceholderType placeholderType : lsPlaceholderTypes)
							{
								if (pSlideMaster->GetType() != SLIDE_CONTAINER_FEEDBACK && placeholderType == RL_PLACEHOLDER_BUTTON)
									continue;
								RLControlList lsPlaceholdersInLayout = pLayout->GetPlaceholders(placeholderType);
								for (RLControl* pPlaceholder : lsPlaceholdersInLayout)
								{
									if (placeholderType != RL_PLACEHOLDER_BUTTON && placeholderType != RL_PLACEHOLDER_QUESTION && placeholderType != RL_PLACEHOLDER_CONTENT) // Some have default events
										lsEventsCheck.push_back(pPlaceholder);
									lsAnimsCheck.push_back(pPlaceholder);
								}
							}

							// Slide events
							wxASSERT(pLayout->HasInheritFlag(RL_SLIDE_INHERIT_EVENTS));
							wxASSERT(pLayout->HasInheritFlag(RL_SLIDE_INHERIT_COLOR_SCHEME));
							wxASSERT(pLayout->HasInheritFlag(RL_SLIDE_INHERIT_FONT_SCHEME));
							wxASSERT(pLayout->HasInheritFlag(RL_SLIDE_INHERIT_EVENTS));
							if (pLayout->GetFeedbackID() != RLSlide::FEEDBACK_REVIEW)
							{
								wxArrayString arrTemplateNotInheritBackgrounds;
								arrTemplateNotInheritBackgrounds.Add("Title Slide");
								arrTemplateNotInheritBackgrounds.Add("Content and Image");
								arrTemplateNotInheritBackgrounds.Add("Title and Content");
								arrTemplateNotInheritBackgrounds.Add("Title Only");

								std::function<bool()> isTemplateNotRequireInheritBackground = [&]() -> bool 
								{
									wxString currentLayoutName = pLayout->GetSlideName();
									for (size_t i = 0; i < arrTemplateNotInheritBackgrounds.GetCount(); i++)
									{
										if (currentLayoutName.StartsWith(arrTemplateNotInheritBackgrounds[i]))
										{
											return true;
										}
									}
									return false;
								};

								if (!isTemplateNotRequireInheritBackground()) 
								{
									wxASSERT(pLayout->HasInheritFlag(RL_SLIDE_INHERIT_BACKGROUND));
								}
							}
						}
						for (RLControl* pControl : lsAnimsCheck)
						{
							wxASSERT(pControl->HasFlag(RLControl::FLG_INHERITE_ANIMATIONS));
						}
						for (RLControl* pControl : lsEventsCheck)
						{
							wxASSERT(pControl->HasFlag(RLControl::FLG_INHERITE_EVENTS));
						}
					}
#endif
				}
			}
			RL_SCOPED_MUTEX(m_csTemplate);
			if (pProject)
			{
				m_lsTemplates.push_back(pProject);
				DoSortTemplates();
				for (wxEvtHandler* evtHanlder : m_themeEvtHandlers)
				{
					wxCommandEvent* pEvt = new wxCommandEvent(RLEVT_THEME_LOADED);
					pEvt->SetString(strFilePath);
					evtHanlder->QueueEvent(pEvt);
				}
			}
			else
			{
				rlRemovePath(strFilePath, m_lsOpenedTemplates);
			}
			m_nLoadedThemeCount++;
		});
	}
}


int RLTemplateManager::GetColorCount(int nType /*= -1*/) const
{
	RL_SCOPED_MUTEX(m_csTemplate);
	if (nType == -1)
		return m_lsColorSchemes.size();
	int nCount = 0;
	for (std::list<RLColorScheme*>::const_iterator it = m_lsColorSchemes.begin(); it != m_lsColorSchemes.end(); it++)
	{
		const RLColorScheme* pColor = *it;
		if (nType == (int)pColor->GetType())
			nCount++;
	}
	return nCount;
}

int RLTemplateManager::GetFontCount(int nType /*= -1*/) const
{
	RL_SCOPED_MUTEX(m_csTemplate);
	if (nType == -1)
		return m_lsFontSchemes.size();
	int nCount = 0;
	for (std::list<RLFontScheme*>::const_iterator it = m_lsFontSchemes.begin(); it != m_lsFontSchemes.end(); it++)
	{
		const RLFontScheme* pFont = *it;
		if (nType == (int)pFont->GetType())
			nCount++;
	}
	return nCount;
}


void RLTemplateManager::AddToColorList(RLColorScheme* pColor)
{
	// Built-In colors are at the beginning of the list
	RL_SCOPED_MUTEX(m_csTemplate);
	m_lsColorSchemes.push_back(pColor);
}

void RLTemplateManager::AddToFontList(RLFontScheme* pFont)
{
	// Built-In fonts are at the beginning of the list
	RL_SCOPED_MUTEX(m_csTemplate);
	m_lsFontSchemes.push_back(pFont);
}

RLProject * RLTemplateManager::GetResourceLibProject() const
{
	return m_pIconsProj;
}

void RLTemplateManager::SetResourceLibProject(RLProject* pProject)
{
	m_pIconsProj = pProject;
}

void RLTemplateManager::ClearGraphicsResources()
{
	RL_SCOPED_MUTEX(m_csTemplate);
	if (m_pResourceManager)
		m_pResourceManager->ClearThumbnails();
	if (m_pIconsProj)
		m_pIconsProj->ClearCachedGraphics();
	for (RLProject* pProject : m_lsTemplates)
		pProject->ClearCachedGraphics();
	m_thumbnailLoader->ClearGraphicsResources();
}

void RLTemplateManager::AddThemeEvtHandler(wxEvtHandler* evtHandler)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	if (!m_thumbnailLoader)
		return;
	m_themeEvtHandlers.push_back(evtHandler);
	m_thumbnailLoader->AddEvtHandler(evtHandler);
}


void RLTemplateManager::RemoveThemeEvtHandler(wxEvtHandler* evtHandler)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	if (!m_thumbnailLoader)
		return;
	m_themeEvtHandlers.remove(evtHandler);
	m_thumbnailLoader->RemoveEvtHandler(evtHandler);
}

wxBitmap RLTemplateManager::GetTemplateThumbnail(const RLTemplateInfo& templateInfo, const wxSize& szThumbnailLogicalSize, double dScaleFactor)
{
	RL_SCOPED_MUTEX(m_csTemplate);
	if (!m_thumbnailLoader)
		return wxNullBitmap;
	return m_thumbnailLoader->GetThumbnail(templateInfo, szThumbnailLogicalSize, dScaleFactor);
}

wxBitmap RLTemplateManager::GetTemplateDefaultThumbnail(const wxSize& szThumbnailLogicalSize, double dScaleFactor)
{
	wxSize szPhysicalSize = szThumbnailLogicalSize * dScaleFactor;
	wxImage imageEmpty(szPhysicalSize);
	wxRect rc(0, 0, szPhysicalSize.x, szPhysicalSize.y);
	imageEmpty.SetRGB(rc, 127, 127, 127);
	rc.Deflate(1);
	imageEmpty.SetRGB(rc, 255, 255, 255);
	wxBitmap bmp(imageEmpty);
	bmp.SetScaleFactor(dScaleFactor);
	return bmp;
}

wxDEFINE_EVENT(RLEVT_THEME_LOADED, wxCommandEvent);
wxDEFINE_EVENT(RLEVT_THEME_ADDED, wxCommandEvent);
wxDEFINE_EVENT(RLEVT_THEME_REMOVED, wxCommandEvent);
wxDEFINE_EVENT(RLEVT_THEME_THUMBNAIL_GENERATED, wxCommandEvent);
wxDEFINE_EVENT(RLEVT_THEME_DEFAULT_CHANGED, wxCommandEvent);

RLTemplateThumbnailLoader::RLTemplateThumbnailLoader()
{
	wxJSONValue jvFileUuidMap = RLConfig::Get()->GetOptionAsJSON("themes.thumbnails");
	for (wxString strFile : jvFileUuidMap.GetMemberNames())
	{
		m_thumbnailFileThemeUuidMap[strFile] = jvFileUuidMap.Get(strFile, "").AsString();
	}

	m_canceled = false;
	m_thread = new std::thread([&]() {
		while (true)
		{
			std::tuple<RLTemplateInfo, wxSize, double> task;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_condition.wait(lock, [this] { return (m_canceled || m_tasks.size() > 0); });
				if (m_canceled)
					return;
				task = m_tasks.front();
			}

			wxString strTemplate = std::get<0>(task).m_strFilePath;
			wxString strUUID = std::get<0>(task).m_strUUID;
			wxSize szThumbnailSize = std::get<1>(task);
			double dScaleFactor = std::get<2>(task);
			bool shouldCloseProject = false;

			RLProject* pProject = RLTemplateManager::Get()->GetTemplate(strTemplate);

			//Note: we don't always open slide template while running the program
			if (!pProject && strTemplate.EndsWith(RL_SLIDE_TEMPLATE_FILE_EXT_WITH_DOT))
			{
				pProject = RLTemplateManager::Get()->OpenTemplate(strTemplate);
				shouldCloseProject = pProject && !RLTemplateManager::IsBuiltInThemePath(pProject->GetFilePath());
			}
			if (pProject)
			{
				{
					std::unique_lock<std::mutex> lock(m_mutex);
					m_tasks.pop_front();
				}

				wxBitmap bmp;
				if (pProject->GetFilePath().EndsWith(RL_THEME_FILE_EXT_WITH_DOT))
					bmp = pProject->GetBitmap(szThumbnailSize, dScaleFactor);
				else
					bmp = GetFirstSlideThumbnail(pProject, szThumbnailSize, dScaleFactor);

				if (shouldCloseProject)
				{
					RLTemplateManager::Get()->CloseTemplate(pProject, false);
				}

				if (bmp.IsOk())
				{
					//Note: Should also lock writing to temporary file
					std::unique_lock<std::mutex> lock(m_mutex);

					wxString strThumbnailPath = GetThumnailPath(strTemplate, strUUID, szThumbnailSize, dScaleFactor);
					bmp.SaveFile(strThumbnailPath, wxBITMAP_TYPE_PNG);
					
					m_thumbnailsCache.Insert(strThumbnailPath, bmp);
					m_thumbnailFileThemeUuidMap[strThumbnailPath] = strUUID;

					for (wxEvtHandler* evtHandler : m_evtHandlers)
					{
						wxCommandEvent* pEvt = new wxCommandEvent(RLEVT_THEME_THUMBNAIL_GENERATED);
						pEvt->SetString(strTemplate);
						evtHandler->QueueEvent(pEvt);
					}
				}
				else
				{
					wxASSERT(false);
				}
			}
		}
	});
}

RLTemplateThumbnailLoader::~RLTemplateThumbnailLoader()
{
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		if (m_canceled)
			return;
		m_canceled = true;
		m_condition.notify_all();
	}
	m_thread->join();
	wxDELETE(m_thread);

	wxJSONValue jvFileUuidMap(wxJSONTYPE_OBJECT);
	std::map<wxString, wxString>::const_iterator it, end;
	for (it = m_thumbnailFileThemeUuidMap.begin(), end = m_thumbnailFileThemeUuidMap.end(); it != end; ++it)
	{
		jvFileUuidMap[it->first] = it->second;
	}

	RLConfig::Get()->SaveOption("themes.thumbnails", jvFileUuidMap);
}

wxString RLTemplateThumbnailLoader::GetThumnailPath(const wxString& strThemePath, const wxString& strUUID, const wxSize& sz, double dScaleFactor)
{
	return RLTemplateManager::GetThumnailPath(strThemePath, strUUID, sz, dScaleFactor);
}

wxBitmap RLTemplateThumbnailLoader::GetThumbnail(const RLTemplateInfo& templateInfo, const wxSize& size, double dScaleFactor)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	wxASSERT_MSG(fabs(dScaleFactor - wxRound(dScaleFactor)) < 0.01, "Odd content scale factor should not be occurred");
	wxASSERT(!templateInfo.m_strUUID.empty());

	wxString strThumbnailPath = GetThumnailPath(templateInfo.m_strFilePath, templateInfo.m_strUUID, size, dScaleFactor);
	
	std::map<wxString, wxString>::iterator it = m_thumbnailFileThemeUuidMap.find(strThumbnailPath);
	if (it != m_thumbnailFileThemeUuidMap.end() && it->second != templateInfo.m_strUUID)
	{
		m_thumbnailsCache.Remove(strThumbnailPath);
		if (wxFileExists(strThumbnailPath))
			wxRemoveFile(strThumbnailPath);
	}

	if (m_thumbnailsCache.Exists(strThumbnailPath))
	{
		return m_thumbnailsCache.Fetch(strThumbnailPath);
	}
	else
	{
		if (wxFileExists(strThumbnailPath))
		{
			wxBitmap bmp(strThumbnailPath, wxBITMAP_TYPE_PNG);
			bmp.SetScaleFactor(dScaleFactor);
			m_thumbnailsCache.Insert(strThumbnailPath, bmp);
			return bmp;
		}

		//Note: we don't always open slide template while running the program
		if (templateInfo.m_strFilePath.EndsWith(RL_SLIDE_TEMPLATE_FILE_EXT_WITH_DOT))
		{
			auto it = std::find_if(m_tasks.begin(), m_tasks.end(), [&templateInfo](const std::tuple<RLTemplateInfo, wxSize, double>& task) {
				return std::get<0>(task).m_strFilePath == templateInfo.m_strFilePath;
			});
			if (it == m_tasks.end())
			{
				m_tasks.push_back({ templateInfo, size, dScaleFactor });
				m_condition.notify_all();
			}
		}
		else
		{
			RLProject* pProject = RLTemplateManager::Get()->GetTemplate(templateInfo.m_strFilePath);
			if (pProject)
			{
				auto it = std::find_if(m_tasks.begin(), m_tasks.end(), [&templateInfo](const std::tuple<RLTemplateInfo, wxSize, double>& task) {
					return std::get<0>(task).m_strFilePath == templateInfo.m_strFilePath;
				});
				if (it == m_tasks.end())
				{
					m_tasks.push_back({ templateInfo, size, dScaleFactor });
					m_condition.notify_all();
				}
			}
		}

		return wxNullBitmap;
	}
}

wxBitmap RLTemplateThumbnailLoader::GetFirstSlideThumbnail(RLProject* pProject, const wxSize& szSize, double dScaleFactor)
{
	if (szSize.x <= 0 || szSize.y <= 0)
	{
		wxASSERT(false);
		return wxNullBitmap;
	}
	if (!pProject->GetMainSlideContainer()->GetSlides().size())
	{		
		return pProject->GetBitmap(szSize, dScaleFactor);
	}
	RLSlide* pLayout = pProject->GetMainSlideContainer()->GetSlides().front();
	wxBitmap bmSlide = wxNullBitmap;
	if (pLayout && pLayout->GetProject() == pProject)
	{
		int nLayoutIndex = pProject->GetCurLayoutIndex();
		int nViewportWidth = pProject->GetViewportWidth(nLayoutIndex);
		pProject->SetViewportWidth(-1);
		wxON_BLOCK_EXIT0([&]() {
			pProject->SetViewportWidth(nViewportWidth);
		});

		// #13540
		pLayout->Layout(nLayoutIndex);

		wxSize szSlide = pLayout->GetLayoutSize(pProject->GetCurLayoutIndex());
		int nSlideW = szSlide.GetWidth();
		int nSlideH = szSlide.GetHeight();
		wxGraphicsBitmap gbmSuface = rlMakeBitmap(nSlideW, nSlideH);

		wxScopedPtr<wxGraphicsContext> scopedCtxOriginalScale(rlCreateRenderingContext(gbmSuface));
		if (scopedCtxOriginalScale.get())
		{
			RLSlideRenderer renderer(pLayout, true);
			//Don't render drop shadow in thumbnail for high speed (like PPT)
			renderer.SetRenderFlags(RENDER_ALL & ~(RENDER_GRAPHICS_EFFECTS | RENDER_VIDEO_SYNC | RENDER_INVISIBLE | RENDER_SPELL_CHECKER | RENDER_SHOW_ALL_MODE | RENDER_LARGEST_LAYOUT));
			renderer.Render(scopedCtxOriginalScale.get());
			scopedCtxOriginalScale.reset();
			int nFitWidth = szSize.x;
			int nFitHeight = szSize.y;

			bmSlide.CreateScaled(nFitWidth, nFitHeight, 32, dScaleFactor);
			bmSlide.UseAlpha();
			wxMemoryDC memDC(bmSlide);
			if (!memDC.IsOk())
				return wxNullBitmap;

			wxScopedPtr<wxGraphicsContext> scopedCtx(rlCreateRenderingContext(memDC));
			if (!scopedCtx.get())
				return wxNullBitmap;
			double dCornerR = wxWindow::FromDIP(4, NULL);
			wxGraphicsPath clipPath = scopedCtx->CreatePath();
			clipPath.AddRoundedRectangle(0.5, 0.5, nFitWidth - 1, nFitHeight - 1, dCornerR);
			rlClipPath(scopedCtx.get(), clipPath);

			//Draw bitmap with scale make a better result than rendering directly with scale
			scopedCtx->SetInterpolationQuality(wxINTERPOLATION_BEST);
			scopedCtx->DrawBitmap(gbmSuface, 0, 0, nFitWidth, nFitHeight);
			wxGraphicsContext* pCtx = scopedCtx.get();

			pCtx->SetBrush(wxNullGraphicsBrush);
			pCtx->SetPen(rlMakePen(0xFFE0E0E0));
			pCtx->EnableOffset(false);
			rlUnclip(pCtx);

			pCtx->DrawRoundedRectangle(0.5, 0.5, nFitWidth - 1, nFitHeight - 1, dCornerR);

			scopedCtx.reset();
			memDC.SelectObject(wxNullBitmap);
		}
	}
	return bmSlide;
}

void RLTemplateThumbnailLoader::ClearThumbnail(const wxString& strUUID)
{
	wxASSERT(false);
}

void RLTemplateThumbnailLoader::ClearGraphicsResources()
{
	std::unique_lock<std::mutex> lock(m_mutex);
	m_thumbnailsCache.Clear();
}

void RLTemplateThumbnailLoader::AddEvtHandler(wxEvtHandler* evtHandler)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	m_evtHandlers.push_front(evtHandler);
}

void RLTemplateThumbnailLoader::RemoveEvtHandler(wxEvtHandler* evtHandler)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	m_evtHandlers.remove(evtHandler);
}

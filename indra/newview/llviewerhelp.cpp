/** 
 * @file llviewerhelp.cpp
 * @brief Utility functions for the Help system
 * @author Tofu Linden
 *
 * $LicenseInfo:firstyear=2009&license=viewergpl$
 * 
 * Copyright (c) 2009, Linden Research, Inc.
 * 
 * Second Life Viewer Source Code
 * The source code in this file ("Source Code") is provided by Linden Lab
 * to you under the terms of the GNU General Public License, version 2.0
 * ("GPL"), unless you have obtained a separate licensing agreement
 * ("Other License"), formally executed by you and Linden Lab.  Terms of
 * the GPL can be found in doc/GPL-license.txt in this distribution, or
 * online at http://secondlifegrid.net/programs/open_source/licensing/gplv2
 * 
 * There are special exceptions to the terms and conditions of the GPL as
 * it is applied to this Source Code. View the full text of the exception
 * in the file doc/FLOSS-exception.txt in this software distribution, or
 * online at
 * http://secondlifegrid.net/programs/open_source/licensing/flossexception
 * 
 * By copying, modifying or distributing this software, you acknowledge
 * that you have read and understood your obligations described above,
 * and agree to abide by those obligations.
 * 
 * ALL LINDEN LAB SOURCE CODE IS PROVIDED "AS IS." LINDEN LAB MAKES NO
 * WARRANTIES, EXPRESS, IMPLIED OR OTHERWISE, REGARDING ITS ACCURACY,
 * COMPLETENESS OR PERFORMANCE.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llfloaterhelpbrowser.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llviewercontrol.h"
#include "llversionviewer.h"
#include "llappviewer.h"

#include "llviewerhelputil.h"
#include "llviewerhelp.h"


//////////////////////////////
// implement LLHelp interface

void LLViewerHelp::showTopic(const std::string &topic)
{
	showHelp();
	
	if( gSavedSettings.getBOOL("HelpUseLocal") )
	{
		LLFloaterHelpBrowser* helpbrowser = dynamic_cast<LLFloaterHelpBrowser*>(LLFloaterReg::getInstance("help_browser"));
		helpbrowser->navigateToLocalPage( "help-offline" , "index.html" );
	}
	else 
	{
		const LLOSInfo& osinfo = LLAppViewer::instance()->getOSInfo();
		std::string helpURL = LLViewerHelpUtil::buildHelpURL( topic, gSavedSettings, osinfo );
		setRawURL( helpURL );
	}
}

std::string LLViewerHelp::defaultTopic()
{
	// *hack: to be done properly
	return "this_is_fallbacktopic";
}

//////////////////////////////
// our own interfaces

std::string LLViewerHelp::getTopicFromFocus()
{
	// use UI element with viewer's keyboard focus as basis for searching
	LLUICtrl* focused = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());

	if (focused)
	{
		std::string topic;
		if (focused->findHelpTopic(topic))
		{
			return topic;
		}
	}

	// didn't find a help topic in the UI hierarchy for focused
	// element, return the fallback topic name instead.
	return defaultTopic();
}

// static 
void LLViewerHelp::showHelp()
{
	LLFloaterHelpBrowser* helpbrowser = dynamic_cast<LLFloaterHelpBrowser*>(LLFloaterReg::getInstance("help_browser"));
	if (helpbrowser)
	{
		BOOL visible = TRUE;
		BOOL take_focus = TRUE;
		helpbrowser->setVisible(visible);
		helpbrowser->setFrontmost(take_focus);
	}
	else
	{
		llwarns << "Eep, help_browser floater not found" << llendl;
	}
}

// static
void LLViewerHelp::setRawURL(std::string url)
{
	LLFloaterHelpBrowser* helpbrowser = dynamic_cast<LLFloaterHelpBrowser*>(LLFloaterReg::getInstance("help_browser"));
	if (helpbrowser)
	{
		helpbrowser->openMedia(url);	
	}
	else
	{
		llwarns << "Eep, help_browser floater not found" << llendl;
	}
}

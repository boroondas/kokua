/**
 * @file llviewermedia.cpp
 * @brief Client interface to the media engine
 *
 * $LicenseInfo:firstyear=2007&license=viewergpl$
 * 
 * Copyright (c) 2007-2009, Linden Research, Inc.
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

#include "llviewermedia.h"
#include "llviewermediafocus.h"
#include "llmimetypes.h"
#include "llmediaentry.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewerparcelmedia.h"
#include "llviewerparcelmgr.h"
#include "llversionviewer.h"
#include "llviewertexturelist.h"
#include "llvovolume.h"
#include "llpluginclassmedia.h"

#include "llevent.h"		// LLSimpleListener
#include "llnotifications.h"
#include "lluuid.h"

#include <boost/bind.hpp>	// for SkinFolder listener
#include <boost/signals2.hpp>

// Move this to its own file.

LLViewerMediaEventEmitter::~LLViewerMediaEventEmitter()
{
	observerListType::iterator iter = mObservers.begin();

	while( iter != mObservers.end() )
	{
		LLViewerMediaObserver *self = *iter;
		iter++;
		remObserver(self);
	}
}

///////////////////////////////////////////////////////////////////////////////
//
bool LLViewerMediaEventEmitter::addObserver( LLViewerMediaObserver* observer )
{
	if ( ! observer )
		return false;

	if ( std::find( mObservers.begin(), mObservers.end(), observer ) != mObservers.end() )
		return false;

	mObservers.push_back( observer );
	observer->mEmitters.push_back( this );

	return true;
}

///////////////////////////////////////////////////////////////////////////////
//
bool LLViewerMediaEventEmitter::remObserver( LLViewerMediaObserver* observer )
{
	if ( ! observer )
		return false;

	mObservers.remove( observer );
	observer->mEmitters.remove(this);

	return true;
}

///////////////////////////////////////////////////////////////////////////////
//
void LLViewerMediaEventEmitter::emitEvent( LLPluginClassMedia* media, LLViewerMediaObserver::EMediaEvent event )
{
	// Broadcast the event to any observers.
	observerListType::iterator iter = mObservers.begin();
	while( iter != mObservers.end() )
	{
		LLViewerMediaObserver *self = *iter;
		++iter;
		self->handleMediaEvent( media, event );
	}
}

// Move this to its own file.
LLViewerMediaObserver::~LLViewerMediaObserver()
{
	std::list<LLViewerMediaEventEmitter *>::iterator iter = mEmitters.begin();

	while( iter != mEmitters.end() )
	{
		LLViewerMediaEventEmitter *self = *iter;
		iter++;
		self->remObserver( this );
	}
}


// Move this to its own file.
// helper class that tries to download a URL from a web site and calls a method
// on the Panel Land Media and to discover the MIME type
class LLMimeDiscoveryResponder : public LLHTTPClient::Responder
{
LOG_CLASS(LLMimeDiscoveryResponder);
public:
	LLMimeDiscoveryResponder( viewer_media_t media_impl)
		: mMediaImpl(media_impl),
		  mInitialized(false)
	{}



	virtual void completedHeader(U32 status, const std::string& reason, const LLSD& content)
	{
		std::string media_type = content["content-type"].asString();
		std::string::size_type idx1 = media_type.find_first_of(";");
		std::string mime_type = media_type.substr(0, idx1);
		completeAny(status, mime_type);
	}

	virtual void error( U32 status, const std::string& reason )
	{
		// completeAny(status, "none/none");
	}

	void completeAny(U32 status, const std::string& mime_type)
	{
		if(!mInitialized && ! mime_type.empty())
		{
			if (mMediaImpl->initializeMedia(mime_type))
			{
				mInitialized = true;
				mMediaImpl->play();
			}
		}
	}

	public:
		viewer_media_t mMediaImpl;
		bool mInitialized;
};
typedef std::vector<LLViewerMediaImpl*> impl_list;
static impl_list sViewerMediaImplList;
static LLTimer sMediaCreateTimer;
static const F32 LLVIEWERMEDIA_CREATE_DELAY = 1.0f;

//////////////////////////////////////////////////////////////////////////////////////////
static void add_media_impl(LLViewerMediaImpl* media)
{
	sViewerMediaImplList.push_back(media);
}

//////////////////////////////////////////////////////////////////////////////////////////
static void remove_media_impl(LLViewerMediaImpl* media)
{
	impl_list::iterator iter = sViewerMediaImplList.begin();
	impl_list::iterator end = sViewerMediaImplList.end();
	
	for(; iter != end; iter++)
	{
		if(media == *iter)
		{
			sViewerMediaImplList.erase(iter);
			return;
		}
	}
}


//////////////////////////////////////////////////////////////////////////////////////////
// LLViewerMedia

//////////////////////////////////////////////////////////////////////////////////////////
// static
viewer_media_t LLViewerMedia::newMediaImpl(
											 const LLUUID& texture_id,
											 S32 media_width, 
											 S32 media_height, 
											 U8 media_auto_scale,
											 U8 media_loop)
{
	LLViewerMediaImpl* media_impl = getMediaImplFromTextureID(texture_id);
	if(media_impl == NULL || texture_id.isNull())
	{
		// Create the media impl
		media_impl = new LLViewerMediaImpl(texture_id, media_width, media_height, media_auto_scale, media_loop);
	}
	else
	{
		media_impl->stop();
		media_impl->mTextureId = texture_id;
		media_impl->mMediaWidth = media_width;
		media_impl->mMediaHeight = media_height;
		media_impl->mMediaAutoScale = media_auto_scale;
		media_impl->mMediaLoop = media_loop;
	}

	return media_impl;
}

viewer_media_t LLViewerMedia::updateMediaImpl(LLMediaEntry* media_entry, const std::string& previous_url, bool update_from_self)
{	
	// Try to find media with the same media ID
	viewer_media_t media_impl = getMediaImplFromTextureID(media_entry->getMediaID());

	bool was_loaded = false;
	bool needs_navigate = false;
	
	if(media_impl)
	{	
		was_loaded = media_impl->hasMedia();
		
		media_impl->setHomeURL(media_entry->getHomeURL());
		
		media_impl->mMediaAutoScale = media_entry->getAutoScale();
		media_impl->mMediaLoop = media_entry->getAutoLoop();
		media_impl->mMediaWidth = media_entry->getWidthPixels();
		media_impl->mMediaHeight = media_entry->getHeightPixels();
		if (media_impl->mMediaSource)
		{
			media_impl->mMediaSource->setAutoScale(media_impl->mMediaAutoScale);
			media_impl->mMediaSource->setLoop(media_impl->mMediaLoop);
			media_impl->mMediaSource->setSize(media_entry->getWidthPixels(), media_entry->getHeightPixels());
		}
		
		if((was_loaded || media_entry->getAutoPlay()) && !update_from_self)
		{
			if(!media_entry->getCurrentURL().empty())
			{
				needs_navigate = (media_entry->getCurrentURL() != previous_url);
			}
			else if(!media_entry->getHomeURL().empty())
			{
				needs_navigate = (media_entry->getHomeURL() != previous_url);
			}
		}
	}
	else
	{
		media_impl = newMediaImpl(
			media_entry->getMediaID(), 
			media_entry->getWidthPixels(),
			media_entry->getHeightPixels(), 
			media_entry->getAutoScale(), 
			media_entry->getAutoLoop());
		
		media_impl->setHomeURL(media_entry->getHomeURL());
		
		if(media_entry->getAutoPlay())
		{
			needs_navigate = true;
		}
	}
	
	if(media_impl && needs_navigate)
	{
		std::string url = media_entry->getCurrentURL();
		if(url.empty())
			url = media_entry->getHomeURL();
			
		media_impl->navigateTo(url, "", true, true);
	}
	
	return media_impl;
}

//////////////////////////////////////////////////////////////////////////////////////////
// static
LLViewerMediaImpl* LLViewerMedia::getMediaImplFromTextureID(const LLUUID& texture_id)
{
	impl_list::iterator iter = sViewerMediaImplList.begin();
	impl_list::iterator end = sViewerMediaImplList.end();

	for(; iter != end; iter++)
	{
		LLViewerMediaImpl* media_impl = *iter;
		if(media_impl->getMediaTextureID() == texture_id)
		{
			return media_impl;
		}
	}
	return NULL;
}

//////////////////////////////////////////////////////////////////////////////////////////
// static
std::string LLViewerMedia::getCurrentUserAgent()
{
	// Don't use user-visible string to avoid 
	// punctuation and strange characters.
	std::string skin_name = gSavedSettings.getString("SkinCurrent");

	// Just in case we need to check browser differences in A/B test
	// builds.
	std::string channel = gSavedSettings.getString("VersionChannelName");

	// append our magic version number string to the browser user agent id
	// See the HTTP 1.0 and 1.1 specifications for allowed formats:
	// http://www.ietf.org/rfc/rfc1945.txt section 10.15
	// http://www.ietf.org/rfc/rfc2068.txt section 3.8
	// This was also helpful:
	// http://www.mozilla.org/build/revised-user-agent-strings.html
	std::ostringstream codec;
	codec << "SecondLife/";
	codec << LL_VERSION_MAJOR << "." << LL_VERSION_MINOR << "." << LL_VERSION_PATCH << "." << LL_VERSION_BUILD;
	codec << " (" << channel << "; " << skin_name << " skin)";
	llinfos << codec.str() << llendl;
	
	return codec.str();
}

//////////////////////////////////////////////////////////////////////////////////////////
// static
void LLViewerMedia::updateBrowserUserAgent()
{
	std::string user_agent = getCurrentUserAgent();
	
	impl_list::iterator iter = sViewerMediaImplList.begin();
	impl_list::iterator end = sViewerMediaImplList.end();

	for(; iter != end; iter++)
	{
		LLViewerMediaImpl* pimpl = *iter;
		if(pimpl->mMediaSource && pimpl->mMediaSource->pluginSupportsMediaBrowser())
		{
			pimpl->mMediaSource->setBrowserUserAgent(user_agent);
		}
	}

}

//////////////////////////////////////////////////////////////////////////////////////////
// static
bool LLViewerMedia::handleSkinCurrentChanged(const LLSD& /*newvalue*/)
{
	// gSavedSettings is already updated when this function is called.
	updateBrowserUserAgent();
	return true;
}

//////////////////////////////////////////////////////////////////////////////////////////
// static
bool LLViewerMedia::textureHasMedia(const LLUUID& texture_id)
{
	impl_list::iterator iter = sViewerMediaImplList.begin();
	impl_list::iterator end = sViewerMediaImplList.end();

	for(; iter != end; iter++)
	{
		LLViewerMediaImpl* pimpl = *iter;
		if(pimpl->getMediaTextureID() == texture_id)
		{
			return true;
		}
	}
	return false;
}

//////////////////////////////////////////////////////////////////////////////////////////
// static
void LLViewerMedia::setVolume(F32 volume)
{
	impl_list::iterator iter = sViewerMediaImplList.begin();
	impl_list::iterator end = sViewerMediaImplList.end();

	for(; iter != end; iter++)
	{
		LLViewerMediaImpl* pimpl = *iter;
		pimpl->setVolume(volume);
	}
}

// This is the predicate function used to sort sViewerMediaImplList by priority.
static inline bool compare_impl_interest(const LLViewerMediaImpl* i1, const LLViewerMediaImpl* i2)
{
	if(i1->hasFocus())
	{
		// The item with user focus always comes to the front of the list, period.
		return true;
	}
	else if(i2->hasFocus())
	{
		// The item with user focus always comes to the front of the list, period.
		return false;
	}
	else if(i1->getUsedInUI() && !i2->getUsedInUI())
	{
		// i1 is a UI element, i2 is not.  This makes i1 "less than" i2, so it sorts earlier in our list.
		return true;
	}
	else if(i2->getUsedInUI() && !i1->getUsedInUI())
	{
		// i2 is a UI element, i1 is not.  This makes i2 "less than" i1, so it sorts earlier in our list.
		return false;
	}
	else
	{
		// The object with the larger interest value should be earlier in the list, so we reverse the sense of the comparison here.
		return (i1->getInterest() > i2->getInterest());
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
// static
void LLViewerMedia::updateMedia()
{
	impl_list::iterator iter = sViewerMediaImplList.begin();
	impl_list::iterator end = sViewerMediaImplList.end();

	for(; iter != end; iter++)
	{
		LLViewerMediaImpl* pimpl = *iter;
		pimpl->update();
		pimpl->calculateInterest();
	}
		
	// Sort the static instance list using our interest criteria
	std::stable_sort(sViewerMediaImplList.begin(), sViewerMediaImplList.end(), compare_impl_interest);

	// Go through the list again and adjust according to priority.
	iter = sViewerMediaImplList.begin();
	end = sViewerMediaImplList.end();
	
	F64 total_cpu = 0.0f;
	int impl_count_total = 0;
	int impl_count_interest_low = 0;
	int impl_count_interest_normal = 0;

#if 0	
	LL_DEBUGS("PluginPriority") << "Sorted impls:" << llendl;
#endif

	U32 max_instances = gSavedSettings.getU32("PluginInstancesTotal");
	U32 max_normal = gSavedSettings.getU32("PluginInstancesNormal");
	U32 max_low = gSavedSettings.getU32("PluginInstancesLow");
	F32 max_cpu = gSavedSettings.getF32("PluginInstancesCPULimit");
	// Setting max_cpu to 0.0 disables CPU usage checking.
	bool check_cpu_usage = (max_cpu != 0.0f);
	
	// Notes on tweakable params:
	// max_instances must be set high enough to allow the various instances used in the UI (for the help browser, search, etc.) to be loaded.
	// If max_normal + max_low is less than max_instances, things will tend to get unloaded instead of being set to slideshow.
	
	for(; iter != end; iter++)
	{
		LLViewerMediaImpl* pimpl = *iter;
		
		LLPluginClassMedia::EPriority new_priority = LLPluginClassMedia::PRIORITY_NORMAL;

		if(impl_count_total > (int)max_instances)
		{
			// Hard limit on the number of instances that will be loaded at one time
			new_priority = LLPluginClassMedia::PRIORITY_UNLOADED;
		}
		else if(!pimpl->getVisible())
		{
			new_priority = LLPluginClassMedia::PRIORITY_HIDDEN;
		}
		else if(pimpl->hasFocus())
		{
			new_priority = LLPluginClassMedia::PRIORITY_HIGH;
		}
		else if(pimpl->getUsedInUI())
		{
			new_priority = LLPluginClassMedia::PRIORITY_NORMAL;
		}
		else
		{
			// Look at interest and CPU usage for instances that aren't in any of the above states.
			
			// Heuristic -- if the media texture's approximate screen area is less than 1/4 of the native area of the texture,
			// turn it down to low instead of normal.  This may downsample for plugins that support it.
			bool media_is_small = pimpl->getInterest() < (pimpl->getApproximateTextureInterest() / 4);
			
			if(pimpl->getInterest() == 0.0f)
			{
				// This media is completely invisible, due to being outside the view frustrum or out of range.
				new_priority = LLPluginClassMedia::PRIORITY_HIDDEN;
			}
			else if(check_cpu_usage && (total_cpu > max_cpu))
			{
				// Higher priority plugins have already used up the CPU budget.  Set remaining ones to slideshow priority.
				new_priority = LLPluginClassMedia::PRIORITY_SLIDESHOW;
			}
			else if((impl_count_interest_normal < (int)max_normal) && !media_is_small)
			{
				// Up to max_normal inworld get normal priority
				new_priority = LLPluginClassMedia::PRIORITY_NORMAL;
				impl_count_interest_normal++;
			}
			else if (impl_count_interest_low + impl_count_interest_normal < (int)max_low + (int)max_normal)
			{
				// The next max_low inworld get turned down
				new_priority = LLPluginClassMedia::PRIORITY_LOW;
				impl_count_interest_low++;
				
				// Set the low priority size for downsampling to approximately the size the texture is displayed at.
				{
					F32 approximate_interest_dimension = fsqrtf(pimpl->getInterest());
					
					pimpl->setLowPrioritySizeLimit(llround(approximate_interest_dimension));
				}
			}
			else
			{
				// Any additional impls (up to max_instances) get very infrequent time
				new_priority = LLPluginClassMedia::PRIORITY_SLIDESHOW;
			}
		}
		
		pimpl->setPriority(new_priority);

#if 0		
		LL_DEBUGS("PluginPriority") << "    " << pimpl 
			<< ", setting priority to " << new_priority
			<< (pimpl->hasFocus()?", HAS FOCUS":"") 
			<< (pimpl->getUsedInUI()?", is UI":"") 
			<< ", cpu " << pimpl->getCPUUsage() 
			<< ", interest " << pimpl->getInterest() 
			<< ", media url " << pimpl->getMediaURL() << llendl;
#endif

		total_cpu += pimpl->getCPUUsage();
		impl_count_total++;
	}
	
	LL_DEBUGS("PluginPriority") << "Total reported CPU usage is " << total_cpu << llendl;

}

//////////////////////////////////////////////////////////////////////////////////////////
// static
void LLViewerMedia::cleanupClass()
{
	// This is no longer necessary, since sViewerMediaImplList is no longer smart pointers.
}

//////////////////////////////////////////////////////////////////////////////////////////
// LLViewerMediaImpl
//////////////////////////////////////////////////////////////////////////////////////////
LLViewerMediaImpl::LLViewerMediaImpl(	  const LLUUID& texture_id, 
										  S32 media_width, 
										  S32 media_height, 
										  U8 media_auto_scale, 
										  U8 media_loop)
:	
	mMediaSource( NULL ),
	mMovieImageHasMips(false),
	mTextureId(texture_id),
	mMediaWidth(media_width),
	mMediaHeight(media_height),
	mMediaAutoScale(media_auto_scale),
	mMediaLoop(media_loop),
	mNeedsNewTexture(true),
	mSuspendUpdates(false),
	mVisible(true),
	mLastSetCursor( UI_CURSOR_ARROW ),
	mMediaNavState( MEDIANAVSTATE_NONE ),
	mInterest(0.0f),
	mUsedInUI(false),
	mHasFocus(false),
	mPriority(LLPluginClassMedia::PRIORITY_UNLOADED),
	mDoNavigateOnLoad(false),
	mDoNavigateOnLoadRediscoverType(false),
	mDoNavigateOnLoadServerRequest(false),
	mMediaSourceFailedInit(false),
	mIsUpdated(false)
{ 
	
	add_media_impl(this);
	
	// connect this media_impl to the media texture, creating it if it doesn't exist.0
	// This is necessary because we need to be able to use getMaxVirtualSize() even if the media plugin is not loaded.
	LLViewerMediaTexture* media_tex = LLViewerTextureManager::getMediaTexture(mTextureId);
	if(media_tex)
	{
		media_tex->setMediaImpl();
	}

}

//////////////////////////////////////////////////////////////////////////////////////////
LLViewerMediaImpl::~LLViewerMediaImpl()
{
	if( gEditMenuHandler == this )
	{
		gEditMenuHandler = NULL;
	}
	
	destroyMediaSource();
	
	LLViewerMediaTexture::removeMediaImplFromTexture(mTextureId) ;

	remove_media_impl(this);
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::emitEvent(LLPluginClassMedia* plugin, LLViewerMediaObserver::EMediaEvent event)
{
	// Broadcast to observers using the superclass version
	LLViewerMediaEventEmitter::emitEvent(plugin, event);
	
	// If this media is on one or more LLVOVolume objects, tell them about the event as well.
	std::list< LLVOVolume* >::iterator iter = mObjectList.begin() ;
	while(iter != mObjectList.end())
	{
		LLVOVolume *self = *iter;
		++iter;
		self->mediaEvent(this, plugin, event);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
bool LLViewerMediaImpl::initializeMedia(const std::string& mime_type)
{
	if((mMediaSource == NULL) || (mMimeType != mime_type))
	{
		if(! initializePlugin(mime_type))
		{
			// This may be the case where the plugin's priority is PRIORITY_UNLOADED
			return false;
		}
	}

	// play();
	return (mMediaSource != NULL);
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::createMediaSource()
{
	if(mPriority == LLPluginClassMedia::PRIORITY_UNLOADED)
	{
		// This media shouldn't be created yet.
		return;
	}
	
	if(mDoNavigateOnLoad)
	{
		if(! mMediaURL.empty())
		{
			navigateTo(mMediaURL, mMimeType, mDoNavigateOnLoadRediscoverType, mDoNavigateOnLoadServerRequest);
		}
		else if(! mMimeType.empty())
		{
			initializeMedia(mMimeType);
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::destroyMediaSource()
{
	mNeedsNewTexture = true;

	// Tell the viewer media texture it's no longer active
	LLViewerMediaTexture* oldImage = LLViewerTextureManager::findMediaTexture( mTextureId );
	if (oldImage)
	{
		oldImage->setPlaying(FALSE) ;
	}

	if(mMediaSource)
	{
		delete mMediaSource;
		mMediaSource = NULL;
	}	
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::setMediaType(const std::string& media_type)
{
	mMimeType = media_type;
}

//////////////////////////////////////////////////////////////////////////////////////////
/*static*/
LLPluginClassMedia* LLViewerMediaImpl::newSourceFromMediaType(std::string media_type, LLPluginClassMediaOwner *owner /* may be NULL */, S32 default_width, S32 default_height)
{
	std::string plugin_basename = LLMIMETypes::implType(media_type);
	
	if(plugin_basename.empty())
	{
		LL_WARNS("Media") << "Couldn't find plugin for media type " << media_type << LL_ENDL;
	}
	else
	{
		std::string plugins_path = gDirUtilp->getLLPluginDir();
		plugins_path += gDirUtilp->getDirDelimiter();
		
		std::string launcher_name = gDirUtilp->getLLPluginLauncher();
		std::string plugin_name = gDirUtilp->getLLPluginFilename(plugin_basename);

		// See if the plugin executable exists
		llstat s;
		if(LLFile::stat(launcher_name, &s))
		{
			LL_WARNS("Media") << "Couldn't find launcher at " << launcher_name << LL_ENDL;
		}
		else if(LLFile::stat(plugin_name, &s))
		{
			LL_WARNS("Media") << "Couldn't find plugin at " << plugin_name << LL_ENDL;
		}
		else
		{
			LLPluginClassMedia* media_source = new LLPluginClassMedia(owner);
			media_source->setSize(default_width, default_height);
			if (media_source->init(launcher_name, plugin_name))
			{
				return media_source;
			}
			else
			{
				LL_WARNS("Media") << "Failed to init plugin.  Destroying." << LL_ENDL;
				delete media_source;
			}
		}
	}
	
	LL_WARNS("Plugin") << "plugin intialization failed for mime type: " << media_type << LL_ENDL;
	LLSD args;
	args["MIME_TYPE"] = media_type;
	LLNotifications::instance().add("NoPlugin", args);

	return NULL;
}							

//////////////////////////////////////////////////////////////////////////////////////////
bool LLViewerMediaImpl::initializePlugin(const std::string& media_type)
{
	if(mMediaSource)
	{
		// Save the previous media source's last set size before destroying it.
		mMediaWidth = mMediaSource->getSetWidth();
		mMediaHeight = mMediaSource->getSetHeight();
	}
	
	// Always delete the old media impl first.
	destroyMediaSource();
	
	// and unconditionally set the mime type
	mMimeType = media_type;

	if(mPriority == LLPluginClassMedia::PRIORITY_UNLOADED)
	{
		// This impl should not be loaded at this time.
		LL_DEBUGS("PluginPriority") << this << "Not loading (PRIORITY_UNLOADED)" << LL_ENDL;
		
		return false;
	}

	// If we got here, we want to ignore previous init failures.
	mMediaSourceFailedInit = false;

	LLPluginClassMedia* media_source = newSourceFromMediaType(mMimeType, this, mMediaWidth, mMediaHeight);
	
	if (media_source)
	{
		media_source->setDisableTimeout(gSavedSettings.getBOOL("DebugPluginDisableTimeout"));
		media_source->setLoop(mMediaLoop);
		media_source->setAutoScale(mMediaAutoScale);
		media_source->setBrowserUserAgent(LLViewerMedia::getCurrentUserAgent());
		
		mMediaSource = media_source;
		return true;
	}

	// Make sure the timer doesn't try re-initing this plugin repeatedly until something else changes.
	mMediaSourceFailedInit = true;

	return false;
}

void LLViewerMediaImpl::setSize(int width, int height)
{
	mMediaWidth = width;
	mMediaHeight = height;
	if(mMediaSource)
	{
		mMediaSource->setSize(width, height);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::play()
{
	// first stop any previously playing media
	// stop();

	// mMediaSource->addObserver( this );
	if(mMediaSource == NULL)
	{
	 	if(!initializePlugin(mMimeType))
		{
			// This may be the case where the plugin's priority is PRIORITY_UNLOADED
			return;
		}
	}
	
	mMediaSource->loadURI( mMediaURL );
	if(/*mMediaSource->pluginSupportsMediaTime()*/ true)
	{
		start();
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::stop()
{
	if(mMediaSource)
	{
		if(mMediaSource->pluginSupportsMediaBrowser())
		{
			mMediaSource->browse_stop();
		}
		else
		{
			mMediaSource->stop();
		}

		// destroyMediaSource();
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::pause()
{
	if(mMediaSource)
	{
		mMediaSource->pause();
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::start()
{
	if(mMediaSource)
	{
		mMediaSource->start();
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::seek(F32 time)
{
	if(mMediaSource)
	{
		mMediaSource->seek(time);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::setVolume(F32 volume)
{
	if(mMediaSource)
	{
		mMediaSource->setVolume(volume);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::focus(bool focus)
{
	mHasFocus = focus;
	
	if (mMediaSource)
	{
		// call focus just for the hell of it, even though this apopears to be a nop
		mMediaSource->focus(focus);
		if (focus)
		{
			// spoof a mouse click to *actually* pass focus
			// Don't do this anymore -- it actually clicks through now.
//			mMediaSource->mouseEvent(LLPluginClassMedia::MOUSE_EVENT_DOWN, 1, 1, 0);
//			mMediaSource->mouseEvent(LLPluginClassMedia::MOUSE_EVENT_UP, 1, 1, 0);
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
bool LLViewerMediaImpl::hasFocus() const
{
	// FIXME: This might be able to be a bit smarter by hooking into LLViewerMediaFocus, etc.
	return mHasFocus;
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::mouseDown(S32 x, S32 y)
{
	scaleMouse(&x, &y);
	mLastMouseX = x;
	mLastMouseY = y;
//	llinfos << "mouse down (" << x << ", " << y << ")" << llendl;
	if (mMediaSource)
	{
		mMediaSource->mouseEvent(LLPluginClassMedia::MOUSE_EVENT_DOWN, x, y, 0);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::mouseUp(S32 x, S32 y)
{
	scaleMouse(&x, &y);
	mLastMouseX = x;
	mLastMouseY = y;
//	llinfos << "mouse up (" << x << ", " << y << ")" << llendl;
	if (mMediaSource)
	{
		mMediaSource->mouseEvent(LLPluginClassMedia::MOUSE_EVENT_UP, x, y, 0);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::mouseMove(S32 x, S32 y)
{
    scaleMouse(&x, &y);
	mLastMouseX = x;
	mLastMouseY = y;
//	llinfos << "mouse move (" << x << ", " << y << ")" << llendl;
	if (mMediaSource)
	{
		mMediaSource->mouseEvent(LLPluginClassMedia::MOUSE_EVENT_MOVE, x, y, 0);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::mouseDown(const LLVector2& texture_coords)
{
	if(mMediaSource)
	{		
		mouseDown(
			llround(texture_coords.mV[VX] * mMediaSource->getTextureWidth()),
			llround((1.0f - texture_coords.mV[VY]) * mMediaSource->getTextureHeight()));
	}
}

void LLViewerMediaImpl::mouseUp(const LLVector2& texture_coords)
{
	if(mMediaSource)
	{		
		mouseUp(
			llround(texture_coords.mV[VX] * mMediaSource->getTextureWidth()),
			llround((1.0f - texture_coords.mV[VY]) * mMediaSource->getTextureHeight()));
	}
}

void LLViewerMediaImpl::mouseMove(const LLVector2& texture_coords)
{
	if(mMediaSource)
	{		
		mouseMove(
			llround(texture_coords.mV[VX] * mMediaSource->getTextureWidth()),
			llround((1.0f - texture_coords.mV[VY]) * mMediaSource->getTextureHeight()));
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::mouseLeftDoubleClick(S32 x, S32 y)
{
	scaleMouse(&x, &y);
	mLastMouseX = x;
	mLastMouseY = y;
	if (mMediaSource)
	{
		mMediaSource->mouseEvent(LLPluginClassMedia::MOUSE_EVENT_DOUBLE_CLICK, x, y, 0);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::onMouseCaptureLost()
{
	if (mMediaSource)
	{
		mMediaSource->mouseEvent(LLPluginClassMedia::MOUSE_EVENT_UP, mLastMouseX, mLastMouseY, 0);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
BOOL LLViewerMediaImpl::handleMouseUp(S32 x, S32 y, MASK mask) 
{ 
	// NOTE: this is called when the mouse is released when we have capture.
	// Due to the way mouse coordinates are mapped to the object, we can't use the x and y coordinates that come in with the event.
	
	if(hasMouseCapture())
	{
		// Release the mouse -- this will also send a mouseup to the media
		gFocusMgr.setMouseCapture( FALSE );
	}

	return TRUE; 
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::navigateBack()
{
	if (mMediaSource)
	{
		if(mMediaSource->pluginSupportsMediaTime())
		{
			F64 step_scale = 0.02; // temp , can be changed
			F64 back_step = mMediaSource->getCurrentTime() - (mMediaSource->getDuration()*step_scale);
			if(back_step < 0.0)
			{
				back_step = 0.0;
			}
			mMediaSource->seek(back_step);
			//mMediaSource->start(-2.0);
		}
		else
		{
			mMediaSource->browse_back();
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::navigateForward()
{
	if (mMediaSource)
	{
		if(mMediaSource->pluginSupportsMediaTime())
		{
			F64 step_scale = 0.02; // temp , can be changed
			F64 forward_step = mMediaSource->getCurrentTime() + (mMediaSource->getDuration()*step_scale);
			if(forward_step > mMediaSource->getDuration())
			{
				forward_step = mMediaSource->getDuration();
			}
			mMediaSource->seek(forward_step);
			//mMediaSource->start(2.0);
		}
		else
		{
			mMediaSource->browse_forward();
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::navigateReload()
{
	navigateTo(mMediaURL, "", true, false);
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::navigateHome()
{
	navigateTo(mHomeURL, "", true, false);
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::navigateTo(const std::string& url, const std::string& mime_type,  bool rediscover_type, bool server_request)
{
	// Helpful to have media urls in log file. Shouldn't be spammy.
	llinfos << "url=" << url << " mime_type=" << mime_type << llendl;
	
	if(server_request)
	{
		setNavState(MEDIANAVSTATE_SERVER_SENT);
	}
	else
	{
		setNavState(MEDIANAVSTATE_NONE);
	}
	
	// Always set the current URL and MIME type.
	mMediaURL = url;
	mMimeType = mime_type;
	
	// If the current URL is not null, make the instance do a navigate on load.
	mDoNavigateOnLoad = !mMediaURL.empty();

	// if mime type discovery was requested, we'll need to do it when the media loads
	mDoNavigateOnLoadRediscoverType = rediscover_type;
	
	// and if this was a server request, the navigate on load will also need to be one.
	mDoNavigateOnLoadServerRequest = server_request;

	if(mPriority == LLPluginClassMedia::PRIORITY_UNLOADED)
	{
		// This impl should not be loaded at this time.
		LL_DEBUGS("PluginPriority") << this << "Not loading (PRIORITY_UNLOADED)" << LL_ENDL;
		
		return;
	}
	
	// If the caller has specified a non-empty MIME type, look that up in our MIME types list.
	// If we have a plugin for that MIME type, use that instead of attempting auto-discovery.
	// This helps in supporting legacy media content where the server the media resides on returns a bogus MIME type
	// but the parcel owner has correctly set the MIME type in the parcel media settings.
	
	if(!mMimeType.empty() && (mMimeType != "none/none"))
	{
		std::string plugin_basename = LLMIMETypes::implType(mMimeType);
		if(!plugin_basename.empty())
		{
			// We have a plugin for this mime type
			rediscover_type = false;
		}
	}

	if(rediscover_type)
	{

		LLURI uri(mMediaURL);
		std::string scheme = uri.scheme();

		if(scheme.empty() || "http" == scheme || "https" == scheme)
		{
			LLHTTPClient::getHeaderOnly( mMediaURL, new LLMimeDiscoveryResponder(this));
		}
		else if("data" == scheme || "file" == scheme || "about" == scheme)
		{
			// FIXME: figure out how to really discover the type for these schemes
			// We use "data" internally for a text/html url for loading the login screen
			if(initializeMedia("text/html"))
			{
				mMediaSource->loadURI( mMediaURL );
			}
		}
		else
		{
			// This catches 'rtsp://' urls
			if(initializeMedia(scheme))
			{
				mMediaSource->loadURI( mMediaURL );
			}
		}
	}
	else if (mMediaSource)
	{
		mMediaSource->loadURI( mMediaURL );
	}
	else if(initializeMedia(mime_type) && mMediaSource)
	{
		mMediaSource->loadURI( mMediaURL );
	}
	else
	{
		LL_WARNS("Media") << "Couldn't navigate to: " << url << " as there is no media type for: " << mime_type << LL_ENDL;
		return;
	}

}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::navigateStop()
{
	if(mMediaSource)
	{
		mMediaSource->browse_stop();
	}

}

//////////////////////////////////////////////////////////////////////////////////////////
bool LLViewerMediaImpl::handleKeyHere(KEY key, MASK mask)
{
	bool result = false;
	// *NOTE:Mani - if this doesn't exist llmozlib goes crashy in the debug build.
	// LLMozlib::init wants to write some files to <exe_dir>/components
	std::string debug_init_component_dir( gDirUtilp->getExecutableDir() );
	debug_init_component_dir += "/components";
	LLAPRFile::makeDir(debug_init_component_dir.c_str()); 
	
	if (mMediaSource)
	{
		result = mMediaSource->keyEvent(LLPluginClassMedia::KEY_EVENT_DOWN ,key, mask);
	}
	
	return result;
}

//////////////////////////////////////////////////////////////////////////////////////////
bool LLViewerMediaImpl::handleUnicodeCharHere(llwchar uni_char)
{
	bool result = false;
	
	if (mMediaSource)
	{
		mMediaSource->textInput(wstring_to_utf8str(LLWString(1, uni_char)));
	}
	
	return result;
}

//////////////////////////////////////////////////////////////////////////////////////////
bool LLViewerMediaImpl::canNavigateForward()
{
	BOOL result = FALSE;
	if (mMediaSource)
	{
		result = mMediaSource->getHistoryForwardAvailable();
	}
	return result;
}

//////////////////////////////////////////////////////////////////////////////////////////
bool LLViewerMediaImpl::canNavigateBack()
{
	BOOL result = FALSE;
	if (mMediaSource)
	{
		result = mMediaSource->getHistoryBackAvailable();
	}
	return result;
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::update()
{
	if(mMediaSource == NULL && !mMediaSourceFailedInit)
	{
		if(mPriority != LLPluginClassMedia::PRIORITY_UNLOADED)
		{
			// This media may need to be loaded.
			if(sMediaCreateTimer.hasExpired())
			{
				LL_DEBUGS("PluginPriority") << this << ": creating media based on timer expiration" << LL_ENDL;
				createMediaSource();
				sMediaCreateTimer.setTimerExpirySec(LLVIEWERMEDIA_CREATE_DELAY);
			}
			else
			{
				LL_DEBUGS("PluginPriority") << this << ": NOT creating media (waiting on timer)" << LL_ENDL;
			}
		}
	}
	
	if(mMediaSource == NULL)
	{
		return;
	}
	
	mMediaSource->idle();
	
	if(mMediaSource->isPluginExited())
	{
		destroyMediaSource();
		return;
	}

	if(!mMediaSource->textureValid())
	{
		return;
	}
	
	if(mSuspendUpdates || !mVisible)
	{
		return;
	}
	
	LLViewerMediaTexture* placeholder_image = updatePlaceholderImage();
		
	if(placeholder_image)
	{
		LLRect dirty_rect;
		
		// Since we're updating this texture, we know it's playing.  Tell the texture to do its replacement magic so it gets rendered.
		placeholder_image->setPlaying(TRUE);

		if(mMediaSource->getDirty(&dirty_rect))
		{
			// Constrain the dirty rect to be inside the texture
			S32 x_pos = llmax(dirty_rect.mLeft, 0);
			S32 y_pos = llmax(dirty_rect.mBottom, 0);
			S32 width = llmin(dirty_rect.mRight, placeholder_image->getWidth()) - x_pos;
			S32 height = llmin(dirty_rect.mTop, placeholder_image->getHeight()) - y_pos;
			
			if(width > 0 && height > 0)
			{

				U8* data = mMediaSource->getBitsData();

				// Offset the pixels pointer to match x_pos and y_pos
				data += ( x_pos * mMediaSource->getTextureDepth() * mMediaSource->getBitsWidth() );
				data += ( y_pos * mMediaSource->getTextureDepth() );
				
				placeholder_image->setSubImage(
						data, 
						mMediaSource->getBitsWidth(), 
						mMediaSource->getBitsHeight(),
						x_pos, 
						y_pos, 
						width, 
						height);

			}
			
			mMediaSource->resetDirty();
		}
	}
}


//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::updateImagesMediaStreams()
{
}


//////////////////////////////////////////////////////////////////////////////////////////
LLViewerMediaTexture* LLViewerMediaImpl::updatePlaceholderImage()
{
	if(mTextureId.isNull())
	{
		// The code that created this instance will read from the plugin's bits.
		return NULL;
	}
	
	LLViewerMediaTexture* placeholder_image = LLViewerTextureManager::getMediaTexture( mTextureId );
	
	if (mNeedsNewTexture 
		|| placeholder_image->getUseMipMaps()
		|| placeholder_image->getWidth() != mMediaSource->getTextureWidth()
		|| placeholder_image->getHeight() != mMediaSource->getTextureHeight())
	{
		LL_DEBUGS("Media") << "initializing media placeholder" << LL_ENDL;
		LL_DEBUGS("Media") << "movie image id " << mTextureId << LL_ENDL;

		int texture_width = mMediaSource->getTextureWidth();
		int texture_height = mMediaSource->getTextureHeight();
		int texture_depth = mMediaSource->getTextureDepth();
		
		// MEDIAOPT: check to see if size actually changed before doing work
		placeholder_image->destroyGLTexture();
		// MEDIAOPT: apparently just calling setUseMipMaps(FALSE) doesn't work?
		placeholder_image->reinit(FALSE);	// probably not needed

		// MEDIAOPT: seems insane that we actually have to make an imageraw then
		// immediately discard it
		LLPointer<LLImageRaw> raw = new LLImageRaw(texture_width, texture_height, texture_depth);
		raw->clear(0x0f, 0x0f, 0x0f, 0xff);
		int discard_level = 0;

		// ask media source for correct GL image format constants
		placeholder_image->setExplicitFormat(mMediaSource->getTextureFormatInternal(),
											 mMediaSource->getTextureFormatPrimary(),
											 mMediaSource->getTextureFormatType(),
											 mMediaSource->getTextureFormatSwapBytes());

		placeholder_image->createGLTexture(discard_level, raw);

		// MEDIAOPT: set this dynamically on play/stop
		// FIXME
//		placeholder_image->mIsMediaTexture = true;
		mNeedsNewTexture = false;
	}
	
	return placeholder_image;
}


//////////////////////////////////////////////////////////////////////////////////////////
LLUUID LLViewerMediaImpl::getMediaTextureID()
{
	return mTextureId;
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::setVisible(bool visible)
{
	mVisible = visible;
	
	if(mVisible)
	{
		if(mMediaSource && mMediaSource->isPluginExited())
		{
			destroyMediaSource();
		}
		
		if(!mMediaSource)
		{
			createMediaSource();
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::mouseCapture()
{
	gFocusMgr.setMouseCapture(this);
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::scaleMouse(S32 *mouse_x, S32 *mouse_y)
{
#if 0
	S32 media_width, media_height;
	S32 texture_width, texture_height;
	getMediaSize( &media_width, &media_height );
	getTextureSize( &texture_width, &texture_height );
	S32 y_delta = texture_height - media_height;

	*mouse_y -= y_delta;
#endif
}

//////////////////////////////////////////////////////////////////////////////////////////
bool LLViewerMediaImpl::isMediaPlaying()
{
	bool result = false;
	
	if(mMediaSource)
	{
		EMediaStatus status = mMediaSource->getStatus();
		if(status == MEDIA_PLAYING || status == MEDIA_LOADING)
			result = true;
	}
	
	return result;
}
//////////////////////////////////////////////////////////////////////////////////////////
bool LLViewerMediaImpl::isMediaPaused()
{
	bool result = false;

	if(mMediaSource)
	{
		if(mMediaSource->getStatus() == MEDIA_PAUSED)
			result = true;
	}
	
	return result;
}

//////////////////////////////////////////////////////////////////////////////////////////
//
bool LLViewerMediaImpl::hasMedia()
{
	return mMediaSource != NULL;
}

//////////////////////////////////////////////////////////////////////////////////////////
void LLViewerMediaImpl::handleMediaEvent(LLPluginClassMedia* plugin, LLPluginClassMediaOwner::EMediaEvent event)
{
	switch(event)
	{
		case MEDIA_EVENT_PLUGIN_FAILED_LAUNCH:
		{
			// The plugin failed to load properly.  Make sure the timer doesn't retry.
			mMediaSourceFailedInit = true;
			
			// TODO: may want a different message for this case?
			LLSD args;
			args["PLUGIN"] = LLMIMETypes::implType(mMimeType);
			LLNotifications::instance().add("MediaPluginFailed", args);
		}
		break;

		case MEDIA_EVENT_PLUGIN_FAILED:
		{
			LLSD args;
			args["PLUGIN"] = LLMIMETypes::implType(mMimeType);
			// SJB: This is getting called every frame if the plugin fails to load, continuously respawining the alert!
			//LLNotifications::instance().add("MediaPluginFailed", args);
		}
		break;
		
		case MEDIA_EVENT_CURSOR_CHANGED:
		{
			LL_DEBUGS("Media") <<  "Media event:  MEDIA_EVENT_CURSOR_CHANGED, new cursor is " << plugin->getCursorName() << LL_ENDL;

			std::string cursor = plugin->getCursorName();
			
			if(cursor == "arrow")
				mLastSetCursor = UI_CURSOR_ARROW;
			else if(cursor == "ibeam")
				mLastSetCursor = UI_CURSOR_IBEAM;
			else if(cursor == "splith")
				mLastSetCursor = UI_CURSOR_SIZEWE;
			else if(cursor == "splitv")
				mLastSetCursor = UI_CURSOR_SIZENS;
			else if(cursor == "hand")
				mLastSetCursor = UI_CURSOR_HAND;
			else // for anything else, default to the arrow
				mLastSetCursor = UI_CURSOR_ARROW;
		}
		break;

		case LLViewerMediaObserver::MEDIA_EVENT_NAVIGATE_BEGIN:
		{
			LL_DEBUGS("Media") << "MEDIA_EVENT_NAVIGATE_BEGIN, uri is: " << plugin->getNavigateURI() << LL_ENDL;

			if(getNavState() == MEDIANAVSTATE_SERVER_SENT)
			{
				setNavState(MEDIANAVSTATE_SERVER_BEGUN);
			}
			else
			{
				setNavState(MEDIANAVSTATE_BEGUN);
			}
		}
		break;

		case LLViewerMediaObserver::MEDIA_EVENT_NAVIGATE_COMPLETE:
		{
			LL_DEBUGS("Media") << "MEDIA_EVENT_NAVIGATE_COMPLETE, uri is: " << plugin->getNavigateURI() << LL_ENDL;

			if(getNavState() == MEDIANAVSTATE_BEGUN)
			{
				setNavState(MEDIANAVSTATE_COMPLETE_BEFORE_LOCATION_CHANGED);
			}
			else if(getNavState() == MEDIANAVSTATE_SERVER_BEGUN)
			{
				setNavState(MEDIANAVSTATE_SERVER_COMPLETE_BEFORE_LOCATION_CHANGED);
			}
			else
			{
				// all other cases need to leave the state alone.
			}
		}
		break;
		
		case LLViewerMediaObserver::MEDIA_EVENT_LOCATION_CHANGED:
		{
			LL_DEBUGS("Media") << "MEDIA_EVENT_LOCATION_CHANGED, uri is: " << plugin->getLocation() << LL_ENDL;

			if(getNavState() == MEDIANAVSTATE_BEGUN)
			{
				setNavState(MEDIANAVSTATE_FIRST_LOCATION_CHANGED);
			}
			else if(getNavState() == MEDIANAVSTATE_SERVER_BEGUN)
			{
				setNavState(MEDIANAVSTATE_SERVER_FIRST_LOCATION_CHANGED);
			}
			else
			{
				// Don't track redirects.
				setNavState(MEDIANAVSTATE_NONE);
			}
		}
		break;

		
		default:
		break;
	}

	// Just chain the event to observers.
	emitEvent(plugin, event);
}

////////////////////////////////////////////////////////////////////////////////
// virtual
void
LLViewerMediaImpl::cut()
{
	if (mMediaSource)
		mMediaSource->cut();
}

////////////////////////////////////////////////////////////////////////////////
// virtual
BOOL
LLViewerMediaImpl::canCut() const
{
	if (mMediaSource)
		return mMediaSource->canCut();
	else
		return FALSE;
}

////////////////////////////////////////////////////////////////////////////////
// virtual
void
LLViewerMediaImpl::copy()
{
	if (mMediaSource)
		mMediaSource->copy();
}

////////////////////////////////////////////////////////////////////////////////
// virtual
BOOL
LLViewerMediaImpl::canCopy() const
{
	if (mMediaSource)
		return mMediaSource->canCopy();
	else
		return FALSE;
}

////////////////////////////////////////////////////////////////////////////////
// virtual
void
LLViewerMediaImpl::paste()
{
	if (mMediaSource)
		mMediaSource->paste();
}

////////////////////////////////////////////////////////////////////////////////
// virtual
BOOL
LLViewerMediaImpl::canPaste() const
{
	if (mMediaSource)
		return mMediaSource->canPaste();
	else
		return FALSE;
}

void LLViewerMediaImpl::setUpdated(BOOL updated)
{
	mIsUpdated = updated ;
}

BOOL LLViewerMediaImpl::isUpdated()
{
	return mIsUpdated ;
}

void LLViewerMediaImpl::calculateInterest()
{
	LLViewerMediaTexture* texture = LLViewerTextureManager::findMediaTexture( mTextureId );
	
	if(texture != NULL)
	{
		mInterest = texture->getMaxVirtualSize();
	}
	else
	{
		// I don't think this case should ever be hit.
		LL_WARNS("Plugin") << "no texture!" << LL_ENDL;
		mInterest = 0.0f;
	}
}

F64 LLViewerMediaImpl::getApproximateTextureInterest()
{
	F64 result = 0.0f;
	
	if(mMediaSource)
	{
		result = mMediaSource->getFullWidth();
		result *= mMediaSource->getFullHeight();
	}
	
	return result;
}

void LLViewerMediaImpl::setUsedInUI(bool used_in_ui)
{
	mUsedInUI = used_in_ui; 
	
	// HACK: Force elements used in UI to load right away.
	// This fixes some issues where UI code that uses the browser instance doesn't expect it to be unloaded.
	if(mUsedInUI && (mPriority == LLPluginClassMedia::PRIORITY_UNLOADED))
	{
		if(getVisible())
		{
			mPriority = LLPluginClassMedia::PRIORITY_NORMAL;
		}
		else
		{
			mPriority = LLPluginClassMedia::PRIORITY_HIDDEN;
		}

		createMediaSource();
	}
};

F64 LLViewerMediaImpl::getCPUUsage() const
{
	F64 result = 0.0f;
	
	if(mMediaSource)
	{
		result = mMediaSource->getCPUUsage();
	}
	
	return result;
}

void LLViewerMediaImpl::setPriority(LLPluginClassMedia::EPriority priority)
{
	mPriority = priority;
	
	if(priority == LLPluginClassMedia::PRIORITY_UNLOADED)
	{
		if(mMediaSource)
		{
			// Need to unload the media source
			destroyMediaSource();
		}
	}

	if(mMediaSource)
	{
		mMediaSource->setPriority(mPriority);
	}
	
	// NOTE: loading (or reloading) media sources whose priority has risen above PRIORITY_UNLOADED is done in update().
}

void LLViewerMediaImpl::setLowPrioritySizeLimit(int size)
{
	if(mMediaSource)
	{
		mMediaSource->setLowPrioritySizeLimit(size);
	}
}

void LLViewerMediaImpl::setNavState(EMediaNavState state)
{
	mMediaNavState = state;
	
	switch (state) 
	{
		case MEDIANAVSTATE_NONE: LL_DEBUGS("Media") << "Setting nav state to MEDIANAVSTATE_NONE" << llendl; break;
		case MEDIANAVSTATE_BEGUN: LL_DEBUGS("Media") << "Setting nav state to MEDIANAVSTATE_BEGUN" << llendl; break;
		case MEDIANAVSTATE_FIRST_LOCATION_CHANGED: LL_DEBUGS("Media") << "Setting nav state to MEDIANAVSTATE_FIRST_LOCATION_CHANGED" << llendl; break;
		case MEDIANAVSTATE_COMPLETE_BEFORE_LOCATION_CHANGED: LL_DEBUGS("Media") << "Setting nav state to MEDIANAVSTATE_COMPLETE_BEFORE_LOCATION_CHANGED" << llendl; break;
		case MEDIANAVSTATE_SERVER_SENT: LL_DEBUGS("Media") << "Setting nav state to MEDIANAVSTATE_SERVER_SENT" << llendl; break;
		case MEDIANAVSTATE_SERVER_BEGUN: LL_DEBUGS("Media") << "Setting nav state to MEDIANAVSTATE_SERVER_BEGUN" << llendl; break;
		case MEDIANAVSTATE_SERVER_FIRST_LOCATION_CHANGED: LL_DEBUGS("Media") << "Setting nav state to MEDIANAVSTATE_SERVER_FIRST_LOCATION_CHANGED" << llendl; break;
		case MEDIANAVSTATE_SERVER_COMPLETE_BEFORE_LOCATION_CHANGED: LL_DEBUGS("Media") << "Setting nav state to MEDIANAVSTATE_SERVER_COMPLETE_BEFORE_LOCATION_CHANGED" << llendl; break;
	}
}


void LLViewerMediaImpl::addObject(LLVOVolume* obj) 
{
	std::list< LLVOVolume* >::iterator iter = mObjectList.begin() ;
	for(; iter != mObjectList.end() ; ++iter)
	{
		if(*iter == obj)
		{
			return ; //already in the list.
		}
	}

	mObjectList.push_back(obj) ;
}
	
void LLViewerMediaImpl::removeObject(LLVOVolume* obj) 
{
	mObjectList.remove(obj) ;	
}
	
const std::list< LLVOVolume* >* LLViewerMediaImpl::getObjectList() const 
{
	return &mObjectList ;
}

//////////////////////////////////////////////////////////////////////////////////////////
//static
void LLViewerMedia::toggleMusicPlay(void*)
{
// FIXME: This probably doesn't belong here
#if 0
	if (mMusicState != PLAYING)
	{
		mMusicState = PLAYING; // desired state
		if (gAudiop)
		{
			LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
			if ( parcel )
			{
				gAudiop->startInternetStream(parcel->getMusicURL());
			}
		}
	}
	else
	{
		mMusicState = STOPPED; // desired state
		if (gAudiop)
		{
			gAudiop->stopInternetStream();
		}
	}
#endif
}

//////////////////////////////////////////////////////////////////////////////////////////
//static
void LLViewerMedia::toggleMediaPlay(void*)
{
// FIXME: This probably doesn't belong here
#if 0
	if (LLViewerMedia::isMediaPaused())
	{
		LLViewerParcelMedia::start();
	}
	else if(LLViewerMedia::isMediaPlaying())
	{
		LLViewerParcelMedia::pause();
	}
	else
	{
		LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
		if (parcel)
		{
			LLViewerParcelMedia::play(parcel);
		}
	}
#endif
}

//////////////////////////////////////////////////////////////////////////////////////////
//static
void LLViewerMedia::mediaStop(void*)
{
// FIXME: This probably doesn't belong here
#if 0
	LLViewerParcelMedia::stop();
#endif
}

//////////////////////////////////////////////////////////////////////////////////////////
//static 
bool LLViewerMedia::isMusicPlaying()
{	
// FIXME: This probably doesn't belong here
// FIXME: make this work
	return false;	
}

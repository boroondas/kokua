/** 
 * @file llhttpassetstorage.cpp
 * @brief Subclass capable of loading asset data to/from an external
 * source. Currently, a web server accessed via curl
 *
 * Copyright (c) 2003-$CurrentYear$, Linden Research, Inc.
 * $License$
 */

#include "linden_common.h"

#include "llhttpassetstorage.h"

#include "indra_constants.h"
#include "llvfile.h"
#include "llvfs.h"

#include "zlib/zlib.h"

const F32 MAX_PROCESSING_TIME = 0.005f;
const S32 CURL_XFER_BUFFER_SIZE = 65536;
// Try for 30 minutes for now.
const F32 GET_URL_TO_FILE_TIMEOUT = 1800.0f;

const S32 COMPRESSED_INPUT_BUFFER_SIZE = 4096;

const S32 HTTP_OK = 200;
const S32 HTTP_PUT_OK = 201;
const S32 HTTP_NO_CONTENT = 204;
const S32 HTTP_MISSING = 404;
const S32 HTTP_SERVER_BAD_GATEWAY = 502;
const S32 HTTP_SERVER_TEMP_UNAVAILABLE = 503;

/////////////////////////////////////////////////////////////////////////////////
// LLTempAssetData
// An asset not stored on central asset store, but on a simulator node somewhere.
/////////////////////////////////////////////////////////////////////////////////
struct LLTempAssetData
{
	LLUUID	mAssetID;
	LLUUID	mAgentID;
	std::string	mHostName;
};

/////////////////////////////////////////////////////////////////////////////////
// LLHTTPAssetRequest
/////////////////////////////////////////////////////////////////////////////////

class LLHTTPAssetRequest : public LLAssetRequest
{
public:
	LLHTTPAssetRequest(LLHTTPAssetStorage *asp, const LLUUID &uuid, LLAssetType::EType type, const char *url, CURLM *curl_multi);
	virtual ~LLHTTPAssetRequest();
	
	void setupCurlHandle();

	void   	prepareCompressedUpload();
	void	finishCompressedUpload();
	size_t	readCompressedData(void* data, size_t size);

	static size_t curlCompressedUploadCallback(
					void *data, size_t size, size_t nmemb, void *user_data);

public:
	LLHTTPAssetStorage *mAssetStoragep;

	CURL  *mCurlHandle;
	CURLM *mCurlMultiHandle;
	char  *mURLBuffer;
	struct curl_slist *mHTTPHeaders;
	LLVFile *mVFile;
	LLUUID  mTmpUUID;
	BOOL    mIsUpload;
	BOOL	mIsLocalUpload;
	BOOL	mIsDownload;

	bool		mZInitialized;
	z_stream	mZStream;
	char*		mZInputBuffer;
	bool		mZInputExhausted;

	FILE *mFP;
};


LLHTTPAssetRequest::LLHTTPAssetRequest(LLHTTPAssetStorage *asp, const LLUUID &uuid, LLAssetType::EType type, const char *url, CURLM *curl_multi)
	: LLAssetRequest(uuid, type),
	mZInitialized(false)
{
	mAssetStoragep = asp;
	mCurlHandle = NULL;
	mCurlMultiHandle = curl_multi;
	mVFile = NULL;
	mIsUpload = FALSE;
	mIsLocalUpload = FALSE;
	mIsDownload = FALSE;
	mHTTPHeaders = NULL;
	
	mURLBuffer = new char[strlen(url) + 1]; /*Flawfinder: ignore*/
	if (mURLBuffer)
	{
	    strcpy(mURLBuffer, url);
	}
}

LLHTTPAssetRequest::~LLHTTPAssetRequest()
{
	// Cleanup/cancel the request
	if (mCurlHandle)
	{
		curl_multi_remove_handle(mCurlMultiHandle, mCurlHandle);
		curl_easy_cleanup(mCurlHandle);
		if (mAssetStoragep)
		{
			// Terminating a request.  Thus upload or download is no longer pending.
			if (mIsUpload)
			{
				mAssetStoragep->clearPendingUpload();
			}
			else if (mIsLocalUpload)
			{
				mAssetStoragep->clearPendingLocalUpload();
			}
			else if (mIsDownload)
			{
				mAssetStoragep->clearPendingDownload();
			}
			else
			{
				llerrs << "LLHTTPAssetRequest::~LLHTTPAssetRequest - Destroyed request is not upload OR download, this is bad!" << llendl;
			}
		}
		else
		{
			llerrs << "LLHTTPAssetRequest::~LLHTTPAssetRequest - No asset storage associated with this request!" << llendl;
		}
	}
	if (mHTTPHeaders)
	{
		curl_slist_free_all(mHTTPHeaders);
	}
	delete[] mURLBuffer;
	delete   mVFile;
	finishCompressedUpload();
}

void LLHTTPAssetRequest::setupCurlHandle()
{
	mCurlHandle = curl_easy_init();
	curl_easy_setopt(mCurlHandle, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(mCurlHandle, CURLOPT_NOPROGRESS, 1);
	curl_easy_setopt(mCurlHandle, CURLOPT_URL, mURLBuffer);
	curl_easy_setopt(mCurlHandle, CURLOPT_PRIVATE, this);
	if (mIsDownload)
	{
		curl_easy_setopt(mCurlHandle, CURLOPT_ENCODING, "");
		// only do this on downloads, as uploads 
		// to some apache configs (like our test grids)
		// mistakenly claim the response is gzip'd if the resource
		// name ends in .gz, even though in a PUT, the response is
		// just plain HTML saying "created"
	}
	/* Remove the Pragma: no-cache header that libcurl inserts by default;
	   we want the cached version, if possible. */
	if (mZInitialized)
	{
		curl_easy_setopt(mCurlHandle, CURLOPT_PROXY, "");
			// disable use of proxy, which can't handle chunked transfers
	}
	mHTTPHeaders = curl_slist_append(mHTTPHeaders, "Pragma:");
	// resist the temptation to explicitly add the Transfer-Encoding: chunked
	// header here - invokes a libCURL bug
	curl_easy_setopt(mCurlHandle, CURLOPT_HTTPHEADER, mHTTPHeaders);
	if (mAssetStoragep)
	{
		// Set the appropriate pending upload or download flag
		if (mIsUpload)
		{
			mAssetStoragep->setPendingUpload();
		}
		else if (mIsLocalUpload)
		{
			mAssetStoragep->setPendingLocalUpload();
		}
		else if (mIsDownload)
		{
			mAssetStoragep->setPendingDownload();
		}
		else
		{
			llerrs << "LLHTTPAssetRequest::setupCurlHandle - Request is not upload OR download, this is bad!" << llendl;
		}
	}
	else
	{
		llerrs << "LLHTTPAssetRequest::setupCurlHandle - No asset storage associated with this request!" << llendl;
	}
}

void LLHTTPAssetRequest::prepareCompressedUpload()
{
	mZStream.next_in = Z_NULL;
	mZStream.avail_in = 0;
	mZStream.zalloc = Z_NULL;
	mZStream.zfree = Z_NULL;
	mZStream.opaque = Z_NULL;

	int r = deflateInit2(&mZStream,
			1,			// compression level
			Z_DEFLATED,	// the only method defined
			15 + 16,	// the default windowBits + gzip header flag
			8,			// the default memLevel
			Z_DEFAULT_STRATEGY);

	if (r != Z_OK)
	{
		llerrs << "LLHTTPAssetRequest::prepareCompressedUpload defalateInit2() failed" << llendl;
	}

	mZInitialized = true;
	mZInputBuffer = new char[COMPRESSED_INPUT_BUFFER_SIZE];
	mZInputExhausted = false;

	mVFile = new LLVFile(gAssetStorage->mVFS,
					getUUID(), getType(), LLVFile::READ);
}

void LLHTTPAssetRequest::finishCompressedUpload()
{
	if (mZInitialized)
	{
		llinfos << "LLHTTPAssetRequest::finishCompressedUpload: "
			<< "read " << mZStream.total_in << " byte asset file, "
			<< "uploaded " << mZStream.total_out << " byte compressed asset"
			<< llendl;

		deflateEnd(&mZStream);
		delete[] mZInputBuffer;
	}
}

size_t LLHTTPAssetRequest::readCompressedData(void* data, size_t size)
{
	mZStream.next_out = (Bytef*)data;
	mZStream.avail_out = size;

	while (mZStream.avail_out > 0)
	{
		if (mZStream.avail_in == 0  &&  !mZInputExhausted)
		{
			S32 to_read = llmin(COMPRESSED_INPUT_BUFFER_SIZE,
							(S32)(mVFile->getSize() - mVFile->tell()));
			
			mVFile->read((U8*)mZInputBuffer, to_read); /*Flawfinder: ignore*/

			mZStream.next_in = (Bytef*)mZInputBuffer;
			mZStream.avail_in = mVFile->getLastBytesRead();

			mZInputExhausted = mZStream.avail_in == 0;
		}

		int r = deflate(&mZStream,
					mZInputExhausted ? Z_FINISH : Z_NO_FLUSH);

		if (r == Z_STREAM_END)
		{
			break;
		}
	}

	return size - mZStream.avail_out;
}

//static
size_t LLHTTPAssetRequest::curlCompressedUploadCallback(
		void *data, size_t size, size_t nmemb, void *user_data)
{
	if (!gAssetStorage)
	{
		return 0;
	}
	CURL *curl_handle = (CURL *)user_data;
	LLHTTPAssetRequest *req = NULL;
	curl_easy_getinfo(curl_handle, CURLINFO_PRIVATE, &req);

	return req->readCompressedData(data, size * nmemb);
}

/////////////////////////////////////////////////////////////////////////////////
// LLHTTPAssetStorage
/////////////////////////////////////////////////////////////////////////////////


LLHTTPAssetStorage::LLHTTPAssetStorage(LLMessageSystem *msg, LLXferManager *xfer,
									 LLVFS *vfs, const LLHost &upstream_host,
									 const char *web_host,
									 const char *local_web_host,
									 const char *host_name)
	: LLAssetStorage(msg, xfer, vfs, upstream_host)
{
	_init(web_host, local_web_host, host_name);
}

LLHTTPAssetStorage::LLHTTPAssetStorage(LLMessageSystem *msg, LLXferManager *xfer,
									   LLVFS *vfs,
									   const char *web_host,
									   const char *local_web_host,
									   const char *host_name)
	: LLAssetStorage(msg, xfer, vfs)
{
	_init(web_host, local_web_host, host_name);
}

void LLHTTPAssetStorage::_init(const char *web_host, const char *local_web_host, const char* host_name)
{
	mBaseURL = web_host;
	mLocalBaseURL = local_web_host;
	mHostName = host_name;

	// Do not change this "unless you are familiar with and mean to control 
	// internal operations of libcurl"
	// - http://curl.haxx.se/libcurl/c/curl_global_init.html
	curl_global_init(CURL_GLOBAL_ALL);

	mCurlMultiHandle = curl_multi_init();

	mPendingDownload = FALSE;
	mPendingUpload = FALSE;
	mPendingLocalUpload = FALSE;
}

LLHTTPAssetStorage::~LLHTTPAssetStorage()
{
	curl_multi_cleanup(mCurlMultiHandle);
	mCurlMultiHandle = NULL;
	
	curl_global_cleanup();
}

// storing data is simpler than getting it, so we just overload the whole method
void LLHTTPAssetStorage::storeAssetData(
	const LLUUID& uuid,
	LLAssetType::EType type,
	LLAssetStorage::LLStoreAssetCallback callback,
	void* user_data,
	bool temp_file,
	bool is_priority,
	bool store_local,
	const LLUUID& requesting_agent_id)
{
	if (mVFS->getExists(uuid, type))
	{
		LLAssetRequest *req = new LLAssetRequest(uuid, type);
		req->mUpCallback = callback;
		req->mUserData = user_data;
		req->mRequestingAgentID = requesting_agent_id;

		// this will get picked up and transmitted in checkForTimeouts
		if(store_local)
		{
			mPendingLocalUploads.push_back(req);
		}
		else if(is_priority)
		{
			mPendingUploads.push_front(req);
		}
		else
		{
			mPendingUploads.push_back(req);
		}
	}
	else
	{
		llwarns << "AssetStorage: attempt to upload non-existent vfile " << uuid << ":" << LLAssetType::lookup(type) << llendl;
		if (callback)
		{
			callback(uuid, user_data,  LL_ERR_ASSET_REQUEST_NONEXISTENT_FILE );
		}
	}
}

// virtual
void LLHTTPAssetStorage::storeAssetData(
	const char* filename,
	const LLUUID& asset_id,
	LLAssetType::EType asset_type,
	LLStoreAssetCallback callback,
	void* user_data,
	bool temp_file,
	bool is_priority)
{
	llinfos << "LLAssetStorage::storeAssetData (legacy)" << asset_id << ":" << LLAssetType::lookup(asset_type) << llendl;

	LLLegacyAssetRequest *legacy = new LLLegacyAssetRequest;

	legacy->mUpCallback = callback;
	legacy->mUserData = user_data;

	FILE *fp = LLFile::fopen(filename, "rb"); /*Flawfinder: ignore*/
	if (fp)
	{
		LLVFile file(mVFS, asset_id, asset_type, LLVFile::WRITE);

		fseek(fp, 0, SEEK_END);
		S32 size = ftell(fp);
		fseek(fp, 0, SEEK_SET);

		file.setMaxSize(size);

		const S32 buf_size = 65536;
		U8 copy_buf[buf_size];
		while ((size = (S32)fread(copy_buf, 1, buf_size, fp)))
		{
			file.write(copy_buf, size);
		}
		fclose(fp);

		// if this upload fails, the caller needs to setup a new tempfile for us
		if (temp_file)
		{
			LLFile::remove(filename);
		}
		
		storeAssetData(
			asset_id,
			asset_type,
			legacyStoreDataCallback,
			(void**)legacy,
			temp_file,
			is_priority);
	}
	else
	{
		if (callback)
		{
			callback(LLUUID::null, user_data, LL_ERR_CANNOT_OPEN_FILE);
		}
	}
}

// internal requester, used by getAssetData in superclass
void LLHTTPAssetStorage::_queueDataRequest(const LLUUID& uuid, LLAssetType::EType type,
										  void (*callback)(LLVFS *vfs, const LLUUID&, LLAssetType::EType, void *, S32),
										  void *user_data, BOOL duplicate,
										   BOOL is_priority)
{
	// stash the callback info so we can find it after we get the response message
	LLAssetRequest *req = new LLAssetRequest(uuid, type);
	req->mDownCallback = callback;
	req->mUserData = user_data;
	req->mIsPriority = is_priority;

	// this will get picked up and downloaded in checkForTimeouts

	//
	// HAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACK!  Asset requests were taking too long and timing out.
	// Since texture requests are the LEAST sensitive (on the simulator) to being delayed, add
	// non-texture requests to the front, and add texture requests to the back.  The theory is
	// that we always want them first, even if they're out of order.
	//
	
	if (req->getType() == LLAssetType::AT_TEXTURE)
	{
		mPendingDownloads.push_back(req);
	}
	else
	{
		mPendingDownloads.push_front(req);
	}
}

// overloaded to additionally move data to/from the webserver
void LLHTTPAssetStorage::checkForTimeouts()
{
	LLAssetRequest *req = NULL;
	if (mPendingDownloads.size() > 0  && !mPendingDownload)
	{	
		req = mPendingDownloads.front();
		// Setup this curl download request
		// We need to generate a new request here
		// since the one in the list could go away
		char tmp_url[MAX_STRING]; /*Flawfinder: ignore*/
		char uuid_str[UUID_STR_LENGTH]; /*Flawfinder: ignore*/
		req->getUUID().toString(uuid_str);
		std::string base_url = getBaseURL(req->getUUID(), req->getType());
		snprintf(tmp_url, sizeof(tmp_url), "%s/%36s.%s", base_url.c_str() , uuid_str, LLAssetType::lookup(req->getType())); /*Flawfinder: ignore*/

		LLHTTPAssetRequest *new_req = new LLHTTPAssetRequest(this, req->getUUID(), req->getType(), tmp_url, mCurlMultiHandle);
		new_req->mTmpUUID.generate();
		new_req->mIsDownload = TRUE;

		// Sets pending download flag internally
		new_req->setupCurlHandle();
		curl_easy_setopt(new_req->mCurlHandle, CURLOPT_FOLLOWLOCATION, TRUE);
		curl_easy_setopt(new_req->mCurlHandle, CURLOPT_WRITEFUNCTION, &curlDownCallback);
		curl_easy_setopt(new_req->mCurlHandle, CURLOPT_WRITEDATA, new_req->mCurlHandle);
	
		curl_multi_add_handle(mCurlMultiHandle, new_req->mCurlHandle);
		llinfos << "Requesting " << new_req->mURLBuffer << llendl;

	}

	
	if (mPendingUploads.size() > 0 && !mPendingUpload)
	{
		req = mPendingUploads.front();
		// setup this curl upload request

		bool do_compress = req->getType() == LLAssetType::AT_OBJECT;

		char tmp_url[MAX_STRING];/*Flawfinder: ignore*/
		char uuid_str[UUID_STR_LENGTH];/*Flawfinder: ignore*/
		req->getUUID().toString(uuid_str);
		snprintf(tmp_url, sizeof(tmp_url), 					/*Flawfinder: ignore*/
				do_compress ? "%s/%s.%s.gz" : "%s/%s.%s",
				mBaseURL.c_str(), uuid_str, LLAssetType::lookup(req->getType())); 

		LLHTTPAssetRequest *new_req = new LLHTTPAssetRequest(this, req->getUUID(), req->getType(), tmp_url, mCurlMultiHandle);
		new_req->mIsUpload = TRUE;
		if (do_compress)
		{
			new_req->prepareCompressedUpload();
		}

		// Sets pending upload flag internally
		new_req->setupCurlHandle();
		curl_easy_setopt(new_req->mCurlHandle, CURLOPT_UPLOAD, 1);
		curl_easy_setopt(new_req->mCurlHandle, CURLOPT_WRITEFUNCTION, &nullOutputCallback);

		if (do_compress)
		{
			curl_easy_setopt(new_req->mCurlHandle, CURLOPT_READFUNCTION,
					&LLHTTPAssetRequest::curlCompressedUploadCallback);
		}
		else
		{
			LLVFile file(mVFS, req->getUUID(), req->getType());
			curl_easy_setopt(new_req->mCurlHandle, CURLOPT_INFILESIZE, file.getSize());
			curl_easy_setopt(new_req->mCurlHandle, CURLOPT_READFUNCTION,
					&curlUpCallback);
		}
		curl_easy_setopt(new_req->mCurlHandle, CURLOPT_READDATA, new_req->mCurlHandle);
	
		curl_multi_add_handle(mCurlMultiHandle, new_req->mCurlHandle);
		llinfos << "Requesting PUT " << new_req->mURLBuffer << llendl;
		// Pending upload will have been flagged by the request
	}


	if (mPendingLocalUploads.size() > 0 && !mPendingLocalUpload)
	{
		req = mPendingLocalUploads.front();
		// setup this curl upload request
		LLVFile file(mVFS, req->getUUID(), req->getType());

		char tmp_url[MAX_STRING]; /*Flawfinder: ignore*/
		char uuid_str[UUID_STR_LENGTH]; /*Flawfinder: ignore*/
		req->getUUID().toString(uuid_str);
		
		// KLW - All temporary uploads are saved locally "http://localhost:12041/asset"
		snprintf(tmp_url, sizeof(tmp_url), "%s/%36s.%s", mLocalBaseURL.c_str(), uuid_str, LLAssetType::lookup(req->getType())); /*Flawfinder: ignore*/

		LLHTTPAssetRequest *new_req = new LLHTTPAssetRequest(this, req->getUUID(), req->getType(), tmp_url, mCurlMultiHandle);
		new_req->mIsLocalUpload = TRUE;
		new_req->mRequestingAgentID = req->mRequestingAgentID;

		// Sets pending upload flag internally
		new_req->setupCurlHandle();
		curl_easy_setopt(new_req->mCurlHandle, CURLOPT_PUT, 1);
		curl_easy_setopt(new_req->mCurlHandle, CURLOPT_INFILESIZE, file.getSize());
		curl_easy_setopt(new_req->mCurlHandle, CURLOPT_WRITEFUNCTION, &nullOutputCallback);
		curl_easy_setopt(new_req->mCurlHandle, CURLOPT_READFUNCTION, &curlUpCallback);
		curl_easy_setopt(new_req->mCurlHandle, CURLOPT_READDATA, new_req->mCurlHandle);
	
		curl_multi_add_handle(mCurlMultiHandle, new_req->mCurlHandle);
		llinfos << "TAT: LLHTTPAssetStorage::checkForTimeouts() : pending local!"
			<< " Requesting PUT " << new_req->mURLBuffer << llendl;
		// Pending upload will have been flagged by the request
	}
	S32 count = 0;
	CURLMcode mcode;
	int queue_length;
	do
	{
		mcode = curl_multi_perform(mCurlMultiHandle, &queue_length);
		count++;
	} while (mcode == CURLM_CALL_MULTI_PERFORM && (count < 5));

	CURLMsg *curl_msg;
	do
	{
		curl_msg = curl_multi_info_read(mCurlMultiHandle, &queue_length);
		if (curl_msg && curl_msg->msg == CURLMSG_DONE)
		{
			long curl_result = 0;
			S32 xfer_result = 0;

			LLHTTPAssetRequest *req = NULL;
			curl_easy_getinfo(curl_msg->easy_handle, CURLINFO_PRIVATE, &req);
								
			curl_easy_getinfo(curl_msg->easy_handle, CURLINFO_HTTP_CODE, &curl_result);
			if (req->mIsUpload || req->mIsLocalUpload)
			{
				if (curl_msg->data.result == CURLE_OK && (curl_result == HTTP_OK || curl_result == HTTP_PUT_OK || curl_result == HTTP_NO_CONTENT))
				{
					llinfos << "Success uploading " << req->getUUID() << " to " << req->mURLBuffer << llendl;
					if (req->mIsLocalUpload)
					{
						addTempAssetData(req->getUUID(), req->mRequestingAgentID, mHostName);
					}
				}
				else if (curl_msg->data.result == CURLE_COULDNT_CONNECT ||
						curl_msg->data.result == CURLE_OPERATION_TIMEOUTED ||
						curl_result == HTTP_SERVER_BAD_GATEWAY ||
						curl_result == HTTP_SERVER_TEMP_UNAVAILABLE)
				{
					llwarns << "Re-requesting upload for " << req->getUUID() << ".  Received upload error to " << req->mURLBuffer <<
						" with result " << curl_easy_strerror(curl_msg->data.result) << ", http result " << curl_result << llendl;
				}
				else
				{
					llwarns << "Failure uploading " << req->getUUID() << " to " << req->mURLBuffer <<
						" with result " << curl_easy_strerror(curl_msg->data.result) << ", http result " << curl_result << llendl;

					xfer_result = LL_ERR_ASSET_REQUEST_FAILED;
				}

				if (!(curl_msg->data.result == CURLE_COULDNT_CONNECT ||
						curl_msg->data.result == CURLE_OPERATION_TIMEOUTED ||
						curl_result == HTTP_SERVER_BAD_GATEWAY ||
						curl_result == HTTP_SERVER_TEMP_UNAVAILABLE))
				{
					// shared upload finished callback
					// in the base class, this is called from processUploadComplete
					_callUploadCallbacks(req->getUUID(), req->getType(), (xfer_result == 0));
					// Pending upload flag will get cleared when the request is deleted
				}
			}
			else if (req->mIsDownload)
			{
				if (curl_result == HTTP_OK && curl_msg->data.result == CURLE_OK)
				{
					if (req->mVFile && req->mVFile->getSize() > 0)
					{					
						llinfos << "Success downloading " << req->mURLBuffer << ", size " << req->mVFile->getSize() << llendl;

						req->mVFile->rename(req->getUUID(), req->getType());
					}
					else
					{
						// TODO: if this actually indicates a bad asset on the server
						// (not certain at this point), then delete it
						llwarns << "Found " << req->mURLBuffer << " to be zero size" << llendl;
						xfer_result = LL_ERR_ASSET_REQUEST_NOT_IN_DATABASE;
					}
				}
				else
				{
					// KLW - TAT See if an avatar owns this texture, and if so request re-upload.
					llwarns << "Failure downloading " << req->mURLBuffer << 
						" with result " << curl_easy_strerror(curl_msg->data.result) << ", http result " << curl_result << llendl;

					xfer_result = (curl_result == HTTP_MISSING) ? LL_ERR_ASSET_REQUEST_NOT_IN_DATABASE : LL_ERR_ASSET_REQUEST_FAILED;

					if (req->mVFile)
					{
						req->mVFile->remove();
					}
				}

				// call the static callback for transfer completion
				// this will cleanup all requests for this asset, including ours
				downloadCompleteCallback(
					xfer_result,
					req->getUUID(),
					req->getType(),
					(void *)req);
				// Pending download flag will get cleared when the request is deleted
			}
			else
			{
				// nothing, just axe this request
				// currently this can only mean an asset delete
			}

			// Deleting clears the pending upload/download flag if it's set and the request is transferring
			delete req;
			req = NULL;
		}

	} while (curl_msg && queue_length > 0);
	

	LLAssetStorage::checkForTimeouts();
}

// static
size_t LLHTTPAssetStorage::curlDownCallback(void *data, size_t size, size_t nmemb, void *user_data)
{
	if (!gAssetStorage)
	{
		llwarns << "Missing gAssetStorage, aborting curl download callback!" << llendl;
		return 0;
	}
	S32 bytes = (S32)(size * nmemb);
	CURL *curl_handle = (CURL *)user_data;
	LLHTTPAssetRequest *req = NULL;
	curl_easy_getinfo(curl_handle, CURLINFO_PRIVATE, &req);

	if (! req->mVFile)
	{
		req->mVFile = new LLVFile(gAssetStorage->mVFS, req->mTmpUUID, LLAssetType::AT_NONE, LLVFile::APPEND);
	}

	double content_length = 0.0;
	curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &content_length);

	// sanitize content_length, reconcile w/ actual data
	S32 file_length = llmax(0, (S32)llmin(content_length, 20000000.0), bytes + req->mVFile->getSize());

	req->mVFile->setMaxSize(file_length);
	req->mVFile->write((U8*)data, bytes);

	return nmemb;
}

// static 
size_t LLHTTPAssetStorage::curlUpCallback(void *data, size_t size, size_t nmemb, void *user_data)
{
	if (!gAssetStorage)
	{
		llwarns << "Missing gAssetStorage, aborting curl download callback!" << llendl;
		return 0;
	}
	CURL *curl_handle = (CURL *)user_data;
	LLHTTPAssetRequest *req = NULL;
	curl_easy_getinfo(curl_handle, CURLINFO_PRIVATE, &req);

	if (! req->mVFile)
	{
		req->mVFile = new LLVFile(gAssetStorage->mVFS, req->getUUID(), req->getType(), LLVFile::READ);
	}

	S32 bytes = llmin((S32)(size * nmemb), (S32)(req->mVFile->getSize() - req->mVFile->tell()));

	req->mVFile->read((U8*)data, bytes);/*Flawfinder: ignore*/

	return req->mVFile->getLastBytesRead();
}

// static
size_t LLHTTPAssetStorage::nullOutputCallback(void *data, size_t size, size_t nmemb, void *user_data)
{
	// do nothing, this is here to soak up script output so it doesn't end up on stdout

	return nmemb;
}



// blocking asset fetch which bypasses the VFS
// this is a very limited function for use by the simstate loader and other one-offs
S32 LLHTTPAssetStorage::getURLToFile(const LLUUID& uuid, LLAssetType::EType asset_type, const LLString &url, const char *filename, progress_callback callback, void *userdata)
{
	// *NOTE: There is no guarantee that the uuid and the asset_type match
	// - not that it matters. - Doug
	lldebugs << "LLHTTPAssetStorage::getURLToFile() - " << url << llendl;

	FILE *fp = LLFile::fopen(filename, "wb"); /*Flawfinder: ignore*/
	if (! fp)
	{
		llwarns << "Failed to open " << filename << " for writing" << llendl;
		return LL_ERR_ASSET_REQUEST_FAILED;
	}

	// make sure we use the normal curl setup, even though we don't really need a request object
	LLHTTPAssetRequest req(this, uuid, asset_type, url.c_str(), mCurlMultiHandle);
	req.mFP = fp;
	req.mIsDownload = TRUE;
	
	req.setupCurlHandle();
	curl_easy_setopt(req.mCurlHandle, CURLOPT_FOLLOWLOCATION, TRUE);
	curl_easy_setopt(req.mCurlHandle, CURLOPT_WRITEFUNCTION, &curlFileDownCallback);
	curl_easy_setopt(req.mCurlHandle, CURLOPT_WRITEDATA, req.mCurlHandle);

	curl_multi_add_handle(mCurlMultiHandle, req.mCurlHandle);
	llinfos << "Requesting as file " << req.mURLBuffer << llendl;

	// braindead curl loop
	int queue_length;
	CURLMsg *curl_msg;
	LLTimer timeout;
	timeout.setTimerExpirySec(GET_URL_TO_FILE_TIMEOUT);
	bool success = false;
	S32 xfer_result = 0;
	do
	{
		curl_multi_perform(mCurlMultiHandle, &queue_length);
		curl_msg = curl_multi_info_read(mCurlMultiHandle, &queue_length);

		if (callback)
		{
			callback(userdata);
		}

		if ( curl_msg && (CURLMSG_DONE == curl_msg->msg) )
		{
			success = true;
		}
		else if (timeout.hasExpired())
		{
			llwarns << "Request for " << url << " has timed out." << llendl;
			success = false;
			xfer_result = LL_ERR_ASSET_REQUEST_FAILED;
			break;
		}
	} while (!success);

	if (success)
	{
		long curl_result = 0;
		curl_easy_getinfo(curl_msg->easy_handle, CURLINFO_HTTP_CODE, &curl_result);
		
		if (curl_result == HTTP_OK && curl_msg->data.result == CURLE_OK)
		{
			S32 size = ftell(req.mFP);
			if (size > 0)
			{
				// everything seems to be in order
				llinfos << "Success downloading " << req.mURLBuffer << " to file, size " << size << llendl;
			}
			else
			{
				llwarns << "Found " << req.mURLBuffer << " to be zero size" << llendl;
				xfer_result = LL_ERR_ASSET_REQUEST_FAILED;
			}
		}
		else
		{
			xfer_result = curl_result == HTTP_MISSING ? LL_ERR_ASSET_REQUEST_NOT_IN_DATABASE : LL_ERR_ASSET_REQUEST_FAILED;
			llinfos << "Failure downloading " << req.mURLBuffer << 
				" with result " << curl_easy_strerror(curl_msg->data.result) << ", http result " << curl_result << llendl;
		}
	}

	fclose(fp);
	if (xfer_result)
	{
		LLFile::remove(filename);
	}
	return xfer_result;
}


// static
size_t LLHTTPAssetStorage::curlFileDownCallback(void *data, size_t size, size_t nmemb, void *user_data)
{	
	CURL *curl_handle = (CURL *)user_data;
	LLHTTPAssetRequest *req = NULL;
	curl_easy_getinfo(curl_handle, CURLINFO_PRIVATE, &req);

	if (! req->mFP)
	{
		llwarns << "Missing mFP, aborting curl file download callback!" << llendl;
		return 0;
	}

	return fwrite(data, size, nmemb, req->mFP);
}

// virtual 
void LLHTTPAssetStorage::addTempAssetData(const LLUUID& asset_id, const LLUUID& agent_id, const std::string& host_name)
{
	if (agent_id.isNull() || asset_id.isNull())
	{
		llwarns << "TAT: addTempAssetData bad id's asset_id: " << asset_id << "  agent_id: " << agent_id << llendl;
		return;
	}

	LLTempAssetData temp_asset_data;
	temp_asset_data.mAssetID = asset_id;
	temp_asset_data.mAgentID = agent_id;
	temp_asset_data.mHostName = host_name;

	mTempAssets[asset_id] = temp_asset_data;
}

// virtual
BOOL LLHTTPAssetStorage::hasTempAssetData(const LLUUID& texture_id) const
{
	uuid_tempdata_map::const_iterator citer = mTempAssets.find(texture_id);
	BOOL found = (citer != mTempAssets.end());
	return found;
}

// virtual
std::string LLHTTPAssetStorage::getTempAssetHostName(const LLUUID& texture_id) const
{
	uuid_tempdata_map::const_iterator citer = mTempAssets.find(texture_id);
	if (citer != mTempAssets.end())
	{
		return citer->second.mHostName;
	}
	else
	{
		return std::string();
	}
}

// virtual 
LLUUID LLHTTPAssetStorage::getTempAssetAgentID(const LLUUID& texture_id) const
{
	uuid_tempdata_map::const_iterator citer = mTempAssets.find(texture_id);
	if (citer != mTempAssets.end())
	{
		return citer->second.mAgentID;
	}
	else
	{
		return LLUUID::null;
	}
}

// virtual 
void LLHTTPAssetStorage::removeTempAssetData(const LLUUID& asset_id)
{
	mTempAssets.erase(asset_id);
}

// virtual 
void LLHTTPAssetStorage::removeTempAssetDataByAgentID(const LLUUID& agent_id)
{
	uuid_tempdata_map::iterator it = mTempAssets.begin();
	uuid_tempdata_map::iterator end = mTempAssets.end();

	while (it != end)
	{
		const LLTempAssetData& asset_data = it->second;
		if (asset_data.mAgentID == agent_id)
		{
			mTempAssets.erase(it++);
		}
		else
		{
			++it;
		}
	}
}

std::string LLHTTPAssetStorage::getBaseURL(const LLUUID& asset_id, LLAssetType::EType asset_type)
{
	if (LLAssetType::AT_TEXTURE == asset_type)
	{
		uuid_tempdata_map::const_iterator citer = mTempAssets.find(asset_id);
		if (citer != mTempAssets.end())
		{
			const std::string& host_name = citer->second.mHostName;
			std::string url = llformat(LOCAL_ASSET_URL_FORMAT, host_name.c_str());
			return url;
		}
	}

	return mBaseURL;
}

void LLHTTPAssetStorage::dumpTempAssetData(const LLUUID& avatar_id) const
{
	uuid_tempdata_map::const_iterator it = mTempAssets.begin();
	uuid_tempdata_map::const_iterator end = mTempAssets.end();
	S32 count = 0;
	for ( ; it != end; ++it)
	{
		const LLTempAssetData& temp_asset_data = it->second;
		if (avatar_id.isNull()
			|| avatar_id == temp_asset_data.mAgentID)
		{
			llinfos << "TAT: dump agent " << temp_asset_data.mAgentID
				<< " texture " << temp_asset_data.mAssetID
				<< " host " << temp_asset_data.mHostName
				<< llendl;
			count++;
		}
	}

	if (avatar_id.isNull())
	{
		llinfos << "TAT: dumped " << count << " entries for all avatars" << llendl;
	}
	else
	{
		llinfos << "TAT: dumped " << count << " entries for avatar " << avatar_id << llendl;
	}
}

void LLHTTPAssetStorage::clearTempAssetData()
{
	llinfos << "TAT: Clearing temp asset data map" << llendl;
	mTempAssets.clear();
}


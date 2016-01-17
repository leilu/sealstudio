/****************************************************************************
 Copyright (c) 2013 cocos2d-x.org
 
 http://www.cocos2d-x.org
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:
 
 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/
#include "AssetsManager.h"


#include <curl/curl.h>
#include <curl/easy.h>
#include <stdio.h>
#include <vector>
#include <thread>

#if (CC_TARGET_PLATFORM != CC_PLATFORM_WIN32) && (CC_TARGET_PLATFORM != CC_PLATFORM_WP8) && (CC_TARGET_PLATFORM != CC_PLATFORM_WINRT)
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#endif

#include "base/CCDirector.h"
#include "base/CCScheduler.h"
#include "base/CCUserDefault.h"
#include "platform/CCFileUtils.h"

#ifdef MINIZIP_FROM_SYSTEM
#include <minizip/unzip.h>
#else // from our embedded sources
#include "unzip.h"
#endif

#include "crypto/CCCrypto.h"
#include "sqlite3.h"

using namespace cocos2d;
using namespace std;

NS_CC_EXT_BEGIN;

#define KEY_OF_VERSION   "current-version-code"
#define KEY_OF_DOWNLOADED_VERSION    "downloaded-version-code"
#define TEMP_PACKAGE_FILE_NAME    "cocos2dx-update-temp-package.zip"
#define BUFFER_SIZE    8192
#define MAX_FILENAME   512

#define LOW_SPEED_LIMIT 1L
#define LOW_SPEED_TIME 5L


// Message type
#define ASSETSMANAGER_MESSAGE_UPDATE_SUCCEED                0
#define ASSETSMANAGER_MESSAGE_RECORD_DOWNLOADED_VERSION     1
#define ASSETSMANAGER_MESSAGE_PROGRESS                      2
#define ASSETSMANAGER_MESSAGE_ERROR                         3

// Some data struct for sending messages

struct ErrorMessage
{
    AssetsManager::ErrorCode code;
    AssetsManager* manager;
};

struct ProgressMessage
{
    int percent;
    AssetsManager* manager;
};

// Implementation of AssetsManager

AssetsManager::AssetsManager(const char* packageUrl/* =nullptr */, const char* versionFileUrl/* =nullptr */, const char* storagePath/* =nullptr */)
:  _storagePath(storagePath)
, _version("")
, _packageUrl(packageUrl)
, _versionFileUrl(versionFileUrl)
, _downloadedVersion("")
, _curl(nullptr)
, _connectionTimeout(0)
, _delegate(nullptr)
, _isDownloading(false)
, _shouldDeleteDelegateWhenExit(false)
{
    _curl = curl_easy_init();
    checkStoragePath();
}

AssetsManager::~AssetsManager()
{
    curl_easy_cleanup(_curl);
    if (_shouldDeleteDelegateWhenExit)
    {
        delete _delegate;
    }
}

void AssetsManager::checkStoragePath()
{
    if (_storagePath.size() > 0 && _storagePath[_storagePath.size() - 1] != '/')
    {
        _storagePath.append("/");
    }
}

// Multiple key names
static std::string keyWithHash( const char* prefix, const std::string& url )
{
    char buf[256];
    sprintf(buf,"%s%zd",prefix,std::hash<std::string>()(url));
    return buf;
}

// hashed version
std::string AssetsManager::keyOfVersion() const
{
    return keyWithHash(KEY_OF_VERSION,_packageUrl);
}

// hashed version
std::string AssetsManager::keyOfDownloadedVersion() const
{
    return keyWithHash(KEY_OF_DOWNLOADED_VERSION,_packageUrl);
}

static size_t getVersionCode(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    string *version = (string*)userdata;
    version->append((char*)ptr, size * nmemb);
    
    return (size * nmemb);
}

bool AssetsManager::checkUpdate()
{
    std::vector<DownloadInfo> downloadIdList = this->getDownloadIdList();
    if (downloadIdList.empty())
    {
        Director::getInstance()->getScheduler()->performFunctionInCocosThread([&, this]{
            if (this->_delegate)
                this->_delegate->onError(ErrorCode::NO_NEW_VERSION);
        });
        CCLOG("there is no new version");
        curl_easy_cleanup(_curl);
        return false;
    }
    
    return true;
}

void AssetsManager::downloadAndUncompress()
{
    std::vector<DownloadInfo> downloadIdList = this->getDownloadIdList();
    if (downloadIdList.empty()) return;

    bool success = true;
    for (DownloadInfo info: downloadIdList) {
        do
        {
            if (! downLoad(info))  {
                success = false;
                break;
            }
            
            // Uncompress zip file.
            if (! uncompress(info))
            {
                Director::getInstance()->getScheduler()->performFunctionInCocosThread([&, this]{
                    if (this->_delegate)
                        this->_delegate->onError(ErrorCode::UNCOMPRESS);
                });
                success = false;
                break;
            }
            
            string zipfileName = _storagePath + info._path + "/" + info._file;
            FileUtils::getInstance()->removeFile(zipfileName);
            this->updateDownloadFlg(info._id);
            
        } while (0);
        
        if (!success) break;
    }
    
    if ( success) {
        if (this->_delegate) this->_delegate->onSuccess();
    }
    _isDownloading = false;
}

void AssetsManager::update()
{
    if (_isDownloading) return;
    
    _isDownloading = true;
    
    auto t = std::thread(&AssetsManager::downloadAndUncompress, this);
    t.detach();
}

bool AssetsManager::uncompress(AssetsManager::DownloadInfo info)
{
    
    // Open the zip file
    string outFileName = _storagePath + info._path + "/" + info._file;
    unzFile zipfile = unzOpen(outFileName.c_str());
    if (! zipfile)
    {
        CCLOG("can not open downloaded zip file %s", outFileName.c_str());
        return false;
    }
    
    // Get info about the zip file
    unz_global_info global_info;
    if (unzGetGlobalInfo(zipfile, &global_info) != UNZ_OK)
    {
        CCLOG("can not read file global info of %s", outFileName.c_str());
        unzClose(zipfile);
        return false;
    }
    
    // Buffer to hold data read from the zip file
    char readBuffer[BUFFER_SIZE];
    
    CCLOG("start uncompressing");
    
    // Loop to extract all files.
    uLong i;
    for (i = 0; i < global_info.number_entry; ++i)
    {
        // Get info about current file.
        unz_file_info fileInfo;
        char fileName[MAX_FILENAME];
        if (unzGetCurrentFileInfo(zipfile,
                                  &fileInfo,
                                  fileName,
                                  MAX_FILENAME,
                                  nullptr,
                                  0,
                                  nullptr,
                                  0) != UNZ_OK)
        {
            CCLOG("can not read file info");
            unzClose(zipfile);
            return false;
        }
        
        const string fullPath = _storagePath + info._path + "/" + fileName;
        
        // Check if this entry is a directory or a file.
        const size_t filenameLength = strlen(fileName);
        if (fileName[filenameLength-1] == '/')
        {
            // Entry is a direcotry, so create it.
            // If the directory exists, it will failed scilently.
            if (!FileUtils::getInstance()->createDirectory(fullPath.c_str()))
            {
                CCLOG("can not create directory %s", fullPath.c_str());
                unzClose(zipfile);
                return false;
            }
        }
        else
        {
            //There are not directory entry in some case.
            //So we need to test whether the file directory exists when uncompressing file entry
            //, if does not exist then create directory
            const string fileNameStr(fileName);
            
            size_t startIndex=0;
            
            size_t index=fileNameStr.find("/",startIndex);
            
            while(index != std::string::npos)
            {
                const string dir=_storagePath + info._path + "/" + fileNameStr.substr(0,index);
                
                FILE *out = fopen(dir.c_str(), "r");
                
                if(!out)
                {
                    if (!FileUtils::getInstance()->createDirectory(dir.c_str()))
                    {
                        CCLOG("can not create directory %s", dir.c_str());
                        unzClose(zipfile);
                        return false;
                    }
                    else
                    {
                        CCLOG("create directory %s",dir.c_str());
                    }
                }
                else
                {
                    fclose(out);
                }
                
                startIndex=index+1;
                
                index=fileNameStr.find("/",startIndex);
                
            }
            
            
            
            // Entry is a file, so extract it.
            
            // Open current file.
            if (unzOpenCurrentFile(zipfile) != UNZ_OK)
            {
                CCLOG("can not open file %s", fileName);
                unzClose(zipfile);
                return false;
            }
            
            // Create a file to store current file.
            FILE *out = fopen(fullPath.c_str(), "wb");
            
            if (! out)
            {
                CCLOG("can not open destination file %s", fullPath.c_str());
                unzCloseCurrentFile(zipfile);
                unzClose(zipfile);
                return false;
            }
            
            // Write current file content to destinate file.
            int error = UNZ_OK;
            do
            {
                error = unzReadCurrentFile(zipfile, readBuffer, BUFFER_SIZE);
                if (error < 0)
                {
                    CCLOG("can not read zip file %s, error code is %d", fileName, error);
                    unzCloseCurrentFile(zipfile);
                    unzClose(zipfile);
                    return false;
                }
                
                if (error > 0)
                {
                    fwrite(readBuffer, error, 1, out);
                }
            } while(error > 0);
            
            fclose(out);
        }
        
        unzCloseCurrentFile(zipfile);
        
        // Goto next entry listed in the zip file.
        if ((i+1) < global_info.number_entry)
        {
            if (unzGoToNextFile(zipfile) != UNZ_OK)
            {
                CCLOG("can not read next file");
                unzClose(zipfile);
                return false;
            }
        }
    }
    
    CCLOG("end uncompressing");
    unzClose(zipfile);
    
    return true;
}

/*
 * Create a direcotry is platform depended.
 */
bool AssetsManager::createDirectory(const char *path)
{
#if (CC_TARGET_PLATFORM == CC_PLATFORM_WINRT) || (CC_TARGET_PLATFORM == CC_PLATFORM_WP8)
    return FileUtils::getInstance()->createDirectory(_storagePath.c_str());
#elif (CC_TARGET_PLATFORM == CC_PLATFORM_WIN32)
    BOOL ret = CreateDirectoryA(path, nullptr);
    if (!ret && ERROR_ALREADY_EXISTS != GetLastError())
    {
        return false;
    }
    return true;
#else
    mode_t processMask = umask(0);
    int ret = mkdir(path, S_IRWXU | S_IRWXG | S_IRWXO);
    umask(processMask);
    if (ret != 0 && (errno != EEXIST))
    {
        return false;
    }

    return true;
#endif


}

void AssetsManager::setSearchPath()
{
    vector<string> searchPaths = FileUtils::getInstance()->getSearchPaths();
    vector<string>::iterator iter = searchPaths.begin();
    searchPaths.insert(iter, _storagePath);
    FileUtils::getInstance()->setSearchPaths(searchPaths);
}

static size_t downLoadPackage(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    FILE *fp = (FILE*)userdata;
    size_t written = fwrite(ptr, size, nmemb, fp);
    return written;
}

int assetsManagerProgressFunc(void *ptr, double totalToDownload, double nowDownloaded, double totalToUpLoad, double nowUpLoaded)
{
    static int percent = 0;
    int tmp = (int)(nowDownloaded / totalToDownload * 100);
    
    if (percent != tmp)
    {
        percent = tmp;
        Director::getInstance()->getScheduler()->performFunctionInCocosThread([=]{
            auto manager = static_cast<AssetsManager*>(ptr);
            if (manager->_delegate)
                manager->_delegate->onProgress(percent);
        });
    }
    
    return 0;
}

bool AssetsManager::downLoad(AssetsManager::DownloadInfo info)
{
    // Create a file to save package.
    FileUtils::getInstance()->createDirectory(_storagePath + info._path);
    const string outFileName = _storagePath + info._path + "/" + info._file;
    FILE *fp = fopen(outFileName.c_str(), "wb");
    if (! fp)
    {
        Director::getInstance()->getScheduler()->performFunctionInCocosThread([&, this]{
            if (this->_delegate)
                this->_delegate->onError(ErrorCode::CREATE_FILE);
        });
        CCLOG("can not create file %s", outFileName.c_str());
        return false;
    }
    
    char downloadUrl[256] ={0};
    
    sprintf(downloadUrl, "%s/%s/%s", _packageUrl.c_str(),info._path.c_str(),info._file.c_str());
    _curl = curl_easy_init();
    // Download pacakge
    CURLcode res;
    curl_easy_setopt(_curl, CURLOPT_URL, downloadUrl);
    curl_easy_setopt(_curl, CURLOPT_WRITEFUNCTION, downLoadPackage);
    curl_easy_setopt(_curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(_curl, CURLOPT_NOPROGRESS, false);
    curl_easy_setopt(_curl, CURLOPT_PROGRESSFUNCTION, assetsManagerProgressFunc);
    curl_easy_setopt(_curl, CURLOPT_PROGRESSDATA, this);
    curl_easy_setopt(_curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(_curl, CURLOPT_LOW_SPEED_LIMIT, LOW_SPEED_LIMIT);
    curl_easy_setopt(_curl, CURLOPT_LOW_SPEED_TIME, LOW_SPEED_TIME);
    curl_easy_setopt(_curl, CURLOPT_FOLLOWLOCATION, 1 );

    res = curl_easy_perform(_curl);
    
    if (res != 0)
    {
        Director::getInstance()->getScheduler()->performFunctionInCocosThread([&, this]{
            if (this->_delegate)
                this->_delegate->onError(ErrorCode::NETWORK);
        });
        CCLOG("error when download package");
        fclose(fp);
        return false;
    }
    
    CCLOG("succeed downloading package %s/%s/%s", _packageUrl.c_str(),info._path.c_str(),info._file.c_str());
    
    fclose(fp);
    
//    FILE *md5Fp = fopen(outFileName.c_str(), "r");
//    char *checkSum = CCCrypto::getFileMd5Hash(md5Fp);
//    
//    if (strcmp(info._checkSum.c_str(), checkSum) != 0) {
//        // 失敗
//        Director::getInstance()->getScheduler()->performFunctionInCocosThread([&, this]{
//            if (this->_delegate)
//                this->_delegate->onError(ErrorCode::NETWORK);
//        });
//        return false;
//    }
//
//    fclose(md5Fp);
    return true;
}

const char* AssetsManager::getPackageUrl() const
{
    return _packageUrl.c_str();
}

void AssetsManager::setPackageUrl(const char *packageUrl)
{
    _packageUrl = packageUrl;
}

const char* AssetsManager::getStoragePath() const
{
    return _storagePath.c_str();
}

void AssetsManager::setStoragePath(const char *storagePath)
{
    _storagePath = storagePath;
    checkStoragePath();
}

const char* AssetsManager::getVersionFileUrl() const
{
    return _versionFileUrl.c_str();
}

void AssetsManager::setVersionFileUrl(const char *versionFileUrl)
{
    _versionFileUrl = versionFileUrl;
}

string AssetsManager::getVersion()
{
    return UserDefault::getInstance()->getStringForKey(keyOfVersion().c_str());
}

void AssetsManager::deleteVersion()
{
    UserDefault::getInstance()->setStringForKey(keyOfVersion().c_str(), "");
}

void AssetsManager::setDelegate(AssetsManagerDelegateProtocol *delegate)
{
    _delegate = delegate;
}

void AssetsManager::setConnectionTimeout(unsigned int timeout)
{
    _connectionTimeout = timeout;
}

unsigned int AssetsManager::getConnectionTimeout()
{
    return _connectionTimeout;
}

AssetsManager* AssetsManager::create(const char* packageUrl, const char* versionFileUrl, const char* storagePath, ErrorCallback errorCallback, ProgressCallback progressCallback, SuccessCallback successCallback )
{
    class DelegateProtocolImpl : public AssetsManagerDelegateProtocol 
    {
    public :
        DelegateProtocolImpl(ErrorCallback aErrorCallback, ProgressCallback aProgressCallback, SuccessCallback aSuccessCallback)
        : errorCallback(aErrorCallback), progressCallback(aProgressCallback), successCallback(aSuccessCallback)
        {}

        virtual void onError(AssetsManager::ErrorCode errorCode) { errorCallback(int(errorCode)); }
        virtual void onProgress(int percent) { progressCallback(percent); }
        virtual void onSuccess() { successCallback(); }

    private :
        ErrorCallback errorCallback;
        ProgressCallback progressCallback;
        SuccessCallback successCallback;
    };

    auto* manager = new (std::nothrow) AssetsManager(packageUrl,versionFileUrl,storagePath);
    auto* delegate = new (std::nothrow) DelegateProtocolImpl(errorCallback,progressCallback,successCallback);
    manager->setDelegate(delegate);
    manager->_shouldDeleteDelegateWhenExit = true;
    manager->autorelease();
    
    return manager;
}

void AssetsManager::createStoragePath()
{
    // Remove downloaded files
#if (CC_TARGET_PLATFORM == CC_PLATFORM_WINRT) || (CC_TARGET_PLATFORM == CC_PLATFORM_WP8)
    FileUtils::getInstance()->createDirectory(_storagePath.c_str());
#elif (CC_TARGET_PLATFORM == CC_PLATFORM_WIN32)
    if ((GetFileAttributesA(_storagePath.c_str())) == INVALID_FILE_ATTRIBUTES)
    {
        CreateDirectoryA(_storagePath.c_str(), 0);
    }
#else
    DIR *dir = nullptr;
    dir = opendir (_storagePath.c_str());
    if (!dir)
    {
        mkdir(_storagePath.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
    }
#endif
}

void AssetsManager::destroyStoragePath()
{
    // Delete recorded version codes.
    deleteVersion();
    
    // Remove downloaded files
#if (CC_TARGET_PLATFORM == CC_PLATFORM_WINRT) || (CC_TARGET_PLATFORM == CC_PLATFORM_WP8)
    FileUtils::getInstance()->removeDirectory(_storagePath.c_str());
#elif (CC_TARGET_PLATFORM == CC_PLATFORM_WIN32)
    string command = "rd /s /q ";
    // Path may include space.
    command += "\"" + _storagePath + "\"";
    system(command.c_str());
#else
    string command = "rm -r ";
    // Path may include space.
    command += "\"" + _storagePath + "\"";
    system(command.c_str());    
#endif
}

std::vector<AssetsManager::DownloadInfo> AssetsManager::getDownloadIdList(){
    
    std::vector<AssetsManager::DownloadInfo> downloadIdList;
    
    char dbPath[256] = {0};
    sprintf(dbPath,"%s%s",FileUtils::getInstance()->getWritablePath().c_str(),"user/user.db");

    sqlite3 *pDB = NULL;
    int err = sqlite3_open(dbPath, &pDB);
    if(err != SQLITE_OK){
        /* TODO:エラー処理 */
        sqlite3_close(pDB);
        return downloadIdList;
    }
    const char *sql = "select * from resource_ver where update_flg = 1";
    sqlite3_stmt *stmt=NULL;
    sqlite3_prepare(pDB, sql, (int)strlen(sql), &stmt, NULL);
    
    // stmtの内部バッファを一旦クリア
//    sqlite3_reset(stmt);
    
    // stmtのSQLを実行し、結果を一列づつ取得
    int r;
    while (SQLITE_ROW == (r=sqlite3_step(stmt))){
        int  id = sqlite3_column_int(stmt, 0);
        const unsigned char *path = sqlite3_column_text(stmt, 1);
        const unsigned char *file = sqlite3_column_text(stmt, 2);
        const unsigned char *checksum = sqlite3_column_text(stmt, 3);
        
        DownloadInfo info;
        info._id = id;
        info._path = std::string((const char*)path);
        info._file = std::string((const char*)file);
        info._checkSum = std::string((const char*)checksum);
        downloadIdList.push_back(info);
    }
    if (SQLITE_DONE!=r){
        // エラー
        sqlite3_close(pDB);
        return downloadIdList;
    }
    
    // stmt を開放
    sqlite3_finalize(stmt);
    sqlite3_close(pDB);
    return downloadIdList;
}

void AssetsManager::updateDownloadFlg(int id){
    
    char *errorMessage = 0;
    char dbPath[256] = {0};
    sprintf(dbPath,"%s%s",FileUtils::getInstance()->getWritablePath().c_str(),"user/user.db");

    sqlite3 *pDB = NULL;
    int err = sqlite3_open(dbPath, &pDB);
    if(err != SQLITE_OK){
        /* TODO:エラー処理 */
        sqlite3_close(pDB);
        return;
    }

    char updateSql[256] = {0};
    sprintf(updateSql,"update resource_ver set update_flg = 2 where id = %d",id);
    int status = sqlite3_exec(pDB, updateSql, nullptr, nullptr, &errorMessage);
    if(status != SQLITE_OK) CCLOG("update: %s", errorMessage);
    sqlite3_close(pDB);
}


NS_CC_EXT_END;

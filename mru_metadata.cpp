/*****************************************************************************
 *
 * Copyright (c) 2017, Oleg `Kanedias` Chernovskiy
 *
 * This file is part of MARC-FS.
 *
 * MARC-FS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MARC-FS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MARC-FS.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mru_metadata.h"

#include "marc_api_cloudfile.h"

#include "marc_file_node.h"
#include "marc_dir_node.h"
#include "marc_dummy_node.h"

using namespace std;

template<typename T>
T* MruData::getNode(string path) {
    RwLock lock(cacheLock);
    auto it = cache.find(path);
    if (it != cache.end()) {
        T *node = dynamic_cast<T*>(it->second.get());
        if (!node)
            throw invalid_argument("Expected another type in node " + path);

        return node;
    }

    return nullptr;
}

// instantiations
template MarcNode* MruData::getNode(string path);
template MarcFileNode* MruData::getNode(string path);
template MarcDirNode* MruData::getNode(string path);
template MarcDummyNode* MruData::getNode(string path);

template<typename T>
void MruData::create(string path) {
    lock_guard<RwMutex> lock(cacheLock);
    auto it = cache.find(path);
    if (it != cache.end() && it->second->exists()) // something is already present in cache at this path!
        throw invalid_argument("Cache already contains node at path " + path);

    cache[path] = make_unique<T>();
}

// instantiations
template void MruData::create<MarcFileNode>(string path);
template void MruData::create<MarcDirNode>(string path);
template void MruData::create<MarcDummyNode>(string path);

void MruData::putCacheStat(string path, const CloudFile *cf) {
    lock_guard<RwMutex> lock(cacheLock);

    auto it = cache.find(path);
    if (it != cache.end()) // altering cache where it's already present, skip
        return; // may happen in getattr

    if (!cf) {
        // mark non-existing
        cache[path] = make_unique<MarcDummyNode>();
        return;
    }

    MarcNode *node;
    switch (cf->getType()) {
        case CloudFile::File: {
            auto file = new MarcFileNode;
            file->setSize(cf->getSize());
            file->setMtime(static_cast<time_t>(cf->getMtime()));
            node = file;
            break;
        }
        case CloudFile::Directory: {
            node = new MarcDirNode;
            break;
        }
    }
    cache[path] = unique_ptr<MarcNode>(node);
}

int MruData::purgeCache(string path) {
    lock_guard<RwMutex> lock(cacheLock);
    auto it = cache.find(path);

    // node doesn't exist
    if (it == cache.end())
        return 0;

    MarcFileNode* node = dynamic_cast<MarcFileNode*>(it->second.get());
    if (node && node->isOpen()) {
        // we can't delete a file if it's open, should wait when it's closed
        // this should actually never happen
        // fuse will handle this situation before us (hiding file)
        return -EINPROGRESS;
    }

    cache.erase(it);
    return 0;
}

void MruData::renameCache(string oldPath, string newPath)
{
    lock_guard<RwMutex> lock(cacheLock);
    cache[newPath].swap(cache[oldPath]);
    cache[oldPath].reset(new MarcDummyNode);

    // FIXME: moving dirs moves all their content with them!
}

bool MruData::tryFillDir(string path, void *dirhandle, fuse_fill_dir_t filler)
{
    // we store cache in sorted map, this means we can find dir contents
    // by finding itself and promoting iterator further

    // append slash if it's not present. Root dir is special here:
    // "/folder" -> "folder", "/folder/f2" -> "f2"
    bool isRoot = path == "/";
    string nestedPath = isRoot ? path : path + '/';

    RwLock lock(cacheLock);
    auto it = cache.find(nestedPath);
    if (it == cache.end())
        return false; // not found in cache

    // so, we found directory in cache, let's iterate over its contents
    for (; it != cache.end(); ++it) {
        if (it->first.find(nestedPath) != 0) // we are moved to other entries, break
            break;

        if (!it->second->exists())
            continue; // skip negative nodes

        string innerPath = it->first.substr(nestedPath.size());
        if (innerPath.empty())
            continue; // happens when it's root dir or dir itself

        if (innerPath.find('/') != string::npos)
            continue; // skip nested directories

        struct stat stats = {};
        it->second->fillStats(&stats);
        filler(dirhandle, innerPath.data(), &stats, 0);
    }

    return true;
}
